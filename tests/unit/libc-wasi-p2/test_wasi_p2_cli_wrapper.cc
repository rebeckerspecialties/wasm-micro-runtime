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
#include "wasm_component_canonical.h"
#include "wasm_component_resource.h"
#include "wasm_component_resource_table.h"
#include "wasi_p2_common.h"
#include "wasi_p2_sockets.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

StreamResourceType *
get_cli_stream_resource(WASMComponentInstance *instance, uint32_t handle_index)
{
    if (!instance || !instance->table || handle_index == 0) {
        return nullptr;
    }
    auto *handle = static_cast<WASMResourceHandle *>(wasm_component_table_get(
        instance->table, handle_index, WASM_TABLE_ELEM_RESOURCE_HANDLE));
    if (!handle || handle->rep == 0) {
        return nullptr;
    }
    HostResource *resource =
        host_resource_table_get(get_global_host_resource_table(), handle->rep);
    return resource && resource->data
               ? static_cast<StreamResourceType *>(resource->data)
               : nullptr;
}

void
expect_initialized_cli_stream(const StreamResourceType *stream,
                              IOStreamType expected_type)
{
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->type, expected_type);
    EXPECT_EQ(stream->position, 0u);
    EXPECT_FALSE(stream->position_valid);
    EXPECT_FALSE(stream->append);
}

} // namespace

class WasiP2CliWrapperTest : public testing::Test
{
  public:
    WasiP2CliWrapperTest() {}
    ~WasiP2CliWrapperTest() {}
    RuntimeInitArgs init_args;

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;
    WASMComponentInstance *comp_instance;
    libc_wasi_parse_context_t parse_ctx = {};

    char *argv[2] = { (char *)"arg1", (char *)"caf\xC3\xA9" };

    virtual void SetUp()
    {
        printf("Starting setup\n");
        memset(&init_args, 0, sizeof(RuntimeInitArgs));

        init_args.mem_alloc_type = Alloc_With_Pool;
        init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
        init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

        if (!wasm_runtime_full_init(&init_args)) {
            printf("Failed to initialize WAMR runtime.\n");
            runtime_init = false;
        }
        else {
            runtime_init = true;
        }
        bool success = instantiate_host_resource_table();
        ASSERT_TRUE(success);

        bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
        libc_wasi_set_default_options(&parse_ctx);

        parse_ctx.env_list[0] = "MY_VAR=hello";
        parse_ctx.env_list[1] = "ANOTHER=world";
        parse_ctx.env_list_size = 2;

        WASMComponent *component = LoadfromCandidates("test_cli_comp.wasm");
        ASSERT_NE(component, nullptr)
            << "Failed to load/parse component from candidates.";
        libc_component_wasi_init(component, 2, argv, &parse_ctx);

        comp_instance = wasm_component_instantiate_internal(
            component, NULL, error_buf, sizeof(error_buf));
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

// Cli

TEST_F(WasiP2CliWrapperTest, test_call_get_environment)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-environment()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[4]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[11]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->value.list_value.size);
  ASSERT_TRUE(loaded_value->value.list_value.elems[0]->value.tuple_value.elems[0]);

}

TEST_F(WasiP2CliWrapperTest, test_call_get_arguments)
{ 
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-arguments()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[5]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[12]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_EQ(loaded_value->value.list_value.size, 2);
}

TEST_F(WasiP2CliWrapperTest, arguments_honor_non_utf8_canonical_encodings)
{
    const StringEncoding encodings[] = {
        ENCODING_UTF_16,
        ENCODING_LATIN_1_UTF_16,
    };
    for (StringEncoding encoding : encodings) {
        wasi_p2_test::ScopedStringEncoding scoped_encoding(
            comp_instance->core_functions[12]->canon_options, encoding);
        ASSERT_TRUE(scoped_encoding.valid());
        ASSERT_TRUE(wasm_component_application_execute_func(
            comp_instance, const_cast<char *>("call-get-arguments()")));

        WASMComponentTypeInstance *ret_type =
            comp_instance->functions[5]->func_type->results->result;
        LiftLowerContext cx = {};
        cx.canonical_opts = comp_instance->core_functions[12]->canon_options;
        cx.inst = comp_instance;
        wit_value_t loaded_value = nullptr;
        ASSERT_TRUE(load(&cx, 0, ret_type, &loaded_value));
        ASSERT_NE(loaded_value, nullptr);
        ASSERT_EQ(loaded_value->type, COMPONENT_VAL_TYPE_LIST);
        ASSERT_EQ(loaded_value->value.list_value.size, 2u);
        EXPECT_STREQ(
            loaded_value->value.list_value.elems[0]->value.string_value.chars,
            "arg1");
        EXPECT_STREQ(
            loaded_value->value.list_value.elems[1]->value.string_value.chars,
            "caf\xC3\xA9");
        EXPECT_EQ(loaded_value->value.list_value.elems[0]
                      ->value.string_value.hint_encoding,
                  encoding);
        EXPECT_EQ(loaded_value->value.list_value.elems[1]
                      ->value.string_value.hint_encoding,
                  encoding);
        free_wit_value(loaded_value);
    }
}

TEST(WasiP2CommonTest, string_encoding_defaults_safely_without_options)
{
    WASMExecEnv exec_env = {};
    LiftLowerContext cx = {};
    CanonicalOptions canonical_options = {};
    LiftLowerOptions lift_lower_options = {};
    LiftOptions lift_options = {};

    EXPECT_EQ(wasm_get_string_encoding(nullptr), ENCODING_UTF_8);
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);

    exec_env.cx = &cx;
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);
    cx.canonical_opts = &canonical_options;
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);
    canonical_options.lift_lower_opts = &lift_lower_options;
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);

    lift_lower_options.lift_opts = &lift_options;
    lift_options.string_encoding = ENCODING_UTF_16;
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);
    lift_options.string_encoding = ENCODING_LATIN_1_UTF_16;
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);
    lift_options.string_encoding = static_cast<StringEncoding>(UINT32_MAX);
    EXPECT_EQ(wasm_get_string_encoding(&exec_env), ENCODING_UTF_8);
}

TEST_F(WasiP2CliWrapperTest, test_call_initial_cwd)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance,  (char *)"call-initial-cwd()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[6]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[13]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  ASSERT_TRUE(loaded_value->value.option_value.optional_elem);
}

TEST_F(WasiP2CliWrapperTest, test_call_get_stdin)
{
    uint32 argc1 = 1;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
    ASSERT_NE(argv1, nullptr);
    ASSERT_TRUE(wasm_component_application_execute_func_ex(
        comp_instance, (char *)"call-get-stdin()", &argc1, &argv1));
    ASSERT_TRUE(argv1[0] > 0); // index rep
    expect_initialized_cli_stream(
        get_cli_stream_resource(comp_instance, argv1[0]), STREAM_TYPE_GENERIC);
    wasm_runtime_free(argv1);
}

TEST_F(WasiP2CliWrapperTest, test_call_get_stdout)
{
    uint32 argc1 = 1;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
    ASSERT_NE(argv1, nullptr);
    ASSERT_TRUE(wasm_component_application_execute_func_ex(
        comp_instance, (char *)"call-get-stdout()", &argc1, &argv1));
    ASSERT_TRUE(argv1[0] > 0); // index rep
    expect_initialized_cli_stream(
        get_cli_stream_resource(comp_instance, argv1[0]), STREAM_TYPE_GENERIC);
    wasm_runtime_free(argv1);
}

TEST_F(WasiP2CliWrapperTest, test_call_get_stderr)
{
    uint32 argc1 = 1;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
    ASSERT_NE(argv1, nullptr);
    ASSERT_TRUE(wasm_component_application_execute_func_ex(
        comp_instance, (char *)"call-get-stderr()", &argc1, &argv1));
    ASSERT_TRUE(argv1[0] > 0); // index rep
    expect_initialized_cli_stream(
        get_cli_stream_resource(comp_instance, argv1[0]), STREAM_TYPE_GENERIC);
    wasm_runtime_free(argv1);
}

TEST_F(WasiP2CliWrapperTest, test_call_get_terminal_stdin)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-terminal-stdin()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[7]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[14]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);

  // For normal test stdin is redirected to file, causing isatty() to fail
  // For debug execution, this will pass
  if (isatty(fileno(stdin))) {
    ASSERT_TRUE(loaded_value->value.option_value.optional_elem);
  }
  else {
    ASSERT_FALSE(loaded_value->value.option_value.optional_elem);
  }
}

TEST_F(WasiP2CliWrapperTest, test_call_get_terminal_stdout)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-terminal-stdout()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[8]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[15]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);
  
  // For normal test stdout is redirected to file, causing isatty() to fail
  // For debug execution, this will pass
  if (isatty(fileno(stdout))) {
    ASSERT_TRUE(loaded_value->value.option_value.optional_elem);
  }
  else {
    ASSERT_FALSE(loaded_value->value.option_value.optional_elem);
  }
}

TEST_F(WasiP2CliWrapperTest, test_call_get_terminal_stderr)
{

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-get-terminal-stdout()"));
  WASMComponentTypeInstance *ret_type = comp_instance->functions[9]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[16]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);
  ASSERT_TRUE(load_result);

  // For normal test stderr is redirected to file, causing isatty() to fail
  // For debug execution, this will pass
  if (isatty(fileno(stderr))) {
    ASSERT_TRUE(loaded_value->value.option_value.optional_elem);
  }
  else {
    ASSERT_FALSE(loaded_value->value.option_value.optional_elem);
  }
}

TEST_F(WasiP2CliWrapperTest, test_call_exit)
{
    ASSERT_FALSE(wasm_component_application_execute_func(
        comp_instance, (char *)"call-exit()"));
    ASSERT_NE(strstr(comp_instance->cur_exception, "wasi proc exit"), nullptr);
    ASSERT_NE(comp_instance->wasi_ctx, nullptr);
    ASSERT_EQ(comp_instance->wasi_ctx->exit_code, 0U);
}

void
exercise_unavailable_cli(WASMComponentInstance *comp_instance,
                         bool remove_context)
{
    wasi_p2_test::SideEffectSnapshot snapshot;
    wasi_p2_test::ScopedUnavailableWasi unavailable(comp_instance,
                                                    remove_context);
    ASSERT_TRUE(unavailable.valid());

    const char *stream_calls[] = {
        "call-get-stdin()",
        "call-get-stdout()",
        "call-get-stderr()",
    };
    for (const char *call : stream_calls) {
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

    struct TerminalCall {
        const char *name;
        uint32_t function_index;
        uint32_t core_function_index;
    };
    const TerminalCall terminal_calls[] = {
        { "call-get-terminal-stdin()", 7, 14 },
        { "call-get-terminal-stdout()", 8, 15 },
        { "call-get-terminal-stderr()", 9, 16 },
    };
    for (const auto &call : terminal_calls) {
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
        ASSERT_EQ(loaded_value->type, COMPONENT_VAL_TYPE_OPTION);
        EXPECT_EQ(loaded_value->value.option_value.optional_elem, nullptr);
        free_wit_value(loaded_value);
    }

    snapshot.expect_unchanged();
}

TEST_F(WasiP2CliWrapperTest, missing_wasi_context_fails_closed)
{
    exercise_unavailable_cli(comp_instance, true);
}

TEST_F(WasiP2CliWrapperTest, null_wasi_options_fail_closed)
{
    exercise_unavailable_cli(comp_instance, false);
}
