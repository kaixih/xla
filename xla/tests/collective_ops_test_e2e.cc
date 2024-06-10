/* Copyright 2023 The OpenXLA Authors.

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

#include <memory>
#include <utility>
#include <vector>

#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_matchers.h"
#include "xla/literal.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/tests/literal_test_util.h"
#include "xla/tests/test_macros.h"
#include "xla/tests/test_utils.h"

namespace xla {
namespace {

namespace op = ::xla::testing::opcode_matchers;
using ::testing::NotNull;

// Makes a DeviceAssignment device#i to replica_id #i.
DeviceAssignment MakeDeviceAssn(int64_t num_replicas) {
  DeviceAssignment assn(/*replica_count=*/num_replicas,
                        /*computation_count=*/1);
  for (int64_t i = 0; i < num_replicas; ++i) {
    assn(i, 0) = i;
  }
  return assn;
}

class CollectiveOpsTestE2E : public HloTestBase {
 public:
  absl::StatusOr<std::vector<Literal>> ExecuteReplicated(Executable* executable,
                                                         int64_t num_replicas) {
    DeviceAssignment device_assignment = MakeDeviceAssn(num_replicas);
    return HloTestBase::ExecuteReplicated(
        /*executable_provider*/ [&](int64_t) { return executable; },
        /*argument_count_provider*/ [](int64_t) { return 0; },
        /*argument_provider*/ [](int64_t, int64_t) { return nullptr; },
        num_replicas, /*run_hlo_passes=*/false, &device_assignment);
  }
};

// E2E tests for collective ops. These will generally verify some HLO transform
// for collectives (for example, sync -> async conversion) and correct
// execution of the transformed HLO.

// E2E test for async collectives. Tested with both async collective enabled
// and disabled. Verify that async collective is generated when enabled
// in the end-to-end compilation for GPU's and that the execution produces
// correct result.
class AsyncCollectiveOps : public CollectiveOpsTestE2E,
                           public ::testing::WithParamInterface<bool> {
 public:
  AsyncCollectiveOps() : num_devices_(backend().device_count()) {
    VLOG(1) << "Running with " << num_devices_ << " devices";
  }

 protected:
  DebugOptions GetDebugOptionsForTest() override {
    DebugOptions debug_options = HloTestBase::GetDebugOptionsForTest();

    // Enable or disable all async collectives based on test parameter.
    bool enable_async = GetParam();
    if (!enable_async) {
      for (auto option :
           {DebugOptions::NOOP, DebugOptions::ALLREDUCE,
            DebugOptions::ALLGATHER, DebugOptions::REDUCESCATTER,
            DebugOptions::COLLECTIVEBROADCAST, DebugOptions::ALLTOALL,
            DebugOptions::COLLECTIVEPERMUTE}) {
        debug_options.add_xla_gpu_disable_async_collectives(option);
      }
    }
    debug_options.add_xla_disable_hlo_passes(
        "gpu-convert-async-collectives-to-sync");
    return debug_options;
  }

  absl::StatusOr<std::unique_ptr<Executable>> CreateExecutable(
      absl::string_view hlo_string, int64_t num_replicas) {
    HloModuleConfig config =
        GetModuleConfigForTest(/*replica_count=*/num_replicas);

    TF_ASSIGN_OR_RETURN(auto module,
                        ParseAndReturnVerifiedModule(hlo_string, config));
    return HloTestBase::CreateExecutable(std::move(module),
                                         /*run_hlo_passes=*/true);
  }

  bool IsAsync(const HloInstruction* inst) {
    return !inst->backend_config<gpu::GpuBackendConfig>()
                .value()
                .collective_backend_config()
                .is_sync();
  }

  const int64_t num_devices_;
};

XLA_TEST_P(AsyncCollectiveOps, AsyncAllReduce) {
  const absl::string_view kModuleStr = R"(
      HloModule test

      apply_op {
        x = u32[] parameter(0)
        y = u32[] parameter(1)
        ROOT apply_op = u32[] add(x, y)
      }

      ENTRY test_computation {
        id = u32[] replica-id()
        ROOT all-reduce = u32[] all-reduce(id), to_apply=apply_op
      }
    )";

  const int64_t kNumReplicas = 2;
  const bool enable_async_all_reduce = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());

  HloInstruction* all_reduce_start =
      FindInstruction(&executable->module(), HloOpcode::kAllReduceStart);
  HloInstruction* all_reduce_done =
      FindInstruction(&executable->module(), HloOpcode::kAllReduceDone);
  EXPECT_THAT(all_reduce_start, NotNull());
  EXPECT_THAT(all_reduce_done, NotNull());
  EXPECT_EQ(IsAsync(all_reduce_start), enable_async_all_reduce);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  // sum [0, num_devices)
  const uint32_t expected = kNumReplicas * (kNumReplicas - 1) / 2;
  for (int i = 0; i < kNumReplicas; ++i) {
    LiteralTestUtil::ExpectR0Equal<uint32_t>(expected, results[i]);
  }
}

XLA_TEST_P(AsyncCollectiveOps, AsyncAllGather) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[1, 2] broadcast(id), dimensions={}
    a0 = u32[1, 2] constant({{10, 15}})
    a1 = u32[1, 2] add(id2, a0)
    allgather = u32[2, 2] all-gather(a1), dimensions={0}
    ROOT out = u32[4] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_all_gather = GetParam();

  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));

  EXPECT_TRUE(executable->has_module());
  HloInstruction* all_gather_start =
      FindInstruction(&executable->module(), HloOpcode::kAllGatherStart);
  HloInstruction* all_gather_done =
      FindInstruction(&executable->module(), HloOpcode::kAllGatherDone);
  EXPECT_THAT(all_gather_start, NotNull());
  EXPECT_THAT(all_gather_done, NotNull());
  EXPECT_EQ(IsAsync(all_gather_start), enable_async_all_gather);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));

  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, result);
  }
}

XLA_TEST_P(AsyncCollectiveOps, AsyncAllGatherMixedTypes) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[1, 2] broadcast(id), dimensions={}
    a0 = u32[1, 2] constant({{10, 15}})
    a1 = u32[1, 2] add(id2, a0)
    a2 = f32[1, 2] convert(a1)
    allgather = (u32[2, 2], f32[2,2]) all-gather(a1, a2), dimensions={0}
    gte0 = u32[2,2] get-tuple-element(allgather), index=0
    gte1 = f32[2,2] get-tuple-element(allgather), index=1
    out0 = u32[4] reshape(gte0)
    out1 = f32[4] reshape(gte1)
    ROOT out = (u32[4], f32[4]) tuple(out0, out1)
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_all_gather = GetParam();

  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());
  HloInstruction* all_gather_start =
      FindInstruction(&executable->module(), HloOpcode::kAllGatherStart);
  HloInstruction* all_gather_done =
      FindInstruction(&executable->module(), HloOpcode::kAllGatherDone);
  EXPECT_THAT(all_gather_start, NotNull());
  EXPECT_THAT(all_gather_done, NotNull());
  EXPECT_EQ(IsAsync(all_gather_start), enable_async_all_gather);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));

  ASSERT_EQ(results.size(), kNumReplicas);
  for (Literal& result : results) {
    std::vector<Literal> results = result.DecomposeTuple();
    LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, results[0]);
    LiteralTestUtil::ExpectR1Equal<float>({10.0, 15.0, 11.0, 16.0}, results[1]);
  }
}

XLA_TEST_P(AsyncCollectiveOps, AsyncCollectiveBroadcast) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    bcast = u32[2] collective-broadcast(p), replica_groups={{1, 0}}
    ROOT res = copy(bcast)
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_collective_broadcast = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());
  HloInstruction* cb_start =
      FindInstruction(&executable->module(), HloOpcode::kAsyncStart);
  HloInstruction* cb_done =
      FindInstruction(&executable->module(), HloOpcode::kAsyncDone);
  EXPECT_THAT(cb_start, NotNull());
  EXPECT_THAT(cb_done, NotNull());
  EXPECT_EQ(IsAsync(cb_start), enable_async_collective_broadcast);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 11}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 11}, results[1]);
}

XLA_TEST_P(AsyncCollectiveOps, AsyncCollectivePermute) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    permute = u32[2] collective-permute(p), source_target_pairs={{1,0}, {0,1}}
    ROOT copy = u32[2] copy(permute)
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_collective_permute = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());
  HloInstruction* cp_start = FindInstruction(
      &executable->module(), HloOpcode::kCollectivePermuteStart);
  HloInstruction* cp_done =
      FindInstruction(&executable->module(), HloOpcode::kCollectivePermuteDone);
  EXPECT_THAT(cp_start, NotNull());
  EXPECT_THAT(cp_done, NotNull());
  EXPECT_EQ(IsAsync(cp_start), enable_async_collective_permute);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 11}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 10}, results[1]);
}

XLA_TEST_P(AsyncCollectiveOps, AsyncReduceScatter) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  add {
    lhs = u32[] parameter(0)
    rhs = u32[] parameter(1)
    ROOT add = u32[] add(lhs, rhs)
  }

  ENTRY main {
    c0 = u32[8] constant({1, 2, 3, 4, 5, 6, 7, 8})
    c1 = u32[8] constant({10, 11, 12, 13, 14, 15, 16, 17})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[8] select(pb, c0, c1)
    ROOT ars = u32[4] reduce-scatter(data), replica_groups={},
                      dimensions={0}, to_apply=add
  }
  )";

  const int64_t kNumReplicas = 2;
  const bool enable_async_reduce_scatter = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());
  HloInstruction* rs_start =
      FindInstruction(&executable->module(), HloOpcode::kAsyncStart);
  HloInstruction* rs_done =
      FindInstruction(&executable->module(), HloOpcode::kAsyncDone);
  ASSERT_THAT(rs_start, NotNull());
  ASSERT_THAT(rs_done, NotNull());
  HloAsyncInstruction* rs_start_async = Cast<HloAsyncInstruction>(rs_start);
  EXPECT_EQ(rs_start_async->async_wrapped_opcode(), HloOpcode::kReduceScatter);
  EXPECT_EQ(IsAsync(rs_start), enable_async_reduce_scatter);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 13, 15, 17}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({19, 21, 23, 25}, results[1]);
}

XLA_TEST_P(AsyncCollectiveOps, AsyncAllToAllWithSplitDim) {
  const absl::string_view kModuleStr = R"(
  HloModule test

  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    a1 = u32[2] add(id2, a0)
    ROOT a2a = u32[2] all-to-all(u32[2] a1), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_all_to_all = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());

  HloInstruction* a2a_start =
      FindInstruction(&executable->module(), HloOpcode::kAsyncStart);
  HloInstruction* a2a_done =
      FindInstruction(&executable->module(), HloOpcode::kAsyncDone);
  ASSERT_THAT(a2a_start, NotNull());
  ASSERT_THAT(a2a_done, NotNull());
  HloAsyncInstruction* a2a_start_async = Cast<HloAsyncInstruction>(a2a_start);
  EXPECT_EQ(a2a_start_async->async_wrapped_opcode(), HloOpcode::kAllToAll);
  EXPECT_EQ(IsAsync(a2a_start), enable_async_all_to_all);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 11}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({15, 16}, results[1]);
}

XLA_TEST_P(AsyncCollectiveOps, AsyncAllToAllWithoutSplitDim) {
  const absl::string_view kModuleStr = R"(
  HloModule test

  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    a1 = u32[2] add(id2, a0)
    a2 = u32[2] constant({4, 4})
    a3 = u32[2] multiply(a1, a2)
    // r0 : a1 = {10, 15}, a2 = {40, 60)
    // r1 : a1 = {11, 16}, a1 = {44, 64}
    // r0: a2a element 0 = {10, 15}, a2a element 1 = {11, 16}
    // r0: a2a element 0 = {40, 60}, a2a element 1 = {44, 64}
    a2a = (u32[2], u32[2]) all-to-all(u32[2] a1, u32[2] a3), replica_groups={{0,1}}
    gte0 = get-tuple-element(a2a), index=0
    gte1 = get-tuple-element(a2a), index=1
    ROOT x = u32[4] concatenate(gte0, gte1), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 2;
  const bool enable_async_all_to_all = GetParam();
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          CreateExecutable(kModuleStr, kNumReplicas));
  EXPECT_TRUE(executable->has_module());
  HloInstruction* a2a_start =
      FindInstruction(&executable->module(), HloOpcode::kAsyncStart);
  HloInstruction* a2a_done =
      FindInstruction(&executable->module(), HloOpcode::kAsyncDone);
  ASSERT_THAT(a2a_start, NotNull());
  ASSERT_THAT(a2a_done, NotNull());
  HloAsyncInstruction* a2a_start_async = Cast<HloAsyncInstruction>(a2a_start);
  EXPECT_EQ(a2a_start_async->async_wrapped_opcode(), HloOpcode::kAllToAll);
  EXPECT_EQ(IsAsync(a2a_start_async), enable_async_all_to_all);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({40, 60, 44, 64}, results[1]);
}

TEST_P(AsyncCollectiveOps, MatmulReplicated) {
  // collective_permute = f32[16,32]{1,0} collective-permute(x_unscaled),
  // source_target_pairs={{0,1}, {1,2}, {2,3}, {3,0}}
  absl::string_view kModuleReplicatedStr = R"(
    HloModule test

    ENTRY test {
      x_f32 = f32[16,32] parameter(0)
      y_f32 = f32[16,32] parameter(1)
      replica_id = u32[] replica-id()
      addend = f32[] convert(replica_id)
      addend_bcast = f32[16,32] broadcast(addend), dimensions={}
      x_add = f32[16,32] add(addend_bcast, x_f32)
      ROOT dot_a = f32[16,16] dot(x_add, y_f32), lhs_contracting_dims={1}, rhs_contracting_dims={1}
   }
  )";

  absl::string_view kModuleSingleStr = R"(
    HloModule test

    ENTRY test {
      x_f32 = f32[16,32] parameter(0)
      y_f32 = f32[16,32] parameter(1)
      replica_id = u32[] parameter(2)
      addend = f32[] convert(replica_id)
      addend_bcast = f32[16,32] broadcast(addend), dimensions={}
      x_add = f32[16,32] add(addend_bcast, x_f32)
      ROOT dot_a = f32[16,16] dot(x_add, y_f32), lhs_contracting_dims={1}, rhs_contracting_dims={1}
   }
  )";
  const int64_t kNumReplicas = 4;

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  auto opts = GetDebugOptionsForTest();
  opts.set_xla_gpu_enable_cublaslt(GetParam());
  VLOG(0) << "Running with CUBLAS enabled: " << opts.xla_gpu_enable_cublaslt();
  config.set_debug_options(opts);

  TF_ASSERT_OK_AND_ASSIGN(
      auto module, ParseAndReturnVerifiedModule(kModuleReplicatedStr, config));
  DeviceAssignment assn(/*replica_count=*/kNumReplicas,
                        /*computation_count=*/1);
  for (int64_t i = 0; i < kNumReplicas; ++i) {
    assn(i, 0) = i;
  }

  auto fake_arguments = xla::MakeFakeArguments(module.get()).value();
  std::vector<Literal*> fake_ptrs(fake_arguments.size());
  for (int i = 0; i < fake_arguments.size(); i++) {
    fake_ptrs[i] = &fake_arguments[i];
  }
  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          HloTestBase::ExecuteReplicated(
                              std::move(module), fake_ptrs, kNumReplicas, &assn,
                              true /*run_hlo_passes*/, true /*use-threads*/));
  ASSERT_EQ(results.size(), kNumReplicas);

  auto& ref_runner = HloTestBase::reference_runner_;
  TF_ASSERT_OK_AND_ASSIGN(
      auto ref_module, ParseAndReturnVerifiedModule(kModuleSingleStr, config));
  TF_ASSERT_OK_AND_ASSIGN(
      auto ref_exec, ref_runner.CreateExecutable(std::move(ref_module), true));

  ErrorSpec error_spec{1e-5, 1e-5};
  fake_ptrs.push_back(nullptr);
  for (int i = 0; i < kNumReplicas; i++) {
    auto replica_id =
        LiteralUtil::CreateFullWithDescendingLayout<uint32_t>({}, i);
    fake_ptrs.back() = &replica_id;
    TF_ASSERT_OK_AND_ASSIGN(
        auto res, ref_runner.ExecuteWithExecutable(ref_exec.get(), fake_ptrs));
    EXPECT_TRUE(LiteralTestUtil::Near(res, results[i], error_spec));
  }
}

INSTANTIATE_TEST_SUITE_P(AsyncCollectiveOps, AsyncCollectiveOps,
                         ::testing::Bool());

// Tests for HLO level transforms.
TEST_F(CollectiveOpsTestE2E, WhileLoopReduceScatterCodeMotion) {
  const absl::string_view kModuleStr = R"(
  HloModule test

  %add {
    %x = u32[] parameter(0)
    %y = u32[] parameter(1)
    ROOT %add = u32[] add(%x, %y)
  }

  %cond {
    %param = (u32[], u32[2], u32[1]) parameter(0)
    %count = get-tuple-element(%param), index=0
    %limit = u32[] constant(3)
    ROOT %result = pred[] compare(%count, %limit), direction=LT
  }

  %body {
    %param = (u32[], u32[2], u32[1]) parameter(0)

    %count = u32[] get-tuple-element(%param), index=0
    %increment = u32[] constant(1)
    %new_count = u32[] add(%count, %increment)

    // iter0: replica0 = {10, 15}, replica1 = {11, 16}
    // iter1: replica0 = {11, 17}, replica1 = {12, 18}
    // iter2: replica0 = {12, 19}, replica1 = {13, 20}

    %rs_input = u32[2] get-tuple-element(%param), index=1

    // iter0: replica0 = 21, replica1 = 31
    // iter1: replica0 = 23, replica1 = 35
    // iter2: replicq0 = 25, replica1 = 39
    %rs = u32[1] reduce-scatter(%rs_input), replica_groups={{0,1}}, to_apply=%add, dimensions={0}

    // iter0: replica0 = 5, replica1 = 5
    // iter1: replica0 = 26, replica1 = 36
    // iter2: replica0 = 49, replica1 = 70
    %old_accum = u32[1] get-tuple-element(%param), index=2

    // iter0: replica0 = 26, replica1 = 36
    // iter1: replica0 = 49, replica1 = 71
    // iter2: replica0 = 74, replica1 = 110
    %new_accum = u32[1] add(%rs, %old_accum)

    %input_inc = u32[2] constant({1, 2})

    // iter0: replica0 = {11, 17}, replica1 = {12, 18}
    // iter1: replica0 = {12, 19}, replica1 = {13, 20}
    // iter2: replica0 = {13, 21}, replica1 = {14, 22}
    %new_rs_input = u32[2] add(%rs_input, %input_inc)

    ROOT ret = (u32[], u32[2], u32[1]) tuple(%new_count, %new_rs_input, %new_accum)
  }

  ENTRY test_computation {
    // loop that executes 3 times.
    %count = u32[] constant(0)
    %id = u32[] replica-id()
    %id2 = u32[2] broadcast(id), dimensions={}
    %a0 = u32[2] constant({10, 15})
    // replica0: {10, 15}, replica1 : {11, 16}
    %init_rs_input = u32[2] add(id2, a0)
    %init_rs_accum = u32[1] constant({5})
    %while_init = (u32[], u32[2], u32[1]) tuple(%count, %init_rs_input, %init_rs_accum)
    %while_result = (u32[], u32[2], u32[1]) while(%while_init), body=%body, condition=%cond
    ROOT gte = u32[1] get-tuple-element(%while_result), index=2
  }
  )";

  const int64_t kNumReplicas = 2;

  DebugOptions debug_options = GetDebugOptionsForTest();
  debug_options.set_xla_gpu_enable_while_loop_reduce_scatter_code_motion(true);
  HloModuleConfig config;
  config.set_debug_options(debug_options);
  config.set_replica_count(kNumReplicas);
  config.set_num_partitions(1);

  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));
  TF_ASSERT_OK_AND_ASSIGN(
      auto executable,
      CreateExecutable(std::move(module), /*run_hlo_passes=*/true));
  ASSERT_TRUE(executable->has_module());
  HloModule* executable_module = &executable->module();

  // Verify that the reduce-scatter get hoisted out of the while loop.
  const HloInstruction* while_loop =
      FindInstruction(executable_module, HloOpcode::kWhile);
  ASSERT_THAT(while_loop, NotNull());
  const HloInstruction* reduce_scatter =
      FindInstruction(executable_module, HloOpcode::kAsyncStart);
  ASSERT_THAT(reduce_scatter, NotNull());

  const HloAsyncInstruction* rs_async =
      Cast<HloAsyncInstruction>(reduce_scatter);
  EXPECT_EQ(rs_async->async_wrapped_opcode(), HloOpcode::kReduceScatter);

  // Verify that the reduce-scatter has been hoisted out of the while loop and
  // into the entry computation.
  const HloComputation* entry = executable_module->entry_computation();
  EXPECT_EQ(reduce_scatter->parent(), entry);

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({74}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({110}, results[1]);
}

// Verify that all-to-all with split dims is not decomposed to tuples.
TEST_F(CollectiveOpsTestE2E, NoAllToAllDecomposition) {
  const absl::string_view kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2, 2] broadcast(id), dimensions={}
    a0 = u32[2, 2] constant({{10, 15}, {20, 25}})
    a1 = u32[2, 2] add(id2, a0)
    all2all = u32[2, 2] all-to-all(a1), replica_groups={{0,1}}, dimensions={0}
    ROOT out = u32[4] reshape(all2all)
  }
  )";
  const int64_t kNumReplicas = 2;

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      auto executable,
      CreateExecutable(std::move(module), /*run_hlo_passes=*/true));
  ASSERT_TRUE(executable->has_module());
  HloModule* executable_module = &executable->module();

  // Verify that the all-to-all is not decomposed into a tuple all-to-all.
  const HloInstruction* all_to_all =
      FindInstruction(executable_module, HloOpcode::kAllToAll);
  EXPECT_THAT(all_to_all, op::Shape("u32[2, 2]"));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(executable.get(), kNumReplicas));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({20, 25, 21, 26}, results[1]);
}

TEST_F(CollectiveOpsTestE2E, WindowedEinsumE2EAllgatherMultiConsumer) {
  absl::string_view kModuleReplicatedStr = R"(
HloModule pjit__unnamed_wrapped_function_, entry_computation_layout={(bf16[2,16,48]{2,1,0}, bf16[48,192]{1,0}, bf16[48,192]{1,0}, bf16[192,48]{1,0})->bf16[2,16,48]{2,1,0}}, allow_spmd_sharding_propagation_to_parameters={false,false,false,false}, num_partitions=4

ENTRY main.12 {
  Arg_0.1 = bf16[2,16,48]{2,1,0} parameter(0), sharding={devices=[1,4,1]<=[4]}
  Arg_1.2 = bf16[48,192]{1,0} parameter(1), sharding={devices=[1,4]<=[4]}
  dot.5 = bf16[2,16,192]{2,1,0} dot(Arg_0.1, Arg_1.2), lhs_contracting_dims={2}, rhs_contracting_dims={0}
  custom-call.7 = bf16[2,16,192]{2,1,0} custom-call(dot.5), custom_call_target="Sharding", sharding={devices=[1,1,4]<=[4]}
  Arg_2.3 = bf16[48,192]{1,0} parameter(2), sharding={devices=[1,4]<=[4]}
  dot.6 = bf16[2,16,192]{2,1,0} dot(Arg_0.1, Arg_2.3), lhs_contracting_dims={2}, rhs_contracting_dims={0}
  add.8 = bf16[2,16,192]{2,1,0} add(custom-call.7, dot.6)
  Arg_3.4 = bf16[192,48]{1,0} parameter(3), sharding={devices=[4,1]<=[4]}
  dot.9 = bf16[2,16,48]{2,1,0} dot(add.8, Arg_3.4), lhs_contracting_dims={2}, rhs_contracting_dims={0}
  tuple.10 = (bf16[2,16,48]{2,1,0}) tuple(dot.9)
  ROOT get-tuple-element.11 = bf16[2,16,48]{2,1,0} get-tuple-element(tuple.10), index=0, sharding={devices=[1,4,1]<=[4]}
} // main.12
)";

  const int64_t kNumReplicas = 1;
  const int64_t kNumPartitions = 4;

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  auto opts = GetDebugOptionsForTest();
  opts.set_xla_gpu_threshold_for_windowed_einsum_mib(0);
  opts.set_xla_gpu_multi_streamed_windowed_einsum(true);
  opts.set_xla_gpu_graph_min_graph_size(200);
  opts.set_xla_gpu_enable_triton_gemm(false);
  config.set_debug_options(opts);
  config.set_num_partitions(kNumPartitions);
  TF_ASSERT_OK_AND_ASSIGN(
      auto module, ParseAndReturnVerifiedModule(kModuleReplicatedStr, config));
  DeviceAssignment assn(/*replica_count=*/kNumReplicas,
                        /*computation_count=*/kNumPartitions);
  config.set_replica_count(kNumReplicas);
  for (int64_t i = 0; i < kNumPartitions; ++i) {
    assn(0, i) = i;
  }

  auto fake_arguments = xla::MakeFakeArguments(module.get()).value();
  std::vector<Literal*> fake_ptrs(fake_arguments.size());
  for (int i = 0; i < fake_arguments.size(); i++) {
    fake_ptrs[i] = &fake_arguments[i];
  }

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      HloTestBase::ExecuteReplicated(
          std::move(module), fake_ptrs, kNumPartitions, &assn,
          true /*run_hlo_passes*/, true /*use-threads*/));
  ASSERT_EQ(results.size(), kNumPartitions);
  HloModuleConfig ref_config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  auto ref_opts = GetDebugOptionsForTest();
  ref_opts.set_xla_gpu_graph_min_graph_size(200);
  ref_opts.set_xla_gpu_enable_triton_gemm(false);
  ref_config.set_debug_options(ref_opts);
  ref_config.set_num_partitions(kNumPartitions);
  TF_ASSERT_OK_AND_ASSIGN(
      auto ref_module,
      ParseAndReturnVerifiedModule(kModuleReplicatedStr, ref_config));
  auto fake_ref_arguments = xla::MakeFakeArguments(ref_module.get()).value();
  std::vector<Literal*> ref_fake_ptrs(fake_ref_arguments.size());
  for (int i = 0; i < fake_ref_arguments.size(); i++) {
    ref_fake_ptrs[i] = &fake_ref_arguments[i];
  }

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> ref_results,
      HloTestBase::ExecuteReplicated(
          std::move(ref_module), ref_fake_ptrs, kNumPartitions, &assn,
          true /*run_hlo_passes*/, true /*use-threads*/));
  ASSERT_EQ(ref_results.size(), kNumPartitions);
  ErrorSpec error_spec{1e-2, 1e-2};
  // Results should be the same between windowed einsum and non-windowed cases
  for (int i = 0; i < kNumPartitions; i++) {
    EXPECT_TRUE(LiteralTestUtil::Near(ref_results[i], results[i], error_spec));
  }
}

TEST_F(CollectiveOpsTestE2E, Bar) {
  absl::string_view kModuleReplicatedStr = R"(
HloModule module, entry_computation_layout={{(bf16[3,128,128], bf16[3,1,32,128])->bf16[3,128,128]}, allow_spmd_sharding_propagation_to_parameters={false,false}, replica_count=4

add {
  lhs = bf16[] parameter(0)
  rhs = bf16[] parameter(1)
  ROOT add = bf16[] add(lhs, rhs)
}

while_cond {
  param = (s32[], bf16[3,128,128], bf16[3,1,32,128]) parameter(0)
  gte = s32[] get-tuple-element(param), index=0
  constant.1 = s32[] constant(3)
  ROOT cmp = pred[] compare(gte, constant.1), direction=LT
}

while_body {
  param = (s32[], bf16[3,128,128], bf16[3,1,32,128]) parameter(0)
  get-tuple-element.394 = s32[] get-tuple-element(param), index=0
  get-tuple-element.395 = bf16[3,128,128] get-tuple-element(param), index=1
  get-tuple-element.k = bf16[3,1,32,128] get-tuple-element(param), index=2
  constant.2561 = s32[] constant(0)
  constant.2557 = s32[] constant(1)
  add.230 = s32[] add(get-tuple-element.394, constant.2557)
  constant.2559 = s32[] constant(3)
  subtract.139 = s32[] subtract(constant.2559, get-tuple-element.394)
  constant.2560 = s32[] constant(-1)
  add.231 = s32[] add(subtract.139, constant.2560)
  compare.747 = pred[] compare(add.231, constant.2561), direction=LT
  constant.2562 = s32[] constant(2)
  add.232 = s32[] add(subtract.139, constant.2562)
  select.1348 = s32[] select(compare.747, add.232, add.231)
  dynamic-slice.k = bf16[1,1,32,128] dynamic-slice(get-tuple-element.k, select.1348, constant.2561, constant.2561, constant.2561), dynamic_slice_sizes={1,1,32,128}
  r = bf16[1,32,128] reshape(dynamic-slice.k)
  a = bf16[1,32,128] add(r, r), control-predecessors={constant.2559}
  qa = f8e4m3fn[1,32,128] convert(a)
  a1 = bf16[1,32,128] convert(qa)
  ag = bf16[1,128,128] all-gather(a1), dimensions={1}, replica_groups={{0,1,2,3}}
  dynamic-slice.99 = bf16[1,128,128] dynamic-slice(get-tuple-element.395, select.1348, constant.2561, constant.2561), dynamic_slice_sizes={1,128,128}
  ma = bf16[128,128] bitcast(dynamic-slice.99)
  mb = bf16[128,128] bitcast(ag)
  mc = bf16[128,128] dot(ma, mb), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  mul = bf16[1,128,128] bitcast(mc)
  ar.1 = bf16[1,128,128] all-reduce(mul), replica_groups={}, to_apply=add, channel_id=1
  dynamic-update-slice.35 = bf16[3,128,128] dynamic-update-slice(get-tuple-element.395, mul, select.1348, constant.2561, constant.2561)
  ROOT tuple = (s32[], bf16[3,128,128], bf16[3,1,32,128]) tuple(add.230, dynamic-update-slice.35, get-tuple-element.k), control-predecessors={a}
}

ENTRY entry {
  c0 = s32[] constant(0)
  p0 = bf16[3,128,128] parameter(0)
  p1 = bf16[3,1,32,128] parameter(1)
  tuple = (s32[], bf16[3,128,128], bf16[3,1,32,128]) tuple(c0, p0, p1)
  while = (s32[], bf16[3,128,128], bf16[3,1,32,128]) while(tuple), condition=while_cond, body=while_body
  ROOT gte1 = bf16[3,128,128] get-tuple-element(while), index=1
}
)";

  const int64_t kNumReplicas = 4;
  const int64_t kNumPartitions = 1;

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  auto opts = GetDebugOptionsForTest();
  opts.set_xla_gpu_threshold_for_windowed_einsum_mib(0);
  opts.set_xla_gpu_multi_streamed_windowed_einsum(true);
  opts.set_xla_gpu_graph_min_graph_size(200);
  opts.set_xla_gpu_enable_triton_gemm(false);
  config.set_debug_options(opts);
  config.set_num_partitions(kNumPartitions);
  TF_ASSERT_OK_AND_ASSIGN(
      auto module, ParseAndReturnVerifiedModule(kModuleReplicatedStr, config));
  DeviceAssignment assn(/*replica_count=*/kNumReplicas,
                        /*computation_count=*/kNumPartitions);
  config.set_replica_count(kNumReplicas);
  for (int64_t i = 0; i < kNumPartitions; ++i) {
    assn(0, i) = i;
  }

  auto fake_arguments = xla::MakeFakeArguments(module.get()).value();
  std::vector<Literal*> fake_ptrs(fake_arguments.size());
  for (int i = 0; i < fake_arguments.size(); i++) {
    fake_ptrs[i] = &fake_arguments[i];
  }

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      HloTestBase::ExecuteReplicated(
          std::move(module), fake_ptrs, kNumPartitions, &assn,
          true /*run_hlo_passes*/, true /*use-threads*/));
  ASSERT_EQ(results.size(), kNumPartitions);
}

TEST_F(CollectiveOpsTestE2E, Foo) {
  absl::string_view kModuleReplicatedStr = R"(
HloModule pjit_foo, entry_computation_layout={(s32[4,2]{1,0})->s32[4,4,2]{2,1,0}}, allow_spmd_sharding_propagation_to_output={true}, replica_count=4

sum.20 {
  x.21 = s32[] parameter(0)
  y.22 = s32[] parameter(1)
  ROOT add.23 = s32[] add(x.21, y.22)
}

ENTRY main.25 {
  constant.2 = s32[] constant(0)
  broadcast.3 = s32[4,4,2]{2,1,0} broadcast(constant.2), dimensions={}
  Arg_0.1 = s32[4,2]{1,0} parameter(0)
  constant.7 = s32[] constant(10)
  broadcast.8 = s32[4,2]{1,0} broadcast(constant.7), dimensions={}
  add.9 = s32[4,2]{1,0} add(Arg_0.1, broadcast.8), metadata={op_name="pjit(foo)/jit(main)/add" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=29}
  replica-id.10 = u32[] replica-id(), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  constant.5 = u32[] constant(1)
  divide.11 = u32[] divide(replica-id.10, constant.5), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  constant.4 = u32[] constant(4)
  remainder.12 = u32[] remainder(divide.11, constant.4), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  constant.6 = u32[] constant(0)
  dynamic-slice.13 = s32[1,2]{1,0} dynamic-slice(add.9, remainder.12, constant.6), dynamic_slice_sizes={1,2}, metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  all-gather.14 = s32[4,2]{1,0} all-gather(dynamic-slice.13), replica_groups={{0,1,2,3}}, dimensions={0}, metadata={op_name="pjit(foo)/jit(main)/pmap(<lambda>)/all_gather[all_gather_dimension=0 axis_name=i axis_index_groups=None axis_size=4 tiled=False]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  broadcast.18 = s32[1,4,2]{2,1,0} broadcast(all-gather.14), dimensions={1,2}, metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  replica-id.15 = u32[] replica-id(), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  divide.16 = u32[] divide(replica-id.15, constant.5), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  remainder.17 = u32[] remainder(divide.16, constant.4), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  dynamic-update-slice.19 = s32[4,4,2]{2,1,0} dynamic-update-slice(broadcast.3, broadcast.18, remainder.17, constant.6, constant.6), metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
  ROOT all-reduce.24 = s32[4,4,2]{2,1,0} all-reduce(dynamic-update-slice.19), replica_groups={{0,1,2,3}}, to_apply=sum.20, metadata={op_name="pjit(foo)/jit(main)/xla_pmap[backend=None axis_name=i axis_size=4 global_axis_size=4 devices=None in_axes=(0,) name=<lambda> donated_invars=(False,) is_explicit_global_axis_size=False out_axes=(0,)]" source_file="/home/tmp/move_cp_post_layout/psum.py" source_line=31}
} // main.25
)";

  const int64_t kNumReplicas = 4;
  const int64_t kNumPartitions = 1;

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  auto opts = GetDebugOptionsForTest();
  opts.set_xla_gpu_threshold_for_windowed_einsum_mib(0);
  opts.set_xla_gpu_multi_streamed_windowed_einsum(true);
  opts.set_xla_gpu_graph_min_graph_size(200);
  opts.set_xla_gpu_enable_triton_gemm(false);
  config.set_debug_options(opts);
  config.set_num_partitions(kNumPartitions);
  TF_ASSERT_OK_AND_ASSIGN(
      auto module, ParseAndReturnVerifiedModule(kModuleReplicatedStr, config));
  DeviceAssignment assn(/*replica_count=*/kNumReplicas,
                        /*computation_count=*/kNumPartitions);
  config.set_replica_count(kNumReplicas);
  for (int64_t i = 0; i < kNumPartitions; ++i) {
    assn(0, i) = i;
  }

  auto fake_arguments = xla::MakeFakeArguments(module.get()).value();
  std::vector<Literal*> fake_ptrs(fake_arguments.size());
  for (int i = 0; i < fake_arguments.size(); i++) {
    fake_ptrs[i] = &fake_arguments[i];
  }

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      HloTestBase::ExecuteReplicated(
          std::move(module), fake_ptrs, kNumPartitions, &assn,
          true /*run_hlo_passes*/, true /*use-threads*/));
  ASSERT_EQ(results.size(), kNumPartitions);
}
}  // namespace
}  // namespace xla
