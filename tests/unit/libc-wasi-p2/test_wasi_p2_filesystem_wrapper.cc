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
#include "wasi_p2_sockets.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

class WasiP2FilesystemWrapperTest : public testing::Test
{
  public:
    WasiP2FilesystemWrapperTest() {}
    ~WasiP2FilesystemWrapperTest() {}
    RuntimeInitArgs init_args;
    unsigned char *component_raw = NULL;
    libc_wasi_parse_context_t parse_ctx = {};

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;
    WASMComponentInstance *comp_instance;

    WASIContext *wasi_ctx;
    char test_dir[PATH_MAX] = WAMR_UNIT_TEST_SOURCE_DIR "/test_dir";

    int32_t dir_fd;

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

      WASMComponent *component = LoadfromCandidates("test_filesystem_comp.wasm");
      ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";

      libc_wasi_set_default_options(&parse_ctx);
      libc_component_wasi_init(component, 0, NULL, &parse_ctx);
      
      comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
      ASSERT_TRUE(comp_instance);

      init_prestats();

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
        char created_link[128] = "";
        strcat(created_link, test_dir);
        strcat(created_link, "/test_file_2.txt");
        unlink(created_link);

        printf("Ending teardown\n");
    }

    WASMComponent* LoadfromCandidates(const char *file_name) { 
      return load_component_from_candidates_internal(file_name, "libc-wasi-p2");
    }

    void init_prestats()
    {
        WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(
            (WASMModuleInstanceCommon *)
                comp_instance->core_module_instances[0]);
        dir_fd = open(test_dir, O_RDONLY | O_DIRECTORY);
        ASSERT_NE(dir_fd, -1) << strerror(errno);
        ASSERT_TRUE(fd_prestats_insert(wasi_ctx->prestats, test_dir, dir_fd));
        ASSERT_TRUE(
            fd_table_insert_existing(wasi_ctx->curfds, dir_fd, dir_fd, false));
    }

    void test_function_execution(const char *binary_name, const char *func_name) {
      WASMComponent *component = LoadfromCandidates(binary_name);
      ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
      
      WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
      ASSERT_TRUE(comp_instance);

      std::string func_name_args = std::string(func_name) + "()";
      bool status = wasm_component_application_execute_func(comp_instance, (char *)func_name_args.c_str());
      ASSERT_TRUE(status);
    }
};

// Descriptor

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_get_directories)
{

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-get-directories()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[4]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[34]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_TRUE(loaded_value->value.list_value.size);
}

TEST_F(WasiP2FilesystemWrapperTest,
       preopen_paths_honor_non_utf8_canonical_encodings)
{
    const StringEncoding encodings[] = {
        ENCODING_UTF_16,
        ENCODING_LATIN_1_UTF_16,
    };
    for (StringEncoding encoding : encodings) {
        wasi_p2_test::SideEffectSnapshot snapshot;
        wasi_p2_test::ScopedStringEncoding scoped_encoding(
            comp_instance->core_functions[34]->canon_options, encoding);
        ASSERT_TRUE(scoped_encoding.valid());
        ASSERT_TRUE(wasm_component_application_execute_func(
            comp_instance, const_cast<char *>("call-fs-get-directories()")));

        WASMComponentTypeInstance *ret_type =
            comp_instance->functions[4]->func_type->results->result;
        LiftLowerContext cx = {};
        cx.canonical_opts = comp_instance->core_functions[34]->canon_options;
        cx.inst = comp_instance;
        wit_value_t loaded_value = nullptr;
        ASSERT_TRUE(load(&cx, 0, ret_type, &loaded_value));
        ASSERT_NE(loaded_value, nullptr);
        ASSERT_EQ(loaded_value->type, COMPONENT_VAL_TYPE_LIST);
        ASSERT_GT(loaded_value->value.list_value.size, 0u);
        wit_value_t tuple = loaded_value->value.list_value.elems[0];
        ASSERT_NE(tuple, nullptr);
        ASSERT_EQ(tuple->type, COMPONENT_VAL_TYPE_TUPLE);
        ASSERT_EQ(tuple->value.tuple_value.size, 2u);
        wit_value_t descriptor = tuple->value.tuple_value.elems[0];
        wit_value_t path = tuple->value.tuple_value.elems[1];
        ASSERT_NE(descriptor, nullptr);
        ASSERT_NE(path, nullptr);
        EXPECT_STREQ(path->value.string_value.chars, test_dir);
        EXPECT_EQ(path->value.string_value.hint_encoding, encoding);
        const uint32_t rep = descriptor->value.resource_value.value;
        free_wit_value(loaded_value);
        ASSERT_EQ(
            host_resource_table_delete(get_global_host_resource_table(), rep),
            1u);
        snapshot.expect_unchanged();
    }
}

void
exercise_unavailable_filesystem(WASMComponentInstance *comp_instance,
                                bool remove_context)
{
    wasi_p2_test::SideEffectSnapshot snapshot;
    wasi_p2_test::ScopedUnavailableWasi unavailable(comp_instance,
                                                    remove_context);
    ASSERT_TRUE(unavailable.valid());

    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, const_cast<char *>("call-fs-get-directories()")));
    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[4]->func_type->results->result;
    LiftLowerContext cx = {};
    cx.canonical_opts = comp_instance->core_functions[34]->canon_options;
    cx.inst = comp_instance;
    wit_value_t loaded_value = nullptr;
    ASSERT_TRUE(load(&cx, 0, ret_type, &loaded_value));
    ASSERT_NE(loaded_value, nullptr);
    ASSERT_EQ(loaded_value->type, COMPONENT_VAL_TYPE_LIST);
    EXPECT_EQ(loaded_value->value.list_value.size, 0u);
    free_wit_value(loaded_value);

    snapshot.expect_unchanged();
}

TEST_F(WasiP2FilesystemWrapperTest, missing_wasi_context_fails_closed)
{
    exercise_unavailable_filesystem(comp_instance, true);
}

TEST_F(WasiP2FilesystemWrapperTest, null_wasi_options_fail_closed)
{
    exercise_unavailable_filesystem(comp_instance, false);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_open_at)
{
    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, (char *)"call-fs-open-at()"));

    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[23]->func_type->results->result;
    LiftLowerContext cx;
    cx.canonical_opts = comp_instance->core_functions[53]->canon_options;
    cx.inst = comp_instance;

    wit_value_t loaded_value;
    bool load_result = load(&cx, 0, ret_type, &loaded_value);

    ASSERT_TRUE(load_result);
    ASSERT_NE(loaded_value, nullptr);
    ASSERT_TRUE(loaded_value->value.list_value.size);
}

TEST_F(WasiP2FilesystemWrapperTest,
       OpenAtUnsupportedCapabilityDoesNotFreeUninitializedPath)
{
    WASIContext *context = wasm_runtime_get_wasi_ctx(
        (WASMModuleInstanceCommon *)comp_instance->core_module_instances[0]);
    uint32_t saved_common = context->wasi_options->common;
    context->wasi_options->common = 0;

    bool executed = wasm_component_application_execute_func(
        comp_instance, (char *)"call-fs-open-at()");

    context->wasi_options->common = saved_common;
    ASSERT_TRUE(executed);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_read_via_stream)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-read-via-stream()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[5]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[35]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_write_via_stream)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-write-via-stream()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[6]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[36]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_append_via_stream)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-append-via-stream()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[6]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[36]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_advise)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-advise()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[8]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[38]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_sync_data)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-sync-data()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[9]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[39]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_get_flags)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-get-flags()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[10]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[40]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_get_type)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-get-type()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[11]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[41]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_set_size)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-set-size()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[12]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[42]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_set_times)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-set-times()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[13]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[43]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_read)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-read()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[14]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[44]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_write)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-write()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[15]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[45]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_read_directory)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-read-directory()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[16]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[46]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_sync)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-sync()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[17]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[47]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_create_directory_at)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-create-directory-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[18]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[48]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

  // Cleanup the newly created directory
  char created_dir[128] = "";
  strcat(created_dir, test_dir);
  strcat(created_dir, "/test_file");
  rmdir(created_dir);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_stat)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-stat()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[19]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[49]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_stat_at)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-stat-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[20]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[50]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_set_times_at)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-set-times-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[21]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[51]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_link_at)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-link-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[22]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[52]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

    // Cleanup the newly created link
  char created_link[128] = "";
  strcat(created_link, test_dir);
  strcat(created_link, "/test_file_2.txt");
  unlink(created_link);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_readlink_at)
{
  char test_file[128] = "";
  strcat(test_file, test_dir);
  strcat(test_file, "/test_file.txt");

  char test_link[128] = "";
  strcat(test_link, test_dir);
  strcat(test_link, "/test_file");

  symlink(test_file, test_link);

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-readlink-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[24]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[54]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

  unlink(test_link);
}

TEST_F(WasiP2FilesystemWrapperTest,
       ReadlinkAtUnsupportedCapabilityDoesNotFreeUninitializedResult)
{
    WASIContext *context = wasm_runtime_get_wasi_ctx(
        (WASMModuleInstanceCommon *)comp_instance->core_module_instances[0]);
    uint32_t saved_common = context->wasi_options->common;
    context->wasi_options->common = 0;

    bool executed = wasm_component_application_execute_func(
        comp_instance, (char *)"call-fs-readlink-at()");

    context->wasi_options->common = saved_common;
    ASSERT_TRUE(executed);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_remove_directory_at)
{
  char created_dir[128] = "";
  strcat(created_dir, test_dir);
  strcat(created_dir, "/test_file");
  mkdir(created_dir, 0777);

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-remove-directory-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[24]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[55]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_rename_at)
{
  char old_path[128] = "";
  char new_path[128] = "";
  strcat(old_path, test_dir);
  strcat(old_path, "/test_file.txt");
  strcat(new_path, test_dir);
  strcat(new_path, "/test_file_2.txt");

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-rename-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[26]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[56]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

  rename(new_path, old_path);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_symlink_at)
{
  char test_link[128] = "";
  strcat(test_link, test_dir);
  strcat(test_link, "/test_file_2.txt");

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-symlink-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[27]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[57]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

  unlink(test_link);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_unlink_file_at)
{
  char test_file[128] = "";
  strcat(test_file, test_dir);
  strcat(test_file, "/test_file.txt");

  char test_link[128] = "";
  strcat(test_link, test_dir);
  strcat(test_link, "/test_file");

  symlink(test_file, test_link);

  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-unlink-file-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[28]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[58]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

  unlink(test_link);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_is_same_object)
{
  uint32 argc1 = 1;
  uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 1);
  ASSERT_TRUE(wasm_component_application_execute_func_ex(comp_instance, (char*)"call-fs-is-same-object()", &argc1, &argv1));
  ASSERT_TRUE(argv1[0] > 0);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_metadata_hash)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-metadata-hash()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[29]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[59]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_metadata_hash_at)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-metadata-hash-at()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[30]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[60]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_read_directory_entry)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-read-directory-entry()"));
   
  WASMComponentTypeInstance *ret_type = comp_instance->functions[31]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[61]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2FilesystemWrapperTest, test_call_fs_error_code)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-fs-error-code()"));
    
  WASMComponentTypeInstance *ret_type = comp_instance->functions[32]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[62]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_TRUE(loaded_value->value.option_value.optional_elem);
}
