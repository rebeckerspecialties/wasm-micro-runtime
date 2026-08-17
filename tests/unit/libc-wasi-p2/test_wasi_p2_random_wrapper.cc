/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include "../component-instantiation/helpers.h"
#include "wasi_p2_unavailable_context_test.h"

extern "C" {
#include "wasm_component_runtime.h"
#include "wasm_component_host_resource.h"
#include "wasi_p2_sockets.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include "wasm_component_canonical.h"

}

class WasiP2RandomWrapperTest : public testing::Test
{
  public:
    WasiP2RandomWrapperTest() {}
    ~WasiP2RandomWrapperTest() {}
    RuntimeInitArgs init_args;

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;
    WASMComponentInstance *comp_instance;
    libc_wasi_parse_context_t parse_ctx = {};

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

      WASMComponent *component = LoadfromCandidates("test_random_comp.wasm");
      ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";

      libc_wasi_set_default_options(&parse_ctx);
      libc_component_wasi_init(component, 0, NULL, &parse_ctx);
      
      comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
      ASSERT_TRUE(comp_instance);

      bh_log_set_verbose_level(WASM_LOG_LEVEL_VERBOSE);

      destroy_resolve_streams_map();

      printf("Ending setup\n");
    }

    virtual void TearDown() {
      printf("Starting teardown\n");
      destroy_host_resource_table();
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

// Random

TEST_F(WasiP2RandomWrapperTest, test_call_get_random_bytes)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-random-bytes()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[2]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[6]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->type == COMPONENT_VAL_TYPE_LIST);
  ASSERT_TRUE(loaded_value->value.list_value.elems[0]->prim_type == WASM_COMP_PRIMVAL_U8);
  ASSERT_TRUE(loaded_value->value.list_value.size > 0);
}

TEST_F(WasiP2RandomWrapperTest, test_call_get_random_u64)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-get-random-u64()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2RandomWrapperTest, test_call_get_insecure_random_bytes)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-insecure-random-bytes()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[3]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[7]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->type == COMPONENT_VAL_TYPE_LIST);
  ASSERT_TRUE(loaded_value->value.list_value.elems[0]->prim_type == WASM_COMP_PRIMVAL_U8);
  ASSERT_TRUE(loaded_value->value.list_value.size > 0);
}

TEST_F(WasiP2RandomWrapperTest, test_call_get_insecure_random_u64)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-get-insecure-random-u64()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2RandomWrapperTest, test_call_insecure_seed)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-insecure-seed()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[4]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[8]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->type == COMPONENT_VAL_TYPE_TUPLE);
  ASSERT_TRUE(loaded_value->value.tuple_value.size == 2);
}

void
exercise_unavailable_random(WASMComponentInstance *comp_instance,
                            bool remove_context)
{
    wasi_p2_test::SideEffectSnapshot snapshot;
    wasi_p2_test::ScopedUnavailableWasi unavailable(comp_instance,
                                                    remove_context);
    ASSERT_TRUE(unavailable.valid());

    const char *scalar_calls[] = {
        "call-get-random-u64()",
        "call-get-insecure-random-u64()",
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

    const char *list_calls[] = {
        "call-get-random-bytes()",
        "call-get-insecure-random-bytes()",
    };
    for (const char *call : list_calls) {
        EXPECT_FALSE(wasm_component_application_execute_func(
            comp_instance, const_cast<char *>(call)));
        ASSERT_NE(wasm_component_runtime_get_exception(comp_instance), nullptr);
        EXPECT_STREQ(wasm_component_runtime_get_exception(comp_instance),
                     "Exception: random capability is unavailable");
    }

    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, const_cast<char *>("call-insecure-seed()")));
    WASMComponentTypeInstance *seed_type =
        comp_instance->functions[4]->func_type->results->result;
    LiftLowerContext seed_cx = {};
    seed_cx.canonical_opts = comp_instance->core_functions[8]->canon_options;
    seed_cx.inst = comp_instance;
    wit_value_t seed = nullptr;
    ASSERT_TRUE(load(&seed_cx, 0, seed_type, &seed));
    ASSERT_NE(seed, nullptr);
    ASSERT_EQ(seed->type, COMPONENT_VAL_TYPE_TUPLE);
    ASSERT_EQ(seed->value.tuple_value.size, 2u);
    EXPECT_EQ(seed->value.tuple_value.elems[0]->value.u64_value, 0u);
    EXPECT_EQ(seed->value.tuple_value.elems[1]->value.u64_value, 0u);
    free_wit_value(seed);

    snapshot.expect_unchanged();
}

TEST_F(WasiP2RandomWrapperTest, missing_wasi_context_fails_closed)
{
    exercise_unavailable_random(comp_instance, true);
}

TEST_F(WasiP2RandomWrapperTest, null_wasi_options_fail_closed)
{
    exercise_unavailable_random(comp_instance, false);
}
