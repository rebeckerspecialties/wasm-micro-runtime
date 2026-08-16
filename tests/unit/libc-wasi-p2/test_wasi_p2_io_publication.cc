/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "component-model/wasm_component_canon.h"
#include "component-model/wasm_component_canonical.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_resource.h"
#include "component-model/wasm_component_runtime.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
#include "wasi_p2_io.h"
#include "wasi_p2_types.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

constexpr uint32_t kReadLowerIndex = 23;
constexpr uint32_t kSkipLowerIndex = 25;
constexpr uint32_t kWriteLowerIndex = 28;
constexpr uint32_t kWriteZeroesLowerIndex = 32;
constexpr uint32_t kSpliceLowerIndex = 34;
constexpr uint32_t kResultOffset = 0;
constexpr uint32_t kPayloadOffset = 4096;
constexpr uint32_t kNoDeniedRequest = std::numeric_limits<uint32_t>::max();

enum class IoPublicationKind {
    FileRead,
    GenericRead,
    SocketRead,
    CallbackRead,
    FileSkip,
    CallbackSkip,
    FileWrite,
    GenericWrite,
    SocketWrite,
    FileZeroes,
    GenericZeroes,
    SocketZeroes,
    FileSplice,
    GenericSplice,
    SocketSplice,
    CallbackFileSplice,
    CallbackGenericSplice,
    CallbackSocketSplice,
};

const char *
kind_name(IoPublicationKind kind)
{
    switch (kind) {
        case IoPublicationKind::FileRead:
            return "file-read";
        case IoPublicationKind::GenericRead:
            return "generic-read";
        case IoPublicationKind::SocketRead:
            return "socket-read";
        case IoPublicationKind::CallbackRead:
            return "callback-read";
        case IoPublicationKind::FileSkip:
            return "file-skip";
        case IoPublicationKind::CallbackSkip:
            return "callback-skip";
        case IoPublicationKind::FileWrite:
            return "file-write";
        case IoPublicationKind::GenericWrite:
            return "generic-write";
        case IoPublicationKind::SocketWrite:
            return "socket-write";
        case IoPublicationKind::FileZeroes:
            return "file-zeroes";
        case IoPublicationKind::GenericZeroes:
            return "generic-zeroes";
        case IoPublicationKind::SocketZeroes:
            return "socket-zeroes";
        case IoPublicationKind::FileSplice:
            return "file-splice";
        case IoPublicationKind::GenericSplice:
            return "generic-splice";
        case IoPublicationKind::SocketSplice:
            return "socket-splice";
        case IoPublicationKind::CallbackFileSplice:
            return "callback-file-splice";
        case IoPublicationKind::CallbackGenericSplice:
            return "callback-generic-splice";
        case IoPublicationKind::CallbackSocketSplice:
            return "callback-socket-splice";
    }
    return "unknown";
}

struct IoQuotaState {
    std::atomic<uint32_t> reserve_calls = 0;
    std::atomic<uint32_t> denied_calls = 0;
    std::atomic<uint64_t> live_bytes = 0;
    std::atomic<uint32_t> live_allocations = 0;
    std::atomic<uint32_t> retain_calls = 0;
    std::atomic<uint32_t> attachment_release_calls = 0;
    std::atomic<uint32_t> deny_at = kNoDeniedRequest;
};

bool
io_quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<IoQuotaState *>(attachment);
    const uint32_t index = state->reserve_calls.fetch_add(1);
    if (index >= state->deny_at.load()) {
        state->denied_calls.fetch_add(1);
        return false;
    }
    state->live_bytes.fetch_add(bytes);
    state->live_allocations.fetch_add(allocations);
    return true;
}

void
io_quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<IoQuotaState *>(attachment);
    state->live_bytes.fetch_sub(bytes);
    state->live_allocations.fetch_sub(allocations);
}

bool
io_quota_retain(void *attachment)
{
    auto *state = static_cast<IoQuotaState *>(attachment);
    state->retain_calls.fetch_add(1);
    return true;
}

void
io_quota_attachment_release(void *attachment)
{
    auto *state = static_cast<IoQuotaState *>(attachment);
    state->attachment_release_calls.fetch_add(1);
}

std::vector<uint8_t>
read_io_component()
{
    const std::string path =
        std::string(WAMR_UNIT_TEST_SOURCE_DIR) + "/wasm-apps/test_io_comp.wasm";
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamsize size = input.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > UINT32_MAX) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

WASMFunctionInstance *
get_lower(WASMComponentInstance *instance, uint32_t canonical_index)
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

WASMComponentInstance *
lower_owner(WASMFunctionInstance *lower)
{
    return lower && lower->module_instance
               ? lower->module_instance->comp_instance
               : nullptr;
}

WASMMemoryInstance *
lower_memory(WASMFunctionInstance *lower)
{
    if (!lower || !lower->canon_options
        || !lower->canon_options->lift_lower_opts
        || !lower->canon_options->lift_lower_opts->lift_opts) {
        return nullptr;
    }
    return lower->canon_options->lift_lower_opts->lift_opts->memory;
}

bool
ensure_guest_memory(WASMFunctionInstance *lower, uint64_t required_size)
{
    WASMMemoryInstance *memory = lower_memory(lower);
    if (!memory) {
        return false;
    }
    while (memory->memory_data_size < required_size) {
        if (!wasm_runtime_enlarge_memory(
                reinterpret_cast<WASMModuleInstanceCommon *>(
                    lower->module_instance),
                1)) {
            return false;
        }
    }
    return true;
}

bool
invoke_lower(WASMFunctionInstance *lower, uint32_t *argv, uint32_t argc)
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
    std::vector<uint64_t> argv_storage((argc + 2) / 2 + 1, 0);
    uint32_t *four_byte_aligned_argv =
        reinterpret_cast<uint32_t *>(argv_storage.data()) + 1;
    std::memcpy(four_byte_aligned_argv, argv, argc * sizeof(uint32_t));
    exec_env->component_callback_active = true;
    const bool success = wasm_runtime_invoke_native_p2(
        exec_env, lower, four_byte_aligned_argv, argc, argv_ret);
    exec_env->component_callback_active = saved_callback_active;
    return success;
}

bool
create_guest_handle(WASMFunctionInstance *lower, uint32_t param_index,
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
               lower_owner(lower), rep, handle);
}

void
close_stream(void *data)
{
    auto *stream = static_cast<StreamResourceType *>(data);
    if (stream && stream->fd <= INT32_MAX) {
        close(static_cast<int>(stream->fd));
    }
}

uint32_t
add_stream(HostResourceType resource_type, IOStreamType stream_type, int fd,
           StreamResourceType **stream_out, uint64_t position = 0)
{
    if (stream_out) {
        *stream_out = nullptr;
    }
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
    stream->type = stream_type;
    stream->fd = static_cast<uint32_t>(fd);
    stream->position = position;
    stream->position_valid = stream_type == STREAM_TYPE_FILE;
    stream->append = false;
    host_resource_set_dtor(resource, close_stream);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    else if (stream_out) {
        *stream_out = stream;
    }
    return rep;
}

struct CallbackState {
    std::array<uint8_t, 2> bytes = { 'A', 'B' };
    size_t offset = 0;
    uint32_t read_calls = 0;
    uint32_t skip_calls = 0;
};

wasm_component_wasi_input_stream_status_t
callback_read(void *attachment, uint8_t *buffer, uint32_t capacity,
              uint32_t *out_bytes_read)
{
    auto *state = static_cast<CallbackState *>(attachment);
    if (!state || !buffer || !out_bytes_read) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    state->read_calls++;
    *out_bytes_read = 0;
    if (state->offset >= state->bytes.size()) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    const size_t count = std::min(static_cast<size_t>(capacity),
                                  state->bytes.size() - state->offset);
    memcpy(buffer, state->bytes.data() + state->offset, count);
    state->offset += count;
    *out_bytes_read = static_cast<uint32_t>(count);
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

wasm_component_wasi_input_stream_status_t
callback_skip(void *attachment, uint64_t max_bytes, uint64_t *out_bytes_skipped)
{
    auto *state = static_cast<CallbackState *>(attachment);
    if (!state || !out_bytes_skipped) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    state->skip_calls++;
    *out_bytes_skipped = 0;
    if (state->offset >= state->bytes.size()) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    const size_t count = std::min(
        static_cast<uint64_t>(state->bytes.size() - state->offset), max_bytes);
    state->offset += count;
    *out_bytes_skipped = count;
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

void
callback_drop(void *attachment)
{
    delete static_cast<CallbackState *>(attachment);
}

struct TestCallbackInputStream {
    IOStreamType type;
    wasm_component_wasi_input_stream_callbacks_t callbacks;
    void *attachment;
    bool dropped;
};

void
drop_callback_stream(void *data)
{
    auto *stream = static_cast<TestCallbackInputStream *>(data);
    if (!stream || stream->dropped) {
        return;
    }
    stream->dropped = true;
    stream->callbacks.drop(stream->attachment);
}

uint32_t
add_callback_stream(CallbackState **state_out)
{
    auto *state = new CallbackState;
    HostResource *resource = host_resource_create(
        WASI_P2_IO_INPUT_STREAM, sizeof(TestCallbackInputStream));
    if (!resource) {
        delete state;
        return 0;
    }
    auto *stream = static_cast<TestCallbackInputStream *>(resource->data);
    memset(stream, 0, sizeof(*stream));
    stream->type = STREAM_TYPE_HOST_CALLBACK;
    stream->callbacks.struct_size = sizeof(stream->callbacks);
    stream->callbacks.read = callback_read;
    stream->callbacks.skip = callback_skip;
    stream->callbacks.drop = callback_drop;
    stream->attachment = state;
    host_resource_set_dtor(resource, drop_callback_stream);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
        return 0;
    }
    *state_out = state;
    return rep;
}

int
bytes_available(int fd)
{
    int bytes = -1;
    return fd >= 0 && ioctl(fd, FIONREAD, &bytes) == 0 ? bytes : -1;
}

uint32_t
open_fd_count()
{
    struct rlimit limit = {};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return UINT32_MAX;
    }
    const rlim_t capped = std::min<rlim_t>(limit.rlim_cur, 65536);
    uint32_t count = 0;
    for (rlim_t fd = 0; fd < capped; fd++) {
        errno = 0;
        if (fcntl(static_cast<int>(fd), F_GETFD) >= 0 || errno != EBADF) {
            count++;
        }
    }
    return count;
}

std::vector<uint8_t>
read_exact_at(int fd, size_t length)
{
    std::vector<uint8_t> bytes(length);
    if (fd < 0
        || (length > 0
            && pread(fd, bytes.data(), length, 0)
                   != static_cast<ssize_t>(length))) {
        return {};
    }
    return bytes;
}

struct LoadedResult {
    bool valid = false;
    bool is_error = false;
    std::vector<uint8_t> bytes;
    uint64_t value = 0;
};

LoadedResult
consume_result(WASMFunctionInstance *lower)
{
    LoadedResult output;
    wit_value_t result = nullptr;
    LiftLowerContext context = {};
    context.canonical_opts = lower->canon_options;
    context.inst = lower_owner(lower);
    context.borrow_scope_type = BORROW_SCOPE_NONE;
    if (!load(&context, kResultOffset,
              lower->component_function->func_type->results->result, &result)
        || !result) {
        return output;
    }

    output.valid = true;
    output.is_error = result->value.result_value.is_err;
    wit_value_t payload = output.is_error
                              ? result->value.result_value.result.err
                              : result->value.result_value.result.ok;
    if (output.is_error && payload
        && payload->type == COMPONENT_VAL_TYPE_VARIANT) {
        wit_value_t error_resource = payload->value.variant_value.value;
        if (error_resource
            && (error_resource->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                || error_resource->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
            const uint32_t rep = error_resource->value.resource_value.value;
            EXPECT_NE(rep, 0u);
            EXPECT_NE(host_resource_table_delete(
                          get_global_host_resource_table(), rep),
                      0u);
        }
    }
    else if (payload && payload->type == COMPONENT_VAL_TYPE_LIST) {
        for (uint32_t i = 0; i < payload->value.list_value.size; i++) {
            output.bytes.push_back(
                payload->value.list_value.elems[i]->value.u8_value);
        }
    }
    else if (payload && payload->type == COMPONENT_VAL_TYPE_PRIMVAL
             && payload->prim_type == WASM_COMP_PRIMVAL_U64) {
        output.value = payload->value.u64_value;
    }
    free_wit_value(result);
    return output;
}

struct IoSideState {
    std::vector<uint8_t> file_bytes;
    uint64_t input_position = 0;
    uint64_t output_position = 0;
    int input_available = -1;
    int output_available = -1;
    size_t callback_offset = 0;
    uint32_t callback_read_calls = 0;
    uint32_t callback_skip_calls = 0;

    bool operator==(const IoSideState &other) const
    {
        return file_bytes == other.file_bytes
               && input_position == other.input_position
               && output_position == other.output_position
               && input_available == other.input_available
               && output_available == other.output_available
               && callback_offset == other.callback_offset
               && callback_read_calls == other.callback_read_calls
               && callback_skip_calls == other.callback_skip_calls;
    }
};

struct IoPublicationRun {
    uint32_t stage = 0;
    uint32_t execution_requests = 0;
    uint32_t denied_calls = 0;
    uint64_t final_live_bytes = UINT64_MAX;
    uint32_t final_live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    bool invoked = false;
    bool denied_state_unchanged = false;
    bool success_state_exact = false;
    bool result_exact = false;
    bool fd_count_unchanged = false;
};

struct IoOperation {
    IoPublicationKind kind;
    WASMFunctionInstance *lower = nullptr;
    uint32_t input_rep = 0;
    uint32_t output_rep = 0;
    uint32_t input_handle = 0;
    uint32_t output_handle = 0;
    StreamResourceType *input_stream = nullptr;
    StreamResourceType *output_stream = nullptr;
    CallbackState *callback = nullptr;
    FILE *input_file = nullptr;
    FILE *output_file = nullptr;
    int input_observer = -1;
    int output_observer = -1;

    explicit IoOperation(IoPublicationKind selected)
      : kind(selected)
    {
    }

    bool uses_callback() const
    {
        return kind == IoPublicationKind::CallbackRead
               || kind == IoPublicationKind::CallbackSkip
               || kind == IoPublicationKind::CallbackFileSplice
               || kind == IoPublicationKind::CallbackGenericSplice
               || kind == IoPublicationKind::CallbackSocketSplice;
    }

    bool is_splice() const
    {
        return kind == IoPublicationKind::FileSplice
               || kind == IoPublicationKind::GenericSplice
               || kind == IoPublicationKind::SocketSplice
               || kind == IoPublicationKind::CallbackFileSplice
               || kind == IoPublicationKind::CallbackGenericSplice
               || kind == IoPublicationKind::CallbackSocketSplice;
    }

    bool is_write() const
    {
        return kind == IoPublicationKind::FileWrite
               || kind == IoPublicationKind::GenericWrite
               || kind == IoPublicationKind::SocketWrite;
    }

    bool is_zeroes() const
    {
        return kind == IoPublicationKind::FileZeroes
               || kind == IoPublicationKind::GenericZeroes
               || kind == IoPublicationKind::SocketZeroes;
    }

    static bool make_pipe(int *host_fd, int *observer_fd, bool host_reads)
    {
        int fds[2] = { -1, -1 };
        if (pipe(fds) != 0) {
            return false;
        }
        *host_fd = host_reads ? fds[0] : fds[1];
        *observer_fd = host_reads ? fds[1] : fds[0];
        return true;
    }

    static bool make_socket(int *host_fd, int *observer_fd)
    {
        int fds[2] = { -1, -1 };
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            return false;
        }
        *host_fd = fds[0];
        *observer_fd = fds[1];
        return true;
    }

    bool setup(WASMComponentInstance *instance)
    {
        uint32_t lower_index = kReadLowerIndex;
        if (kind == IoPublicationKind::FileSkip
            || kind == IoPublicationKind::CallbackSkip) {
            lower_index = kSkipLowerIndex;
        }
        else if (is_write()) {
            lower_index = kWriteLowerIndex;
        }
        else if (is_zeroes()) {
            lower_index = kWriteZeroesLowerIndex;
        }
        else if (is_splice()) {
            lower_index = kSpliceLowerIndex;
        }
        lower = get_lower(instance, lower_index);
        if (!lower || !ensure_guest_memory(lower, kPayloadOffset + 16)) {
            return false;
        }
        memcpy(lower_memory(lower)->memory_data + kPayloadOffset, "AB", 2);

        const bool input_file_kind = kind == IoPublicationKind::FileRead
                                     || kind == IoPublicationKind::FileSkip
                                     || kind == IoPublicationKind::FileSplice;
        const bool output_file_kind =
            kind == IoPublicationKind::FileWrite
            || kind == IoPublicationKind::FileZeroes
            || kind == IoPublicationKind::FileSplice
            || kind == IoPublicationKind::CallbackFileSplice;

        if (input_file_kind) {
            input_file = tmpfile();
            if (!input_file || pwrite(fileno(input_file), "AB", 2, 0) != 2) {
                return false;
            }
            input_rep = add_stream(WASI_P2_IO_INPUT_STREAM, STREAM_TYPE_FILE,
                                   dup(fileno(input_file)), &input_stream, 0);
        }
        else if (uses_callback()) {
            input_rep = add_callback_stream(&callback);
        }
        else if (kind == IoPublicationKind::GenericRead
                 || kind == IoPublicationKind::GenericSplice) {
            int host_fd = -1;
            if (!make_pipe(&host_fd, &input_observer, true)
                || write(input_observer, "AB", 2) != 2) {
                return false;
            }
            input_rep = add_stream(WASI_P2_IO_INPUT_STREAM, STREAM_TYPE_GENERIC,
                                   host_fd, &input_stream);
        }
        else if (kind == IoPublicationKind::SocketRead
                 || kind == IoPublicationKind::SocketSplice) {
            int host_fd = -1;
            if (!make_socket(&host_fd, &input_observer)
                || write(input_observer, "AB", 2) != 2) {
                return false;
            }
            input_rep = add_stream(WASI_P2_IO_INPUT_STREAM, STREAM_TYPE_SOCKET,
                                   host_fd, &input_stream);
        }

        if (output_file_kind) {
            output_file = tmpfile();
            if (!output_file
                || pwrite(fileno(output_file), "xxxx", 4, 0) != 4) {
                return false;
            }
            const uint64_t position =
                kind == IoPublicationKind::FileZeroes ? 1 : 0;
            output_rep =
                add_stream(WASI_P2_IO_OUTPUT_STREAM, STREAM_TYPE_FILE,
                           dup(fileno(output_file)), &output_stream, position);
        }
        else if (is_write() || is_zeroes() || is_splice()) {
            const bool generic =
                kind == IoPublicationKind::GenericWrite
                || kind == IoPublicationKind::GenericZeroes
                || kind == IoPublicationKind::GenericSplice
                || kind == IoPublicationKind::CallbackGenericSplice;
            int host_fd = -1;
            const bool made = generic
                                  ? make_pipe(&host_fd, &output_observer, false)
                                  : make_socket(&host_fd, &output_observer);
            if (!made) {
                return false;
            }
            output_rep =
                add_stream(WASI_P2_IO_OUTPUT_STREAM,
                           generic ? STREAM_TYPE_GENERIC : STREAM_TYPE_SOCKET,
                           host_fd, &output_stream);
        }

        if (input_rep != 0) {
            if (!uses_callback() && !input_stream) {
                return false;
            }
        }
        if (output_rep != 0) {
            if (!output_stream) {
                return false;
            }
        }

        if (is_splice()) {
            return output_rep != 0 && input_rep != 0
                   && create_guest_handle(lower, 0, output_rep, &output_handle)
                   && create_guest_handle(lower, 1, input_rep, &input_handle);
        }
        const bool takes_input = kind == IoPublicationKind::FileRead
                                 || kind == IoPublicationKind::GenericRead
                                 || kind == IoPublicationKind::SocketRead
                                 || kind == IoPublicationKind::CallbackRead
                                 || kind == IoPublicationKind::FileSkip
                                 || kind == IoPublicationKind::CallbackSkip;
        const uint32_t rep = takes_input ? input_rep : output_rep;
        return rep != 0
               && create_guest_handle(
                   lower, 0, rep, takes_input ? &input_handle : &output_handle);
    }

    bool invoke()
    {
        memset(lower_memory(lower)->memory_data + kResultOffset, 0xa5, 16);
        if (is_splice()) {
            uint32_t argv[] = { output_handle, input_handle, 2, 0,
                                kResultOffset };
            return invoke_lower(lower, argv, 5);
        }
        if (is_write()) {
            uint32_t argv[] = { output_handle, kPayloadOffset, 2,
                                kResultOffset };
            return invoke_lower(lower, argv, 4);
        }
        const uint32_t handle =
            input_rep != 0 && output_rep == 0 ? input_handle : output_handle;
        uint32_t argv[] = { handle, 2, 0, kResultOffset };
        return invoke_lower(lower, argv, 4);
    }

    IoSideState snapshot() const
    {
        IoSideState state;
        if (output_file) {
            state.file_bytes = read_exact_at(fileno(output_file), 4);
        }
        if (input_stream && input_stream->type == STREAM_TYPE_FILE) {
            state.input_position = input_stream->position;
        }
        if (output_stream && output_stream->type == STREAM_TYPE_FILE) {
            state.output_position = output_stream->position;
        }
        if (input_observer >= 0) {
            state.input_available = bytes_available(
                input_stream && input_stream->type == STREAM_TYPE_SOCKET
                    ? static_cast<int>(input_stream->fd)
                    : (input_stream && input_stream->type == STREAM_TYPE_GENERIC
                           ? static_cast<int>(input_stream->fd)
                           : input_observer));
        }
        if (output_observer >= 0) {
            state.output_available = bytes_available(output_observer);
        }
        if (callback) {
            state.callback_offset = callback->offset;
            state.callback_read_calls = callback->read_calls;
            state.callback_skip_calls = callback->skip_calls;
        }
        return state;
    }

    bool success_state_exact() const
    {
        const IoSideState state = snapshot();
        switch (kind) {
            case IoPublicationKind::FileRead:
            case IoPublicationKind::FileSkip:
                return state.input_position == 2;
            case IoPublicationKind::GenericRead:
            case IoPublicationKind::SocketRead:
                return state.input_available == 0;
            case IoPublicationKind::CallbackRead:
                return state.callback_offset == 2
                       && state.callback_read_calls == 1
                       && state.callback_skip_calls == 0;
            case IoPublicationKind::CallbackSkip:
                return state.callback_offset == 2
                       && state.callback_read_calls == 0
                       && state.callback_skip_calls == 1;
            case IoPublicationKind::FileWrite:
                return state.output_position == 2
                       && state.file_bytes
                              == (std::vector<uint8_t>{ 'A', 'B', 'x', 'x' });
            case IoPublicationKind::FileZeroes:
                return state.output_position == 3
                       && state.file_bytes
                              == (std::vector<uint8_t>{ 'x', 0, 0, 'x' });
            case IoPublicationKind::GenericWrite:
            case IoPublicationKind::SocketWrite:
            case IoPublicationKind::GenericZeroes:
            case IoPublicationKind::SocketZeroes:
                return state.output_available == 2;
            case IoPublicationKind::FileSplice:
                return state.input_position == 2 && state.output_position == 2
                       && state.file_bytes
                              == (std::vector<uint8_t>{ 'A', 'B', 'x', 'x' });
            case IoPublicationKind::GenericSplice:
            case IoPublicationKind::SocketSplice:
                return state.input_available == 0
                       && state.output_available == 2;
            case IoPublicationKind::CallbackFileSplice:
                return state.callback_offset == 2
                       && state.callback_read_calls == 1
                       && state.output_position == 2
                       && state.file_bytes
                              == (std::vector<uint8_t>{ 'A', 'B', 'x', 'x' });
            case IoPublicationKind::CallbackGenericSplice:
            case IoPublicationKind::CallbackSocketSplice:
                return state.callback_offset == 2
                       && state.callback_read_calls == 1
                       && state.output_available == 2;
        }
        return false;
    }

    bool result_exact(const LoadedResult &result) const
    {
        if (!result.valid || result.is_error) {
            return false;
        }
        if (kind == IoPublicationKind::FileRead
            || kind == IoPublicationKind::GenericRead
            || kind == IoPublicationKind::SocketRead
            || kind == IoPublicationKind::CallbackRead) {
            return result.bytes == (std::vector<uint8_t>{ 'A', 'B' });
        }
        if (kind == IoPublicationKind::FileSkip
            || kind == IoPublicationKind::CallbackSkip || is_splice()) {
            return result.value == 2;
        }
        return true;
    }

    bool peer_bytes_exact() const
    {
        if (output_observer < 0) {
            return true;
        }
        std::array<uint8_t, 2> bytes = {};
        if (read(output_observer, bytes.data(), bytes.size()) != 2) {
            return false;
        }
        if (is_zeroes()) {
            return bytes == (std::array<uint8_t, 2>{ 0, 0 });
        }
        return bytes == (std::array<uint8_t, 2>{ 'A', 'B' });
    }

    void close_observers()
    {
        if (input_observer >= 0) {
            close(input_observer);
            input_observer = -1;
        }
        if (output_observer >= 0) {
            close(output_observer);
            output_observer = -1;
        }
        if (input_file) {
            fclose(input_file);
            input_file = nullptr;
        }
        if (output_file) {
            fclose(output_file);
            output_file = nullptr;
        }
    }
};

IoPublicationRun
run_io_publication(const std::vector<uint8_t> &component_bytes,
                   IoPublicationKind kind, uint32_t denied_execution_request)
{
    IoPublicationRun result;
    IoQuotaState quota;
    std::vector<uint8_t> component_copy = component_bytes;
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    WASMComponent *component = nullptr;
    WASMComponentInstance *instance = nullptr;
    libc_wasi_parse_context_t parse_context = {};
    WASMAllocationQuotaScope owner_scope = {};
    bool owner_scope_active = false;
    bool host_table_initialized = false;
    char error_buf[256] = {};
    IoOperation operation(kind);

    do {
        if (!wasm_runtime_full_init(&init_args)) {
            break;
        }
        result.stage = 1;
        if (!instantiate_host_resource_table()) {
            break;
        }
        host_table_initialized = true;
        result.stage = 2;

        LoadArgs2 args;
        wasm_runtime_load_args2_init(&args);
        args.name = const_cast<char *>("io-publication-test");
        args.no_resolve = 1;
        args.is_component = 1;
        wasm_runtime_load_args2_set_allocation_quota(
            &args, &quota, io_quota_reserve, io_quota_release, io_quota_retain,
            io_quota_attachment_release);
        component = wasm_component_load_ex2(
            component_copy.data(), static_cast<uint32_t>(component_copy.size()),
            &args, error_buf, sizeof(error_buf));
        if (!component) {
            break;
        }
        result.stage = 3;
        libc_wasi_set_default_options(&parse_context);
        libc_component_wasi_init(component, 0, nullptr, &parse_context);
        instance = wasm_component_instantiate_internal(
            component, nullptr, error_buf, sizeof(error_buf));
        if (!instance) {
            break;
        }
        result.stage = 31;
        if (!wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                              component)) {
            break;
        }
        owner_scope_active = true;
        result.stage = 32;
        if (!operation.setup(instance)) {
            break;
        }
        result.stage = 4;

        const IoSideState before = operation.snapshot();
        const uint32_t fd_before = open_fd_count();
        const uint32_t execution_start = quota.reserve_calls.load();
        if (denied_execution_request != kNoDeniedRequest) {
            quota.deny_at.store(execution_start + denied_execution_request);
        }
        result.invoked = operation.invoke();
        const uint32_t execution_end = quota.reserve_calls.load();
        result.execution_requests = execution_end - execution_start;
        result.denied_calls = quota.denied_calls.load();
        quota.deny_at.store(kNoDeniedRequest);

        if (denied_execution_request == kNoDeniedRequest) {
            const LoadedResult loaded = result.invoked
                                            ? consume_result(operation.lower)
                                            : LoadedResult{};
            result.success_state_exact = operation.success_state_exact();
            result.result_exact =
                operation.result_exact(loaded) && operation.peer_bytes_exact();
        }
        else {
            result.denied_state_unchanged = operation.snapshot() == before;
            if (result.invoked) {
                const LoadedResult denied_result =
                    consume_result(operation.lower);
                result.denied_state_unchanged = result.denied_state_unchanged
                                                && denied_result.valid
                                                && denied_result.is_error;
            }
            else {
                wasm_runtime_clear_exception(
                    reinterpret_cast<WASMModuleInstanceCommon *>(
                        operation.lower->module_instance));
            }

            const bool retry_invoked = operation.invoke();
            const LoadedResult retry_result =
                retry_invoked ? consume_result(operation.lower)
                              : LoadedResult{};
            result.success_state_exact =
                retry_invoked && operation.success_state_exact();
            result.result_exact = operation.result_exact(retry_result)
                                  && operation.peer_bytes_exact();
        }
        result.fd_count_unchanged = open_fd_count() == fd_before;
        result.stage = 5;
    } while (false);

    quota.deny_at.store(kNoDeniedRequest);
    if (owner_scope_active) {
        wasm_allocation_quota_scope_leave(&owner_scope);
    }
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    if (host_table_initialized) {
        destroy_host_resource_table();
    }
    if (component) {
        wasm_component_unload(component);
    }
    operation.close_observers();
    if (result.stage >= 1) {
        wasm_runtime_destroy();
    }
    result.final_live_bytes = quota.live_bytes.load();
    result.final_live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    return result;
}

void
expect_clean_run(const IoPublicationRun &run, IoPublicationKind kind,
                 uint32_t denied_request)
{
    SCOPED_TRACE(std::string(kind_name(kind))
                 + " denial=" + std::to_string(denied_request));
    EXPECT_EQ(run.stage, 5u);
    EXPECT_EQ(run.final_live_bytes, 0u);
    EXPECT_EQ(run.final_live_allocations, 0u);
    EXPECT_GT(run.retain_calls, 0u);
    EXPECT_EQ(run.attachment_release_calls, run.retain_calls);
    EXPECT_TRUE(run.success_state_exact);
    EXPECT_TRUE(run.result_exact);
    EXPECT_TRUE(run.fd_count_unchanged);
}

void
exercise_kind(const std::vector<uint8_t> &component_bytes,
              IoPublicationKind kind)
{
    const IoPublicationRun baseline =
        run_io_publication(component_bytes, kind, kNoDeniedRequest);
    expect_clean_run(baseline, kind, kNoDeniedRequest);
    ASSERT_TRUE(baseline.invoked) << kind_name(kind);
    ASSERT_GT(baseline.execution_requests, 0u) << kind_name(kind);

    for (uint32_t denied = 0; denied < baseline.execution_requests; denied++) {
        const IoPublicationRun run =
            run_io_publication(component_bytes, kind, denied);
        expect_clean_run(run, kind, denied);
        EXPECT_GT(run.denied_calls, 0u);
        EXPECT_TRUE(run.denied_state_unchanged);
    }
}

TEST(WasiP2IoPublicationTest, ReadsAndSkipsAreRetryEquivalentAtEveryDenial)
{
    const std::vector<uint8_t> component = read_io_component();
    ASSERT_FALSE(component.empty());
    for (IoPublicationKind kind :
         { IoPublicationKind::FileRead, IoPublicationKind::GenericRead,
           IoPublicationKind::SocketRead, IoPublicationKind::CallbackRead,
           IoPublicationKind::FileSkip, IoPublicationKind::CallbackSkip }) {
        exercise_kind(component, kind);
    }
}

TEST(WasiP2IoPublicationTest,
     NativeInvocationAcceptsFourByteAlignedCanonicalCells)
{
    const std::vector<uint8_t> component = read_io_component();
    ASSERT_FALSE(component.empty());
    const IoPublicationRun run = run_io_publication(
        component, IoPublicationKind::FileWrite, kNoDeniedRequest);
    expect_clean_run(run, IoPublicationKind::FileWrite, kNoDeniedRequest);
    EXPECT_TRUE(run.invoked);
}

TEST(WasiP2IoPublicationTest, WritesAndZeroesAreRetryEquivalentAtEveryDenial)
{
    const std::vector<uint8_t> component = read_io_component();
    ASSERT_FALSE(component.empty());
    for (IoPublicationKind kind :
         { IoPublicationKind::FileWrite, IoPublicationKind::GenericWrite,
           IoPublicationKind::SocketWrite, IoPublicationKind::FileZeroes,
           IoPublicationKind::GenericZeroes,
           IoPublicationKind::SocketZeroes }) {
        exercise_kind(component, kind);
    }
}

TEST(WasiP2IoPublicationTest, SplicesAreRetryEquivalentAtEveryDenial)
{
    const std::vector<uint8_t> component = read_io_component();
    ASSERT_FALSE(component.empty());
    for (IoPublicationKind kind :
         { IoPublicationKind::FileSplice, IoPublicationKind::GenericSplice,
           IoPublicationKind::SocketSplice,
           IoPublicationKind::CallbackFileSplice,
           IoPublicationKind::CallbackGenericSplice,
           IoPublicationKind::CallbackSocketSplice }) {
        exercise_kind(component, kind);
    }
}

} // namespace
