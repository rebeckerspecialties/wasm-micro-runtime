/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../component-instantiation/helpers.h"
#include "wasi_p2_unavailable_context_test.h"
extern "C" {
#include "wasm_component_runtime.h"
#include "wasm_component_host_resource.h"
#include "wasm_allocation_quota.h"
#include "wasi_p2_sockets.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include "wasm_component_canonical.h"
}

namespace {

struct NativeFdQuotaState {
    uint32_t limit = 1;
    std::atomic<uint32_t> active{ 0 };
    std::atomic<uint32_t> peak{ 0 };
    std::atomic<uint32_t> reserve_calls{ 0 };
    std::atomic<uint32_t> release_calls{ 0 };
    std::atomic<uint32_t> denied_calls{ 0 };
};

bool
reserve_native_fds(void *attachment, uint32_t fd_count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);
    uint32_t current = state->active.load();

    state->reserve_calls++;
    while (current <= state->limit && fd_count <= state->limit - current) {
        if (state->active.compare_exchange_weak(current, current + fd_count)) {
            uint32_t peak = state->peak.load();
            while (peak < current + fd_count
                   && !state->peak.compare_exchange_weak(peak,
                                                         current + fd_count)) {
            }
            return true;
        }
    }
    state->denied_calls++;
    return false;
}

void
release_native_fds(void *attachment, uint32_t fd_count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);
    uint32_t previous = state->active.fetch_sub(fd_count);

    if (previous < fd_count) {
        std::abort();
    }
    state->release_calls++;
}

} // namespace

class WasiP2ClocksWrapperTest : public testing::Test
{
  public:
    WasiP2ClocksWrapperTest() {}
    ~WasiP2ClocksWrapperTest() {}
    RuntimeInitArgs init_args;

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;
    WASMComponent *component = nullptr;
    WASMComponentInstance *comp_instance = nullptr;
    libc_wasi_parse_context_t parse_ctx = {};
    NativeFdQuotaState fd_quota;

    virtual void SetUp() {
      printf("Starting setup\n");
      memset(&init_args, 0, sizeof(RuntimeInitArgs));

      init_args.mem_alloc_type = Alloc_With_Pool;
      init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
      init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

      if (!wasm_runtime_full_init(&init_args)) {
          printf("Failed to initialize WAMR runtime.\n");
          runtime_init = false;
      } else {
          runtime_init = true;
      }
      bool success = instantiate_host_resource_table();
      ASSERT_TRUE(success);

      bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

      component = LoadfromCandidates("test_clocks_comp.wasm");
      ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";

      libc_wasi_set_default_options(&parse_ctx);
      libc_component_wasi_init(component, 0, NULL, &parse_ctx);

      struct InstantiationArgs2 *args = nullptr;
      ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
      wasm_runtime_instantiation_args_set_default_stack_size(
          args, COMPONENT_DEFAULT_STACK_SIZE);
      wasm_runtime_instantiation_args_set_host_managed_heap_size(
          args, COMPONENT_DEFAULT_HEAP_SIZE);
      wasm_runtime_instantiation_args_set_native_fd_quota(
          args, &fd_quota, reserve_native_fds, release_native_fds);
      comp_instance = wasm_component_instantiate_ex2(component, args, error_buf,
                                                     sizeof(error_buf));
      wasm_runtime_instantiation_args_destroy(args);
      ASSERT_TRUE(comp_instance);

      bh_log_set_verbose_level(WASM_LOG_LEVEL_VERBOSE);

      destroy_resolve_streams_map();

      printf("Ending setup\n");
    }

    virtual void TearDown() {
      printf("Starting teardown\n");
      if (comp_instance) {
          wasm_component_deinstantiate(comp_instance);
          comp_instance = nullptr;
      }
      EXPECT_EQ(fd_quota.active.load(), 0U);
      destroy_host_resource_table();
      if (component) {
          wasm_component_unload(component);
          component = nullptr;
      }
        if (runtime_init) {
          printf("Starting to destroy runtime\n");
          wasm_runtime_destroy();
          runtime_init = false;
        }
        
        printf("Ending teardown\n");
    }

    WASMComponent* LoadfromCandidates(const char *file_name) { 
      return load_component_from_candidates_internal(file_name, "libc-wasi-p2");
    }
};

// Clocks

TEST_F(WasiP2ClocksWrapperTest, test_call_monotonic_clock_now)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-monotonic-clock-now()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2ClocksWrapperTest, test_call_monotonic_clock_resolution)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-monotonic-clock-resolution()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2ClocksWrapperTest, test_call_monotonic_clock_subscribe_instant)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-monotonic-clock-subscribe-instant()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2ClocksWrapperTest, test_call_monotonic_clock_subscribe_duration)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-monotonic-clock-subscribe-duration()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2ClocksWrapperTest,
       test_native_fd_quota_denies_second_live_clock_pollable)
{
    uint32 argc1 = 1;
    uint32 *argv1 = static_cast<uint32 *>(wasm_runtime_malloc(sizeof(uint32)));
    ASSERT_NE(argv1, nullptr);
    ASSERT_TRUE(wasm_component_application_execute_func_ex(
        comp_instance, (char *)"call-monotonic-clock-subscribe-duration()",
        &argc1, &argv1));
    ASSERT_TRUE(argv1[0] > 0);
    EXPECT_EQ(fd_quota.active.load(), 1U);
    EXPECT_EQ(fd_quota.peak.load(), 1U);

    argc1 = 1;
    argv1[0] = 0;
    EXPECT_FALSE(wasm_component_application_execute_func_ex(
        comp_instance, (char *)"call-monotonic-clock-subscribe-duration()",
        &argc1, &argv1));
    EXPECT_EQ(fd_quota.denied_calls.load(), 1U);
    EXPECT_EQ(fd_quota.active.load(), 1U);

    wasm_runtime_free(argv1);
    wasm_component_deinstantiate(comp_instance);
    comp_instance = nullptr;
    EXPECT_EQ(fd_quota.release_calls.load(), 1U);
    EXPECT_EQ(fd_quota.active.load(), 0U);
}

TEST_F(WasiP2ClocksWrapperTest, test_call_wall_clock_now)
{

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char*)"call-wall-clock-now()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[4]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[7]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value; 
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->type == COMPONENT_VAL_TYPE_RECORD);
  ASSERT_TRUE(loaded_value->value.record_value.size == 2);
  ASSERT_STREQ(loaded_value->value.record_value.fields[0].key, "seconds");
  ASSERT_TRUE(loaded_value->value.record_value.fields[0].value);
  ASSERT_STREQ(loaded_value->value.record_value.fields[1].key, "nanoseconds");
  ASSERT_TRUE(loaded_value->value.record_value.fields[1].value);

}

TEST_F(WasiP2ClocksWrapperTest, test_call_wall_clock_resolution)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-wall-clock-resolution()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[5]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[8]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value; 
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->type == COMPONENT_VAL_TYPE_RECORD);
  ASSERT_TRUE(loaded_value->value.record_value.size == 2);
  ASSERT_STREQ(loaded_value->value.record_value.fields[0].key, "seconds");
  ASSERT_TRUE(loaded_value->value.record_value.fields[0].value);
  ASSERT_STREQ(loaded_value->value.record_value.fields[1].key, "nanoseconds");
  ASSERT_TRUE(loaded_value->value.record_value.fields[1].value);
}

void
exercise_unavailable_clocks(WASMComponentInstance *comp_instance,
                            bool remove_context)
{
    wasi_p2_test::SideEffectSnapshot snapshot;
    wasi_p2_test::ScopedUnavailableWasi unavailable(comp_instance,
                                                    remove_context);
    ASSERT_TRUE(unavailable.valid());

    const char *scalar_calls[] = {
        "call-monotonic-clock-now()",
        "call-monotonic-clock-resolution()",
        "call-monotonic-clock-subscribe-instant()",
        "call-monotonic-clock-subscribe-duration()",
    };
    for (const char *call : scalar_calls) {
        uint32_t argc = 1;
        uint32_t *argv =
            static_cast<uint32_t *>(wasm_runtime_malloc(sizeof(uint32_t)));
        ASSERT_NE(argv, nullptr);
        argv[0] = UINT32_MAX;
        ASSERT_TRUE(wasm_component_application_execute_func_ex(
            comp_instance, const_cast<char *>(call), &argc, &argv));
        EXPECT_EQ(argv[0], 0u) << call;
        wasm_runtime_free(argv);
    }

    struct WallClockCall {
        const char *name;
        uint32_t function_index;
        uint32_t core_function_index;
    };
    const WallClockCall wall_calls[] = {
        { "call-wall-clock-now()", 4, 7 },
        { "call-wall-clock-resolution()", 5, 8 },
    };
    for (const auto &call : wall_calls) {
        ASSERT_TRUE(wasm_component_application_execute_func(
            comp_instance, const_cast<char *>(call.name)));
        WASMComponentTypeInstance *ret_type =
            comp_instance->functions[call.function_index]
                ->func_type->results->result;
        LiftLowerContext cx = {};
        cx.canonical_opts =
            comp_instance->core_functions[call.core_function_index]
                ->canon_options;
        cx.inst = comp_instance;
        wit_value_t loaded_value = nullptr;
        ASSERT_TRUE(load(&cx, 0, ret_type, &loaded_value));
        ASSERT_NE(loaded_value, nullptr);
        ASSERT_EQ(loaded_value->type, COMPONENT_VAL_TYPE_RECORD);
        ASSERT_EQ(loaded_value->value.record_value.size, 2u);
        EXPECT_EQ(
            loaded_value->value.record_value.fields[0].value->value.u64_value,
            0u);
        EXPECT_EQ(
            loaded_value->value.record_value.fields[1].value->value.u32_value,
            0u);
        free_wit_value(loaded_value);
    }

    snapshot.expect_unchanged();
}

TEST_F(WasiP2ClocksWrapperTest, missing_wasi_context_fails_closed)
{
    exercise_unavailable_clocks(comp_instance, true);
}

TEST_F(WasiP2ClocksWrapperTest, null_wasi_options_fail_closed)
{
    exercise_unavailable_clocks(comp_instance, false);
}
