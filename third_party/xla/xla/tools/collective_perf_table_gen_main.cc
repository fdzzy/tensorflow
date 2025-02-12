/* Copyright 2025 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Computes performance table for various collectives configurations.
// Configurations are fixed on a number of nodes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/collective_device_list.h"
#include "xla/hlo/parser/hlo_parser.h"
#include "xla/service/gpu/model/hlo_op_profile.pb.h"
#include "xla/tools/collective_perf_table_gen.h"
#include "xla/tsl/util/command_line_flags.h"
#include "tsl/platform/init_main.h"

namespace {

constexpr absl::string_view kUsageText = R"(
This tool runs specified collectives sizes and types (HLO ops) on given hardware and
saves throughput. Saved throughput is able to produce a derating curve.

Example usage:

CUDA_VISIBLE_DEVICES=0,1,2,3 bazel run --config=cuda -- \
   :collective_perf_table_gen_main \
   --alsologtostderr \
   --num_nodes=2 \
   --task_id=0 \
   --collectives=ALL_REDUCE \
   --tensor_size_bytes_spec='start=1024,stop=2147483648,factor=2' \
   --collective_devices_spec='[1,8]<=[8]' &

CUDA_VISIBLE_DEVICES=4,5,6,7 bazel run --config=cuda -- \
   :collective_perf_table_gen_main \
   --alsologtostderr \
   --num_nodes=2 \
   --task_id=1 \
   --collectives=ALL_REDUCE \
   --tensor_size_bytes_spec='start=1024,stop=2147483648,factor=2' \
   --collective_devices_spec='[1,8]<=[8]'

* Will run two (--num_nodes=2) separate processes, each process will have access
to 4 GPUs.
* Each process gets assigned a unique identifier.
  (--task_id)
* In this case we will run NCCL AllReduce.
  (--collectives)
* For message sizes {1024, 2048, 4096, ..., 2147483648} bytes.
  (--tensor_size_bytes_spec)
* AllReduce will run across all 8 devices.
  (--collective_devices_spec, HloShardingV2 format)
)";

constexpr absl::string_view kDefaultCoordinatorAddress = "127.0.0.1:1234";

using ::xla::IotaReplicaGroupList;
using ::xla::gpu::CollectivePerfTableGen;
using ::xla::gpu::DeviceHloInstructionProfiles;

std::pair<std::string /*key*/, std::string /*value*/> ExtractKV(
    absl::string_view token_it, char elem_delim = '=') {
  std::string token = std::string(token_it);
  size_t delim_pos = token.find_first_of(elem_delim);
  CHECK_NE(delim_pos, std::string::npos);
  CHECK(delim_pos + 1 < token.size());
  std::string key = token.substr(0, delim_pos);
  std::string value = token.substr(delim_pos + 1);
  return {key, value};
}

IotaReplicaGroupList GetCollectiveDeviceList(
    absl::string_view collective_device_list_unparsed) {
  auto collective_device_list =
      xla::ParseCollectiveDeviceListOnly(collective_device_list_unparsed);
  CHECK_OK(collective_device_list);
  CHECK(collective_device_list->iota_replica_group_list().has_value());

  return *collective_device_list->iota_replica_group_list();
}

std::vector<IotaReplicaGroupList> GetCollectiveDeviceLists(
    absl::string_view collective_device_lists_unparsed) {
  std::vector<IotaReplicaGroupList> device_lists;
  for (absl::string_view token :
       absl::StrSplit(collective_device_lists_unparsed, ';')) {
    device_lists.emplace_back(GetCollectiveDeviceList(token));
  }
  CHECK_GT(device_lists.size(), 0);
  return device_lists;
}

std::vector<CollectivePerfTableGen::CollectiveType> ParseCollectives(
    absl::string_view unparsed) {
  std::vector<CollectivePerfTableGen::CollectiveType> types;
  CHECK(!unparsed.empty());
  for (absl::string_view token : absl::StrSplit(unparsed, ',')) {
    if (token == "ALL_REDUCE") {
      types.push_back(CollectivePerfTableGen::CollectiveType::ALL_REDUCE);
      continue;
    }
    if (token == "ALL_GATHER") {
      types.push_back(CollectivePerfTableGen::CollectiveType::ALL_GATHER);
      continue;
    }
  }
  CHECK_GT(types.size(), 0);
  return types;
}

CollectivePerfTableGen::StepSpec ParseStepSpec(absl::string_view unparsed) {
  CollectivePerfTableGen::StepSpec spec;
  for (absl::string_view token : absl::StrSplit(unparsed, ',')) {
    auto [key, value] = ExtractKV(token);
    if (key == "start") {
      CHECK(absl::SimpleAtoi(value, &spec.start));
    } else if (key == "stop") {
      CHECK(absl::SimpleAtoi(value, &spec.stop));
    } else if (key == "factor") {
      CHECK(absl::SimpleAtoi(value, &spec.factor));
    } else if (key == "step") {
      CHECK(absl::SimpleAtoi(value, &spec.step));
    } else {
      LOG(FATAL) << "Cannot parse: " << token;
    }
  }
  return spec;
}

}  // namespace

// TODO(b/390097558): Add an option to generate perf table for collective which
// gets overlap to model resource contention.
int main(int argc, char* argv[]) {
  int32_t num_nodes = 1;
  int32_t task_id = 0;
  std::string collectives_unparsed;
  std::string tensor_size_bytes_spec_unparsed;
  std::string collective_devices_spec_unparsed;
  std::string coordinator_address = std::string(kDefaultCoordinatorAddress);
  std::string output = std::string(CollectivePerfTableGen::Config::kStdout);

  std::vector<tsl::Flag> flag_list = {
      tsl::Flag("num_nodes", &num_nodes,
                "Specifies number of processes across a distributed system."),
      tsl::Flag("task_id", &task_id,
                "Specifies task identifier of this process. Must be unique "
                "across the distributed system you run it on."),
      tsl::Flag("collectives", &collectives_unparsed,
                "Comma separated list of collectives to generate perf table "
                "for. Allowed values: ALL_REDUCE, ALL_GATHER."),
      tsl::Flag("tensor_size_bytes_spec", &tensor_size_bytes_spec_unparsed,
                "Spec for a search sweep over transfer sizes. Format example: "
                "start=1,stop=8,factor=2 generates {1,2,4,8}."),
      tsl::Flag("collective_devices_spec", &collective_devices_spec_unparsed,
                "';' separated list of replica groups specification. It "
                "follows `IotaReplicaGroupList` printing format."),
      tsl::Flag("coordinator_address", &coordinator_address,
                "Coordinator address in host:port format. For example: "
                "127.0.0.1:1234."),
      tsl::Flag(
          "output", &output,
          "Output mode for the program. If set to 'stdout' performance table "
          "will be printed to the standard output. If given a file with .pbtxt "
          "or .pb extension it will append the contents to that file."),
  };

  std::string kUsageString =
      absl::StrCat(kUsageText, "\n\n", tsl::Flags::Usage(argv[0], flag_list));
  if (!tsl::Flags::Parse(&argc, argv, flag_list)) {
    LOG(QFATAL) << kUsageString;
  }
  tsl::port::InitMain(kUsageString.c_str(), &argc, &argv);

  CollectivePerfTableGen::Config cfg;
  cfg.coordinator_address = coordinator_address;
  cfg.num_nodes = num_nodes;
  cfg.task_id = task_id;
  cfg.collective_types = ParseCollectives(collectives_unparsed);
  cfg.tensor_size_bytes_spec = ParseStepSpec(tensor_size_bytes_spec_unparsed);
  cfg.replica_groups_list =
      GetCollectiveDeviceLists(collective_devices_spec_unparsed);
  cfg.output = output;

  std::unique_ptr<CollectivePerfTableGen> gen =
      CollectivePerfTableGen::Create(cfg);
  DeviceHloInstructionProfiles profiles = gen->ComputeTable();
  CHECK_OK(gen->Dump(profiles));
  return 0;
}
