/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include "../component-instantiation/helpers.h"
#include "wasi_p2_unavailable_context_test.h"
extern "C" {
#include "wasm_component_canon.h"
#include "wasm_component_runtime.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_canonical.h"
#include "wasm_component_resource.h"
#include "wasm_component_resource_table.h"
#include "wasi_p2_io.h"
#include "wasi_p2_io_wrapper.h"
#include "wasi_p2_types.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

constexpr uint32_t kReadLowerIndex = 23;
constexpr uint32_t kBlockingReadLowerIndex = 24;
constexpr uint32_t kSkipLowerIndex = 25;
constexpr uint32_t kBlockingSkipLowerIndex = 26;
constexpr uint32_t kWriteLowerIndex = 28;
constexpr uint32_t kBlockingWriteLowerIndex = 29;
constexpr uint32_t kWriteZeroesLowerIndex = 32;
constexpr uint32_t kBlockingWriteZeroesLowerIndex = 33;
constexpr uint32_t kSpliceLowerIndex = 34;
constexpr uint32_t kBlockingSpliceLowerIndex = 35;
constexpr uint32_t kResultOffset = 0;
constexpr uint32_t kPayloadOffset = 4096;

struct IoNativeFdQuotaState {
    std::atomic<uint32_t> limit{ 64 };
    std::atomic<uint32_t> active{ 0 };
    std::atomic<uint32_t> peak{ 0 };
    std::atomic<uint32_t> denials{ 0 };
    std::atomic<bool> release_underflow{ false };
};

bool
reserve_io_native_fds(void *attachment, uint32_t count)
{
    auto *state = static_cast<IoNativeFdQuotaState *>(attachment);
    uint32_t active = state->active.load();

    while (active <= state->limit.load()
           && count <= state->limit.load() - active) {
        if (state->active.compare_exchange_weak(active, active + count)) {
            uint32_t peak = state->peak.load();
            while (
                peak < active + count
                && !state->peak.compare_exchange_weak(peak, active + count)) {
            }
            return true;
        }
    }
    state->denials++;
    return false;
}

void
release_io_native_fds(void *attachment, uint32_t count)
{
    auto *state = static_cast<IoNativeFdQuotaState *>(attachment);
    uint32_t active = state->active.load();

    while (true) {
        if (active < count) {
            state->release_underflow = true;
            return;
        }
        if (state->active.compare_exchange_weak(active, active - count))
            return;
    }
}

WASMFunctionInstance *
get_stream_lower(WASMComponentInstance *instance, uint32_t canonical_index)
{
    if (!instance || canonical_index >= instance->core_functions_count) {
        return nullptr;
    }
    WASMFunctionInstance *canonical = instance->core_functions[canonical_index];
    if (!canonical || !canonical->component_function
        || !canonical->canon_options) {
        return nullptr;
    }
    for (uint32_t i = 0; i < instance->core_module_instances_count; i++) {
        WASMModuleInstance *module = instance->core_module_instances[i];
        if (!module || !module->module || !module->e) {
            continue;
        }
        for (uint32_t j = 0; j < module->module->import_function_count; j++) {
            WASMFunctionInstance *lower = &module->e->functions[j];
            if (lower->component_function == canonical->component_function
                && lower->canon_options == canonical->canon_options
                && lower->module_instance && lower->u.func_import
                && lower->u.func_import->func_ptr_linked) {
                return lower;
            }
        }
    }
    return nullptr;
}

WASMFunctionInstance *
get_stream_lower_by_native(WASMComponentInstance *instance, const void *native)
{
    if (!instance || !native)
        return nullptr;
    for (uint32_t i = 0; i < instance->core_module_instances_count; i++) {
        WASMModuleInstance *module = instance->core_module_instances[i];
        if (!module || !module->module || !module->e)
            continue;
        for (uint32_t j = 0; j < module->module->import_function_count; j++) {
            WASMFunctionInstance *lower = &module->e->functions[j];
            if (lower->u.func_import
                && lower->u.func_import->func_ptr_linked == native)
                return lower;
        }
    }
    return nullptr;
}

WASMComponentInstance *
stream_lower_owner(WASMFunctionInstance *lower)
{
    return lower && lower->module_instance
               ? lower->module_instance->comp_instance
               : nullptr;
}

WASMMemoryInstance *
stream_lower_memory(WASMFunctionInstance *lower)
{
    if (!lower || !lower->canon_options
        || !lower->canon_options->lift_lower_opts
        || !lower->canon_options->lift_lower_opts->lift_opts) {
        return nullptr;
    }
    return lower->canon_options->lift_lower_opts->lift_opts->memory;
}

bool
invoke_stream_lower_with_u32_result(WASMFunctionInstance *lower, uint32_t *argv,
                                    uint32_t argc, uint32_t *result_out)
{
    if (!lower || !lower->module_instance || argc != lower->param_cell_num) {
        return false;
    }
    auto *exec_env =
        reinterpret_cast<WASMExecEnv *>(wasm_runtime_get_exec_env_singleton(
            reinterpret_cast<WASMModuleInstanceCommon *>(
                lower->module_instance)));
    if (!exec_env) {
        return false;
    }
    const bool saved_callback_active = exec_env->component_callback_active;
    uint32_t argv_ret[2] = {};
    exec_env->component_callback_active = true;
    const bool result =
        wasm_runtime_invoke_native_p2(exec_env, lower, argv, argc, argv_ret);
    exec_env->component_callback_active = saved_callback_active;
    if (result && result_out)
        *result_out = argv_ret[0];
    return result;
}

bool
invoke_stream_lower(WASMFunctionInstance *lower, uint32_t *argv, uint32_t argc)
{
    return invoke_stream_lower_with_u32_result(lower, argv, argc, nullptr);
}

bool
create_stream_guest_handle(WASMFunctionInstance *lower, uint32_t param_index,
                           uint32_t rep, uint32_t *handle)
{
    if (!lower || !lower->component_function
        || !lower->component_function->func_type
        || !lower->component_function->func_type->params
        || param_index >= lower->component_function->func_type->params->count
        || !handle) {
        return false;
    }
    WASMComponentTypeInstance *param_type =
        lower->component_function->func_type->params->params[param_index].type;
    return param_type && param_type->type_specific.resource_handle
           && canon_resource_new(
               param_type->type_specific.resource_handle->resource,
               stream_lower_owner(lower), rep, handle);
}

bool
load_stream_lower_result(WASMFunctionInstance *lower, wit_value_t *result)
{
    if (!lower || !result || !lower->component_function
        || !lower->component_function->func_type
        || !lower->component_function->func_type->results) {
        return false;
    }
    LiftLowerContext context = {};
    context.canonical_opts = lower->canon_options;
    context.inst = stream_lower_owner(lower);
    context.borrow_scope_type = BORROW_SCOPE_NONE;
    return load(&context, kResultOffset,
                lower->component_function->func_type->results->result, result);
}

void
close_stream_resource(void *data)
{
    auto *stream = static_cast<StreamResourceType *>(data);
    if (stream && stream->fd <= INT32_MAX) {
        close(static_cast<int>(stream->fd));
    }
}

uint32_t
add_file_stream_resource(HostResourceType resource_type, int fd,
                         uint64_t position, bool append = false)
{
    if (fd < 0) {
        return 0;
    }
    HostResource *resource =
        host_resource_create(resource_type, sizeof(StreamResourceType));
    if (!resource) {
        close(fd);
        return 0;
    }
    auto *stream = static_cast<StreamResourceType *>(resource->data);
    stream->type = STREAM_TYPE_FILE;
    stream->fd = static_cast<uint32_t>(fd);
    stream->position = position;
    stream->position_valid = true;
    stream->append = append;
    host_resource_set_dtor(resource, close_stream_resource);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

uint32_t
add_generic_stream_resource(HostResourceType resource_type, int fd)
{
    if (fd < 0) {
        return 0;
    }
    HostResource *resource =
        host_resource_create(resource_type, sizeof(StreamResourceType));
    if (!resource) {
        close(fd);
        return 0;
    }
    auto *stream = static_cast<StreamResourceType *>(resource->data);
    stream->type = STREAM_TYPE_GENERIC;
    stream->fd = static_cast<uint32_t>(fd);
    stream->position = 0;
    stream->position_valid = false;
    stream->append = false;
    host_resource_set_dtor(resource, close_stream_resource);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

std::array<uint32_t, 4>
stream_unary_u64_args(uint32_t handle, uint64_t value)
{
    return { handle, static_cast<uint32_t>(value),
             static_cast<uint32_t>(value >> 32), kResultOffset };
}

std::array<uint32_t, 5>
stream_splice_args(uint32_t output_handle, uint32_t input_handle,
                   uint64_t value)
{
    return { output_handle, input_handle, static_cast<uint32_t>(value),
             static_cast<uint32_t>(value >> 32), kResultOffset };
}

std::vector<uint8_t>
stream_result_bytes(wit_value_t result)
{
    std::vector<uint8_t> bytes;
    if (!result || result->value.result_value.is_err
        || !result->value.result_value.result.ok) {
        return bytes;
    }
    ComponentWITList list =
        result->value.result_value.result.ok->value.list_value;
    bytes.reserve(list.size);
    for (uint32_t i = 0; i < list.size; i++) {
        bytes.push_back(list.elems[i]->value.u8_value);
    }
    return bytes;
}

struct CallbackSource {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    uint32_t max_chunk = 1;
};

wasm_component_wasi_input_stream_status_t
callback_stream_read(void *attachment, uint8_t *buffer, uint32_t capacity,
                     uint32_t *out_bytes_read)
{
    auto *source = static_cast<CallbackSource *>(attachment);
    if (!source || !buffer || !out_bytes_read) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    *out_bytes_read = 0;
    if (source->offset >= source->bytes.size()) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    const size_t count = std::min({ static_cast<size_t>(capacity),
                                    static_cast<size_t>(source->max_chunk),
                                    source->bytes.size() - source->offset });
    memcpy(buffer, source->bytes.data() + source->offset, count);
    source->offset += count;
    *out_bytes_read = static_cast<uint32_t>(count);
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

wasm_component_wasi_input_stream_status_t
callback_stream_skip(void *attachment, uint64_t max_bytes,
                     uint64_t *out_bytes_skipped)
{
    auto *source = static_cast<CallbackSource *>(attachment);
    if (!source || !out_bytes_skipped) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    *out_bytes_skipped = 0;
    if (source->offset >= source->bytes.size()) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    const size_t count = std::min({ static_cast<size_t>(max_bytes),
                                    static_cast<size_t>(source->max_chunk),
                                    source->bytes.size() - source->offset });
    source->offset += count;
    *out_bytes_skipped = count;
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

void
callback_stream_drop(void *attachment)
{
    delete static_cast<CallbackSource *>(attachment);
}

/* Match the private callback stream prefix used by wasi_p2_host_io.c so this
 * unit can drive the HostResource-aware wrapper seam directly. */
struct TestCallbackInputStream {
    IOStreamType type;
    wasm_component_wasi_input_stream_callbacks_t callbacks;
    void *attachment;
    bool dropped;
};

void
drop_test_callback_stream(void *data)
{
    auto *stream = static_cast<TestCallbackInputStream *>(data);
    if (!stream || stream->dropped) {
        return;
    }
    stream->dropped = true;
    stream->callbacks.drop(stream->attachment);
}

uint32_t
add_callback_input_resource(std::vector<uint8_t> bytes,
                            CallbackSource **source_out)
{
    auto *source = new CallbackSource;
    source->bytes = std::move(bytes);
    HostResource *resource = host_resource_create(
        WASI_P2_IO_INPUT_STREAM, sizeof(TestCallbackInputStream));
    if (!resource) {
        delete source;
        return 0;
    }
    auto *stream = static_cast<TestCallbackInputStream *>(resource->data);
    memset(stream, 0, sizeof(*stream));
    stream->type = STREAM_TYPE_HOST_CALLBACK;
    stream->callbacks.struct_size = sizeof(stream->callbacks);
    stream->callbacks.read = callback_stream_read;
    stream->callbacks.skip = callback_stream_skip;
    stream->callbacks.drop = callback_stream_drop;
    stream->attachment = source;
    host_resource_set_dtor(resource, drop_test_callback_stream);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
        return 0;
    }
    if (source_out) {
        *source_out = source;
    }
    return rep;
}

} // namespace

class WasiP2IoWrapperTest : public testing::Test
{
  public:
    WasiP2IoWrapperTest() {}
    ~WasiP2IoWrapperTest() {}
    RuntimeInitArgs init_args;
    unsigned char *component_raw = NULL;
    libc_wasi_parse_context_t parse_ctx = {};

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;
    WASMComponent *component = nullptr;
    WASMComponentInstance *comp_instance = nullptr;
    IoNativeFdQuotaState fd_quota;

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

        char path[PATH_MAX] =
            WAMR_UNIT_TEST_SOURCE_DIR "/test_dir/test_input.txt";

        FILE *input_file = freopen(path, "r", stdin);
        ASSERT_TRUE(input_file);

        bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

        component = LoadfromCandidates("test_io_comp.wasm");
        ASSERT_NE(component, nullptr)
            << "Failed to load/parse component from candidates.";

        libc_wasi_set_default_options(&parse_ctx);
        libc_component_wasi_init(component, 0, NULL, &parse_ctx);

        struct InstantiationArgs2 *instantiation_args = nullptr;
        ASSERT_TRUE(
            wasm_runtime_instantiation_args_create(&instantiation_args));
        wasm_runtime_instantiation_args_set_default_stack_size(
            instantiation_args, COMPONENT_DEFAULT_STACK_SIZE);
        wasm_runtime_instantiation_args_set_host_managed_heap_size(
            instantiation_args, COMPONENT_DEFAULT_HEAP_SIZE);
        wasm_runtime_instantiation_args_set_native_fd_quota(
            instantiation_args, &fd_quota, reserve_io_native_fds,
            release_io_native_fds);
        comp_instance = wasm_component_instantiate_ex2(
            component, instantiation_args, error_buf, sizeof(error_buf));
        wasm_runtime_instantiation_args_destroy(instantiation_args);
        ASSERT_TRUE(comp_instance);

        bh_log_set_verbose_level(WASM_LOG_LEVEL_VERBOSE);

        printf("Ending setup\n");
    }

    virtual void TearDown()
    {
        printf("Starting teardown\n");
        if (comp_instance) {
            wasm_component_deinstantiate(comp_instance);
            comp_instance = nullptr;
        }
        destroy_host_resource_table();
        EXPECT_EQ(fd_quota.active.load(), 0u);
        EXPECT_FALSE(fd_quota.release_underflow.load());
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

    WASMComponent *LoadfromCandidates(const char *file_name)
    {
        return load_component_from_candidates_internal(file_name,
                                                       "libc-wasi-p2");
    }

    void test_function_execution(const char *binary_name, const char *func_name)
    {
        WASMComponent *component = LoadfromCandidates(binary_name);
        ASSERT_NE(component, nullptr)
            << "Failed to load/parse component from candidates.";

        WASMComponentInstance *comp_instance =
            wasm_component_instantiate_internal(component, NULL, error_buf,
                                                sizeof(error_buf));
        ASSERT_TRUE(comp_instance);

        std::string func_name_args = std::string(func_name) + "()";
        bool status = wasm_component_application_execute_func(
            comp_instance, (char *)func_name_args.c_str());
        ASSERT_TRUE(status);
    }
};

TEST_F(WasiP2IoWrapperTest,
       FileReadAndSkipWrappersKeepIndependentLogicalCursors)
{
    std::unique_ptr<FILE, decltype(&fclose)> file(tmpfile(), &fclose);
    ASSERT_NE(file, nullptr);
    const std::array<uint8_t, 6> contents = { 'a', 'b', 'c', 'd', 'e', 'f' };
    ASSERT_EQ(pwrite(fileno(file.get()), contents.data(), contents.size(), 0),
              static_cast<ssize_t>(contents.size()));
    ASSERT_EQ(lseek(fileno(file.get()), 0, SEEK_SET), 0);

    WASMFunctionInstance *read_lower =
        get_stream_lower(comp_instance, kReadLowerIndex);
    WASMFunctionInstance *blocking_read_lower =
        get_stream_lower(comp_instance, kBlockingReadLowerIndex);
    WASMFunctionInstance *skip_lower =
        get_stream_lower(comp_instance, kSkipLowerIndex);
    WASMFunctionInstance *blocking_skip_lower =
        get_stream_lower(comp_instance, kBlockingSkipLowerIndex);
    ASSERT_NE(read_lower, nullptr);
    ASSERT_NE(blocking_read_lower, nullptr);
    ASSERT_NE(skip_lower, nullptr);
    ASSERT_NE(blocking_skip_lower, nullptr);

    const uint32_t first_rep = add_file_stream_resource(
        WASI_P2_IO_INPUT_STREAM, dup(fileno(file.get())), 0);
    const uint32_t second_rep = add_file_stream_resource(
        WASI_P2_IO_INPUT_STREAM, dup(fileno(file.get())), 0);
    ASSERT_NE(first_rep, 0u);
    ASSERT_NE(second_rep, 0u);
    uint32_t first_handle = 0;
    uint32_t second_handle = 0;
    ASSERT_TRUE(
        create_stream_guest_handle(read_lower, 0, first_rep, &first_handle));
    ASSERT_TRUE(
        create_stream_guest_handle(read_lower, 0, second_rep, &second_handle));

    auto first_read_args = stream_unary_u64_args(first_handle, 2);
    ASSERT_TRUE(invoke_stream_lower(read_lower, first_read_args.data(),
                                    first_read_args.size()));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(read_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(stream_result_bytes(result), (std::vector<uint8_t>{ 'a', 'b' }));
    free_wit_value(result);

    auto second_read_args = stream_unary_u64_args(second_handle, 2);
    ASSERT_TRUE(invoke_stream_lower(read_lower, second_read_args.data(),
                                    second_read_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(read_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(stream_result_bytes(result), (std::vector<uint8_t>{ 'a', 'b' }));
    free_wit_value(result);

    HostResourceTable *table = get_global_host_resource_table();
    auto *first = static_cast<StreamResourceType *>(
        host_resource_table_get(table, first_rep)->data);
    auto *second = static_cast<StreamResourceType *>(
        host_resource_table_get(table, second_rep)->data);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->position, 2u);
    EXPECT_EQ(second->position, 2u);
    EXPECT_EQ(lseek(fileno(file.get()), 0, SEEK_CUR), 0);

    auto skip_args = stream_unary_u64_args(first_handle, 1);
    ASSERT_TRUE(
        invoke_stream_lower(skip_lower, skip_args.data(), skip_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(skip_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 1u);
    free_wit_value(result);

    auto blocking_skip_args = stream_unary_u64_args(second_handle, 2);
    ASSERT_TRUE(invoke_stream_lower(blocking_skip_lower,
                                    blocking_skip_args.data(),
                                    blocking_skip_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_skip_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 2u);
    free_wit_value(result);
    EXPECT_EQ(first->position, 3u);
    EXPECT_EQ(second->position, 4u);

    auto blocking_read_args = stream_unary_u64_args(first_handle, 2);
    ASSERT_TRUE(invoke_stream_lower(blocking_read_lower,
                                    blocking_read_args.data(),
                                    blocking_read_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_read_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(stream_result_bytes(result), (std::vector<uint8_t>{ 'd', 'e' }));
    free_wit_value(result);
    EXPECT_EQ(first->position, 5u);
    EXPECT_EQ(second->position, 4u);
    EXPECT_EQ(lseek(fileno(file.get()), 0, SEEK_CUR), 0);
}

TEST_F(WasiP2IoWrapperTest, GenericReadAndSkipWrappersPreserveLegacyFdRouting)
{
    int pipe_fds[2] = { -1, -1 };
    ASSERT_EQ(pipe(pipe_fds), 0);
    const std::array<uint8_t, 4> contents = { 'a', 'b', 'c', 'd' };
    ASSERT_EQ(write(pipe_fds[1], contents.data(), contents.size()),
              static_cast<ssize_t>(contents.size()));

    WASMFunctionInstance *read_lower =
        get_stream_lower(comp_instance, kReadLowerIndex);
    WASMFunctionInstance *blocking_read_lower =
        get_stream_lower(comp_instance, kBlockingReadLowerIndex);
    WASMFunctionInstance *skip_lower =
        get_stream_lower(comp_instance, kSkipLowerIndex);
    WASMFunctionInstance *blocking_skip_lower =
        get_stream_lower(comp_instance, kBlockingSkipLowerIndex);
    ASSERT_NE(read_lower, nullptr);
    ASSERT_NE(blocking_read_lower, nullptr);
    ASSERT_NE(skip_lower, nullptr);
    ASSERT_NE(blocking_skip_lower, nullptr);

    const uint32_t rep =
        add_generic_stream_resource(WASI_P2_IO_INPUT_STREAM, pipe_fds[0]);
    pipe_fds[0] = -1;
    ASSERT_NE(rep, 0u);
    uint32_t handle = 0;
    ASSERT_TRUE(create_stream_guest_handle(read_lower, 0, rep, &handle));

    auto read_args = stream_unary_u64_args(handle, 1);
    ASSERT_TRUE(
        invoke_stream_lower(read_lower, read_args.data(), read_args.size()));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(read_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(stream_result_bytes(result), (std::vector<uint8_t>{ 'a' }));
    free_wit_value(result);

    auto blocking_read_args = stream_unary_u64_args(handle, 1);
    ASSERT_TRUE(invoke_stream_lower(blocking_read_lower,
                                    blocking_read_args.data(),
                                    blocking_read_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_read_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(stream_result_bytes(result), (std::vector<uint8_t>{ 'b' }));
    free_wit_value(result);

    auto skip_args = stream_unary_u64_args(handle, 1);
    ASSERT_TRUE(
        invoke_stream_lower(skip_lower, skip_args.data(), skip_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(skip_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 1u);
    free_wit_value(result);

    auto blocking_skip_args = stream_unary_u64_args(handle, 1);
    ASSERT_TRUE(invoke_stream_lower(blocking_skip_lower,
                                    blocking_skip_args.data(),
                                    blocking_skip_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_skip_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 1u);
    free_wit_value(result);

    HostResource *resource =
        host_resource_table_get(get_global_host_resource_table(), rep);
    ASSERT_NE(resource, nullptr);
    auto *stream = static_cast<StreamResourceType *>(resource->data);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->type, STREAM_TYPE_GENERIC);
    EXPECT_EQ(stream->position, 0u);
    EXPECT_FALSE(stream->position_valid);
    EXPECT_FALSE(stream->append);

    close(pipe_fds[1]);
}

TEST_F(WasiP2IoWrapperTest,
       FileWriteAndZeroesWrappersUseIndependentLogicalCursors)
{
    std::unique_ptr<FILE, decltype(&fclose)> file(tmpfile(), &fclose);
    ASSERT_NE(file, nullptr);
    const std::array<uint8_t, 8> initial = { '.', '.', '.', '.',
                                             '.', '.', '.', '.' };
    ASSERT_EQ(pwrite(fileno(file.get()), initial.data(), initial.size(), 0),
              static_cast<ssize_t>(initial.size()));
    ASSERT_EQ(lseek(fileno(file.get()), 0, SEEK_SET), 0);

    WASMFunctionInstance *write_lower =
        get_stream_lower(comp_instance, kWriteLowerIndex);
    WASMFunctionInstance *blocking_write_lower =
        get_stream_lower(comp_instance, kBlockingWriteLowerIndex);
    WASMFunctionInstance *zeroes_lower =
        get_stream_lower(comp_instance, kWriteZeroesLowerIndex);
    WASMFunctionInstance *blocking_zeroes_lower =
        get_stream_lower(comp_instance, kBlockingWriteZeroesLowerIndex);
    ASSERT_NE(write_lower, nullptr);
    ASSERT_NE(blocking_write_lower, nullptr);
    ASSERT_NE(zeroes_lower, nullptr);
    ASSERT_NE(blocking_zeroes_lower, nullptr);

    const uint32_t first_rep = add_file_stream_resource(
        WASI_P2_IO_OUTPUT_STREAM, dup(fileno(file.get())), 0);
    const uint32_t second_rep = add_file_stream_resource(
        WASI_P2_IO_OUTPUT_STREAM, dup(fileno(file.get())), 4);
    ASSERT_NE(first_rep, 0u);
    ASSERT_NE(second_rep, 0u);
    uint32_t first_handle = 0;
    uint32_t second_handle = 0;
    ASSERT_TRUE(
        create_stream_guest_handle(write_lower, 0, first_rep, &first_handle));
    ASSERT_TRUE(
        create_stream_guest_handle(write_lower, 0, second_rep, &second_handle));

    WASMMemoryInstance *memory = stream_lower_memory(write_lower);
    ASSERT_NE(memory, nullptr);
    ASSERT_GE(memory->memory_data_size, kPayloadOffset + 2u);
    memcpy(memory->memory_data + kPayloadOffset, "AB", 2);
    uint32_t write_args[] = { first_handle, kPayloadOffset, 2, kResultOffset };
    ASSERT_TRUE(invoke_stream_lower(write_lower, write_args, 4));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(write_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    free_wit_value(result);

    memcpy(memory->memory_data + kPayloadOffset, "CD", 2);
    uint32_t blocking_write_args[] = { second_handle, kPayloadOffset, 2,
                                       kResultOffset };
    ASSERT_TRUE(
        invoke_stream_lower(blocking_write_lower, blocking_write_args, 4));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_write_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    free_wit_value(result);

    auto zeroes_args = stream_unary_u64_args(first_handle, 1);
    ASSERT_TRUE(invoke_stream_lower(zeroes_lower, zeroes_args.data(),
                                    zeroes_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(zeroes_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    free_wit_value(result);

    auto blocking_zeroes_args = stream_unary_u64_args(second_handle, 1);
    ASSERT_TRUE(invoke_stream_lower(blocking_zeroes_lower,
                                    blocking_zeroes_args.data(),
                                    blocking_zeroes_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_zeroes_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    free_wit_value(result);

    HostResourceTable *table = get_global_host_resource_table();
    auto *first = static_cast<StreamResourceType *>(
        host_resource_table_get(table, first_rep)->data);
    auto *second = static_cast<StreamResourceType *>(
        host_resource_table_get(table, second_rep)->data);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->position, 3u);
    EXPECT_EQ(second->position, 7u);
    EXPECT_EQ(lseek(fileno(file.get()), 0, SEEK_CUR), 0);

    std::array<uint8_t, 8> actual = {};
    ASSERT_EQ(pread(fileno(file.get()), actual.data(), actual.size(), 0),
              static_cast<ssize_t>(actual.size()));
    const std::array<uint8_t, 8> expected = {
        'A', 'B', 0, '.', 'C', 'D', 0, '.'
    };
    EXPECT_EQ(actual, expected);
}

TEST_F(WasiP2IoWrapperTest,
       CallbackToFileSpliceUsesResourceCursorAndBlockingMode)
{
    std::unique_ptr<FILE, decltype(&fclose)> nonblocking_file(tmpfile(),
                                                              &fclose);
    std::unique_ptr<FILE, decltype(&fclose)> blocking_file(tmpfile(), &fclose);
    ASSERT_NE(nonblocking_file, nullptr);
    ASSERT_NE(blocking_file, nullptr);
    const std::array<uint8_t, 6> initial = { '_', '_', '_', '_', '_', '_' };
    ASSERT_EQ(pwrite(fileno(nonblocking_file.get()), initial.data(),
                     initial.size(), 0),
              static_cast<ssize_t>(initial.size()));
    ASSERT_EQ(
        pwrite(fileno(blocking_file.get()), initial.data(), initial.size(), 0),
        static_cast<ssize_t>(initial.size()));
    ASSERT_EQ(lseek(fileno(nonblocking_file.get()), 0, SEEK_SET), 0);
    ASSERT_EQ(lseek(fileno(blocking_file.get()), 0, SEEK_SET), 0);

    WASMFunctionInstance *splice_lower =
        get_stream_lower(comp_instance, kSpliceLowerIndex);
    WASMFunctionInstance *blocking_splice_lower =
        get_stream_lower(comp_instance, kBlockingSpliceLowerIndex);
    ASSERT_NE(splice_lower, nullptr);
    ASSERT_NE(blocking_splice_lower, nullptr);

    const uint32_t nonblocking_output_rep = add_file_stream_resource(
        WASI_P2_IO_OUTPUT_STREAM, dup(fileno(nonblocking_file.get())), 2);
    const uint32_t blocking_output_rep = add_file_stream_resource(
        WASI_P2_IO_OUTPUT_STREAM, dup(fileno(blocking_file.get())), 1);
    CallbackSource *nonblocking_source = nullptr;
    CallbackSource *blocking_source = nullptr;
    const uint32_t nonblocking_input_rep =
        add_callback_input_resource({ 'a', 'b', 'c' }, &nonblocking_source);
    const uint32_t blocking_input_rep =
        add_callback_input_resource({ 'x', 'y', 'z' }, &blocking_source);
    ASSERT_NE(nonblocking_output_rep, 0u);
    ASSERT_NE(blocking_output_rep, 0u);
    ASSERT_NE(nonblocking_input_rep, 0u);
    ASSERT_NE(blocking_input_rep, 0u);
    ASSERT_NE(nonblocking_source, nullptr);
    ASSERT_NE(blocking_source, nullptr);

    uint32_t nonblocking_output_handle = 0;
    uint32_t nonblocking_input_handle = 0;
    uint32_t blocking_output_handle = 0;
    uint32_t blocking_input_handle = 0;
    ASSERT_TRUE(create_stream_guest_handle(
        splice_lower, 0, nonblocking_output_rep, &nonblocking_output_handle));
    ASSERT_TRUE(create_stream_guest_handle(
        splice_lower, 1, nonblocking_input_rep, &nonblocking_input_handle));
    ASSERT_TRUE(create_stream_guest_handle(blocking_splice_lower, 0,
                                           blocking_output_rep,
                                           &blocking_output_handle));
    ASSERT_TRUE(create_stream_guest_handle(
        blocking_splice_lower, 1, blocking_input_rep, &blocking_input_handle));

    auto splice_args = stream_splice_args(nonblocking_output_handle,
                                          nonblocking_input_handle, 3);
    ASSERT_TRUE(invoke_stream_lower(splice_lower, splice_args.data(),
                                    splice_args.size()));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(splice_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 1u);
    free_wit_value(result);
    EXPECT_EQ(nonblocking_source->offset, 1u);

    auto blocking_splice_args =
        stream_splice_args(blocking_output_handle, blocking_input_handle, 3);
    ASSERT_TRUE(invoke_stream_lower(blocking_splice_lower,
                                    blocking_splice_args.data(),
                                    blocking_splice_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_splice_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 3u);
    free_wit_value(result);
    EXPECT_EQ(blocking_source->offset, 3u);

    HostResourceTable *table = get_global_host_resource_table();
    auto *nonblocking_output = static_cast<StreamResourceType *>(
        host_resource_table_get(table, nonblocking_output_rep)->data);
    auto *blocking_output = static_cast<StreamResourceType *>(
        host_resource_table_get(table, blocking_output_rep)->data);
    ASSERT_NE(nonblocking_output, nullptr);
    ASSERT_NE(blocking_output, nullptr);
    EXPECT_EQ(nonblocking_output->position, 3u);
    EXPECT_EQ(blocking_output->position, 4u);
    EXPECT_EQ(lseek(fileno(nonblocking_file.get()), 0, SEEK_CUR), 0);
    EXPECT_EQ(lseek(fileno(blocking_file.get()), 0, SEEK_CUR), 0);

    std::array<uint8_t, 6> nonblocking_actual = {};
    std::array<uint8_t, 6> blocking_actual = {};
    ASSERT_EQ(pread(fileno(nonblocking_file.get()), nonblocking_actual.data(),
                    nonblocking_actual.size(), 0),
              static_cast<ssize_t>(nonblocking_actual.size()));
    ASSERT_EQ(pread(fileno(blocking_file.get()), blocking_actual.data(),
                    blocking_actual.size(), 0),
              static_cast<ssize_t>(blocking_actual.size()));
    EXPECT_EQ(nonblocking_actual,
              (std::array<uint8_t, 6>{ '_', '_', 'a', '_', '_', '_' }));
    EXPECT_EQ(blocking_actual,
              (std::array<uint8_t, 6>{ '_', 'x', 'y', 'z', '_', '_' }));
}

TEST_F(WasiP2IoWrapperTest, CallbackToGenericSplicePreservesLegacyBlockingModes)
{
    int nonblocking_pipe[2] = { -1, -1 };
    int blocking_pipe[2] = { -1, -1 };
    ASSERT_EQ(pipe(nonblocking_pipe), 0);
    ASSERT_EQ(pipe(blocking_pipe), 0);

    WASMFunctionInstance *splice_lower =
        get_stream_lower(comp_instance, kSpliceLowerIndex);
    WASMFunctionInstance *blocking_splice_lower =
        get_stream_lower(comp_instance, kBlockingSpliceLowerIndex);
    ASSERT_NE(splice_lower, nullptr);
    ASSERT_NE(blocking_splice_lower, nullptr);

    const uint32_t nonblocking_output_rep = add_generic_stream_resource(
        WASI_P2_IO_OUTPUT_STREAM, nonblocking_pipe[1]);
    nonblocking_pipe[1] = -1;
    const uint32_t blocking_output_rep =
        add_generic_stream_resource(WASI_P2_IO_OUTPUT_STREAM, blocking_pipe[1]);
    blocking_pipe[1] = -1;
    CallbackSource *nonblocking_source = nullptr;
    CallbackSource *blocking_source = nullptr;
    const uint32_t nonblocking_input_rep =
        add_callback_input_resource({ 'a', 'b', 'c' }, &nonblocking_source);
    const uint32_t blocking_input_rep =
        add_callback_input_resource({ 'x', 'y', 'z' }, &blocking_source);
    ASSERT_NE(nonblocking_output_rep, 0u);
    ASSERT_NE(blocking_output_rep, 0u);
    ASSERT_NE(nonblocking_input_rep, 0u);
    ASSERT_NE(blocking_input_rep, 0u);
    ASSERT_NE(nonblocking_source, nullptr);
    ASSERT_NE(blocking_source, nullptr);

    uint32_t nonblocking_output_handle = 0;
    uint32_t nonblocking_input_handle = 0;
    uint32_t blocking_output_handle = 0;
    uint32_t blocking_input_handle = 0;
    ASSERT_TRUE(create_stream_guest_handle(
        splice_lower, 0, nonblocking_output_rep, &nonblocking_output_handle));
    ASSERT_TRUE(create_stream_guest_handle(
        splice_lower, 1, nonblocking_input_rep, &nonblocking_input_handle));
    ASSERT_TRUE(create_stream_guest_handle(blocking_splice_lower, 0,
                                           blocking_output_rep,
                                           &blocking_output_handle));
    ASSERT_TRUE(create_stream_guest_handle(
        blocking_splice_lower, 1, blocking_input_rep, &blocking_input_handle));

    auto splice_args = stream_splice_args(nonblocking_output_handle,
                                          nonblocking_input_handle, 3);
    ASSERT_TRUE(invoke_stream_lower(splice_lower, splice_args.data(),
                                    splice_args.size()));
    wit_value_t result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(splice_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 1u);
    free_wit_value(result);
    EXPECT_EQ(nonblocking_source->offset, 1u);

    auto blocking_splice_args =
        stream_splice_args(blocking_output_handle, blocking_input_handle, 3);
    ASSERT_TRUE(invoke_stream_lower(blocking_splice_lower,
                                    blocking_splice_args.data(),
                                    blocking_splice_args.size()));
    result = nullptr;
    ASSERT_TRUE(load_stream_lower_result(blocking_splice_lower, &result));
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->value.result_value.is_err);
    EXPECT_EQ(result->value.result_value.result.ok->value.u64_value, 3u);
    free_wit_value(result);
    EXPECT_EQ(blocking_source->offset, 3u);

    std::array<uint8_t, 3> nonblocking_actual = {};
    std::array<uint8_t, 3> blocking_actual = {};
    ASSERT_EQ(read(nonblocking_pipe[0], nonblocking_actual.data(), 1), 1);
    for (size_t i = 0; i < blocking_actual.size(); i++) {
        ASSERT_EQ(read(blocking_pipe[0], &blocking_actual[i], 1), 1);
    }
    EXPECT_EQ(nonblocking_actual[0], 'a');
    EXPECT_EQ(blocking_actual, (std::array<uint8_t, 3>{ 'x', 'y', 'z' }));

    HostResourceTable *table = get_global_host_resource_table();
    auto *nonblocking_output = static_cast<StreamResourceType *>(
        host_resource_table_get(table, nonblocking_output_rep)->data);
    auto *blocking_output = static_cast<StreamResourceType *>(
        host_resource_table_get(table, blocking_output_rep)->data);
    ASSERT_NE(nonblocking_output, nullptr);
    ASSERT_NE(blocking_output, nullptr);
    EXPECT_EQ(nonblocking_output->position, 0u);
    EXPECT_FALSE(nonblocking_output->position_valid);
    EXPECT_FALSE(nonblocking_output->append);
    EXPECT_EQ(blocking_output->position, 0u);
    EXPECT_FALSE(blocking_output->position_valid);
    EXPECT_FALSE(blocking_output->append);

    close(nonblocking_pipe[0]);
    close(blocking_pipe[0]);
}

// Input Stream

TEST_F(WasiP2IoWrapperTest, test_call_read)
{
    ASSERT_TRUE(wasm_component_application_execute_func(comp_instance,
                                                        (char *)"call-read()"));

    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[7]->func_type->results->result;
    LiftLowerContext cx;
    cx.canonical_opts = comp_instance->core_functions[23]->canon_options;
    cx.inst = comp_instance;

    wit_value_t loaded_value;
    bool load_result = load(&cx, 0, ret_type, &loaded_value);

    ASSERT_TRUE(load_result);
    ASSERT_NE(loaded_value, nullptr);
    ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_blocking_read)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-blocking-read()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[8]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[24]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_call_skip)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-skip()"));

  WASMComponentTypeInstance *ret_type = comp_instance->functions[9]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[25]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest,
       NativeInvocationAcceptsFourByteAlignedCanonicalI64Cells)
{
    WASMFunctionInstance *lower = find_lower(comp_instance, 25);
    ASSERT_NE(lower, nullptr);
    auto *exec_env =
        reinterpret_cast<WASMExecEnv *>(wasm_runtime_get_exec_env_singleton(
            reinterpret_cast<WASMModuleInstanceCommon *>(
                lower->module_instance)));
    ASSERT_NE(exec_env, nullptr);

    std::vector<uint64_t> storage((lower->param_cell_num + 2) / 2 + 1, 0);
    uint32 *argv = reinterpret_cast<uint32 *>(storage.data()) + 1;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(argv) & 7u, 4u);
    uint32 argv_ret[2] = {};

    EXPECT_TRUE(wasm_runtime_invoke_native_p2(exec_env, lower, argv,
                                              lower->param_cell_num, argv_ret));
}

TEST_F(WasiP2IoWrapperTest, test_call_blocking_skip)
{
    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, (char *)"call-blocking-skip()"));

    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[10]->func_type->results->result;
    LiftLowerContext cx;
    cx.canonical_opts = comp_instance->core_functions[26]->canon_options;
    cx.inst = comp_instance;

    wit_value_t loaded_value;
    bool load_result = load(&cx, 0, ret_type, &loaded_value);

    ASSERT_TRUE(load_result);
    ASSERT_NE(loaded_value, nullptr);
    ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_call_subscribe_in)
{
    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, (char *)"call-subscribe-in()"));
}

TEST_F(WasiP2IoWrapperTest,
       NativeFdQuotaDeniesSecondPollableThenDropAllowsReopen)
{
    fd_quota.limit = 1;
    int stream_pipe[2] = { -1, -1 };
    ASSERT_EQ(pipe(stream_pipe), 0);
    close(stream_pipe[1]);

    WASMFunctionInstance *lower = get_stream_lower_by_native(
        comp_instance, reinterpret_cast<const void *>(
                           wasi_io_streams_input_stream_subscribe_wrapper));
    ASSERT_NE(lower, nullptr);
    const uint32_t input_rep =
        add_generic_stream_resource(WASI_P2_IO_INPUT_STREAM, stream_pipe[0]);
    ASSERT_NE(input_rep, 0u);
    uint32_t input_handle = 0;
    ASSERT_TRUE(create_stream_guest_handle(lower, 0, input_rep, &input_handle));

    uint32_t argv[] = { input_handle };
    uint32_t first_pollable = 0;
    ASSERT_TRUE(
        invoke_stream_lower_with_u32_result(lower, argv, 1, &first_pollable));
    ASSERT_NE(first_pollable, 0u);
    EXPECT_EQ(fd_quota.active.load(), 1u);
    EXPECT_EQ(fd_quota.peak.load(), 1u);

    uint32_t denied_pollable = UINT32_MAX;
    EXPECT_FALSE(
        invoke_stream_lower_with_u32_result(lower, argv, 1, &denied_pollable));
    EXPECT_EQ(denied_pollable, UINT32_MAX);
    EXPECT_EQ(fd_quota.denials.load(), 1u);
    EXPECT_EQ(fd_quota.active.load(), 1u);

    WASMComponentResourceHandleInstance *pollable_resource_handle =
        lower->component_function->func_type->results->result[0]
            .type_specific.resource_handle;
    ASSERT_NE(pollable_resource_handle, nullptr);
    ASSERT_TRUE(canon_resource_drop(pollable_resource_handle->resource,
                                    stream_lower_owner(lower), first_pollable));
    EXPECT_EQ(fd_quota.active.load(), 0u);

    wasm_runtime_clear_exception(
        reinterpret_cast<WASMModuleInstanceCommon *>(lower->module_instance));
    uint32_t reopened_pollable = 0;
    ASSERT_TRUE(invoke_stream_lower_with_u32_result(lower, argv, 1,
                                                    &reopened_pollable));
    ASSERT_NE(reopened_pollable, 0u);
    EXPECT_EQ(fd_quota.active.load(), 1u);
    ASSERT_TRUE(canon_resource_drop(pollable_resource_handle->resource,
                                    stream_lower_owner(lower),
                                    reopened_pollable));
    EXPECT_EQ(fd_quota.active.load(), 0u);

    WASMComponentResourceHandleInstance *input_resource_handle =
        lower->component_function->func_type->params->params[0]
            .type->type_specific.resource_handle;
    ASSERT_NE(input_resource_handle, nullptr);
    ASSERT_TRUE(canon_resource_drop(input_resource_handle->resource,
                                    stream_lower_owner(lower), input_handle));
}

// Output stream

TEST_F(WasiP2IoWrapperTest, test_check_write)
{
    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, (char *)"call-check-write()"));

    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[11]->func_type->results->result;
    LiftLowerContext cx;
    cx.canonical_opts = comp_instance->core_functions[27]->canon_options;
    cx.inst = comp_instance;

    wit_value_t loaded_value;
    bool load_result = load(&cx, 0, ret_type, &loaded_value);

    ASSERT_TRUE(load_result);
    ASSERT_NE(loaded_value, nullptr);
    ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_write)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-write()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[12]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[28]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_blocking_write_and_flush)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-blocking-write-and-flush()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[13]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[29]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_flush)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-flush()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[14]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[30]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_blocking_flush)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-blocking-flush()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[15]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[31]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_subscribe_out)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-subscribe-out()"));
}

TEST_F(WasiP2IoWrapperTest, test_write_zeroes)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-write-zeroes()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[16]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[32]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_blockin_write_zeroes_and_flush)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-blocking-write-zeroes-and-flush()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[17]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[33]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_call_splice)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-splice()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[18]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[34]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest, test_blocking_splice)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-blocking-splice()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[19]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[35]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

TEST_F(WasiP2IoWrapperTest,
       SpliceUnsupportedCapabilityDoesNotFreeUninitializedHandles)
{
    WASIContext *context = wasm_runtime_get_wasi_ctx(
        (WASMModuleInstanceCommon *)comp_instance->core_module_instances[0]);
    uint32_t saved_common = context->wasi_options->common;
    context->wasi_options->common = 0;

    bool splice_executed = wasm_component_application_execute_func(
        comp_instance, (char *)"call-splice()");
    bool blocking_splice_executed = wasm_component_application_execute_func(
        comp_instance, (char *)"call-blocking-splice()");

    context->wasi_options->common = saved_common;
    ASSERT_TRUE(splice_executed);
    ASSERT_TRUE(blocking_splice_executed);
}

// Error

TEST_F(WasiP2IoWrapperTest, test_to_debug_string)
{
  // redirect stdin to write only file in order to cause input-stream.read to return last-operation-failed and generate an error resource
  char path[PATH_MAX] = WAMR_UNIT_TEST_SOURCE_DIR "/test_dir/test_output.txt";

  FILE *input_file = freopen(path, "w", stdin);
  ASSERT_TRUE(input_file);
  ASSERT_TRUE(wasm_component_application_execute_func(
      comp_instance, (char *)"call-to-debug-string()"))
      << comp_instance->cur_exception;

  WASMComponentTypeInstance *ret_type = comp_instance->functions[20]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[36]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);

}

// Poll

TEST_F(WasiP2IoWrapperTest, test_block)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-block()"));
}

TEST_F(WasiP2IoWrapperTest, test_ready)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-ready()"));
}

TEST_F(WasiP2IoWrapperTest, test_poll)
{
  ASSERT_TRUE(wasm_component_application_execute_func(comp_instance, (char *)"call-poll()"));
  
  WASMComponentTypeInstance *ret_type = comp_instance->functions[21]->func_type->results->result;
  LiftLowerContext cx;
  cx.canonical_opts = comp_instance->core_functions[37]->canon_options;
  cx.inst = comp_instance;

  wit_value_t loaded_value;
  bool load_result = load(&cx, 0, ret_type, &loaded_value);

  ASSERT_TRUE(load_result);
  ASSERT_NE(loaded_value, nullptr);
  ASSERT_FALSE(loaded_value->value.result_value.is_err);
}

void
exercise_unavailable_io(WASMComponentInstance *comp_instance,
                        bool remove_context)
{
    wasi_p2_test::SideEffectSnapshot snapshot;
    wasi_p2_test::ScopedUnavailableWasi unavailable(comp_instance,
                                                    remove_context);
    ASSERT_TRUE(unavailable.valid());

    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, const_cast<char *>("call-ready()")));
    ASSERT_TRUE(wasm_component_application_execute_func(
        comp_instance, const_cast<char *>("call-poll()")));

    WASMComponentTypeInstance *ret_type =
        comp_instance->functions[21]->func_type->results->result;
    LiftLowerContext cx = {};
    cx.canonical_opts = comp_instance->core_functions[37]->canon_options;
    cx.inst = comp_instance;
    wit_value_t loaded_value = nullptr;
    ASSERT_TRUE(load(&cx, 0, ret_type, &loaded_value));
    ASSERT_NE(loaded_value, nullptr);
    free_wit_value(loaded_value);

    snapshot.expect_unchanged();
}

TEST_F(WasiP2IoWrapperTest, missing_wasi_context_fails_closed)
{
    exercise_unavailable_io(comp_instance, true);
}

TEST_F(WasiP2IoWrapperTest, null_wasi_options_fail_closed)
{
    exercise_unavailable_io(comp_instance, false);
}
