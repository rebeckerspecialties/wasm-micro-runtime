/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "component-model/wasm_component_canon.h"
#include "component-model/wasm_component_canonical.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_runtime.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
#include "wasi_p2_filesystem.h"
#include "wasi_p2_types.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

constexpr uint32_t kWriteLowerIndex = 45;
constexpr uint32_t kCreateDirectoryLowerIndex = 48;
constexpr uint32_t kOpenAtLowerIndex = 53;
constexpr uint32_t kReadDirectoryEntryLowerIndex = 62;
constexpr uint32_t kResultOffset = 0;
constexpr uint32_t kPathOffset = 64;
constexpr uint32_t kPayloadOffset = 128;
constexpr uint32_t kNoDeniedRequest = std::numeric_limits<uint32_t>::max();

enum class MutationKind {
    CreateDirectory,
    DescriptorWrite,
    OpenCreate,
    OpenTruncate,
    CreateDirectoryNativeFailure,
    DescriptorWriteNativeFailure,
    OpenNativeFailure,
};

struct FilesystemQuotaState {
    std::atomic<uint32_t> reserve_calls = 0;
    std::atomic<uint32_t> denied_calls = 0;
    std::atomic<uint64_t> live_bytes = 0;
    std::atomic<uint32_t> live_allocations = 0;
    std::atomic<uint32_t> retain_calls = 0;
    std::atomic<uint32_t> attachment_release_calls = 0;
    std::atomic<uint32_t> deny_at = kNoDeniedRequest;
};

bool
filesystem_quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<FilesystemQuotaState *>(attachment);
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
filesystem_quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<FilesystemQuotaState *>(attachment);
    state->live_bytes.fetch_sub(bytes);
    state->live_allocations.fetch_sub(allocations);
}

bool
filesystem_quota_retain(void *attachment)
{
    auto *state = static_cast<FilesystemQuotaState *>(attachment);
    state->retain_calls.fetch_add(1);
    return true;
}

void
filesystem_quota_attachment_release(void *attachment)
{
    auto *state = static_cast<FilesystemQuotaState *>(attachment);
    state->attachment_release_calls.fetch_add(1);
}

std::vector<uint8_t>
read_filesystem_component()
{
    const std::string path = std::string(WAMR_UNIT_TEST_SOURCE_DIR)
                             + "/wasm-apps/test_filesystem_comp.wasm";
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
    exec_env->component_callback_active = true;
    const bool success =
        wasm_runtime_invoke_native_p2(exec_env, lower, argv, argc, argv_ret);
    exec_env->component_callback_active = saved_callback_active;
    return success;
}

bool
create_guest_resource_handle(WASMFunctionInstance *lower, uint32_t rep,
                             uint32_t *handle)
{
    if (!lower || !lower->component_function
        || !lower->component_function->func_type
        || !lower->component_function->func_type->params
        || lower->component_function->func_type->params->count == 0
        || !handle) {
        return false;
    }
    WASMComponentTypeInstance *param_type =
        lower->component_function->func_type->params->params[0].type;
    return param_type && param_type->type_specific.resource_handle
           && canon_resource_new(
               param_type->type_specific.resource_handle->resource,
               lower_owner(lower), rep, handle);
}

uint32_t
add_descriptor_resource(int fd)
{
    HostResource *resource = host_resource_create(WASI_P2_FILESYSTEM_DESCRIPTOR,
                                                  sizeof(wasi_descriptor_t));
    if (!resource) {
        return 0;
    }
    *static_cast<wasi_descriptor_t *>(resource->data) = fd;
    host_resource_set_dtor(resource, filesystem_descriptor_dtor);
    const uint32_t rep =
        host_resource_table_add(get_global_host_resource_table(), resource);
    if (rep == 0) {
        destroy_host_resource(resource);
    }
    return rep;
}

bool
reset_file(int fd, const char *bytes, size_t length)
{
    return ftruncate(fd, 0) == 0
           && (length == 0
               || pwrite(fd, bytes, length, 0) == static_cast<ssize_t>(length));
}

bool
file_matches(int fd, const char *bytes, size_t length)
{
    struct stat statbuf = {};
    if (fstat(fd, &statbuf) != 0
        || statbuf.st_size != static_cast<off_t>(length)) {
        return false;
    }
    std::vector<char> actual(length);
    return (length == 0
            || pread(fd, actual.data(), length, 0)
                   == static_cast<ssize_t>(length))
           && memcmp(actual.data(), bytes, length) == 0;
}

struct PublicationRun {
    uint32_t stage = 0;
    uint32_t execution_requests = 0;
    uint32_t denied_calls = 0;
    uint64_t live_bytes = UINT64_MAX;
    uint32_t live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    uint8_t result_discriminant = 0xff;
    bool invoked = false;
    bool mutated = false;
    bool state_preserved = false;
};

struct DirectoryRetryRun {
    uint32_t stage = 0;
    uint32_t execution_requests = 0;
    uint32_t denied_calls = 0;
    uint64_t live_bytes = UINT64_MAX;
    uint32_t live_allocations = UINT32_MAX;
    uint32_t retain_calls = 0;
    uint32_t attachment_release_calls = 0;
    bool invoked = false;
    bool first_entry_preserved = false;
};

DirectoryRetryRun
run_directory_entry_read(const std::vector<uint8_t> &component_bytes,
                         uint32_t denied_execution_request)
{
    DirectoryRetryRun result;
    FilesystemQuotaState quota;
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
    char temp_dir[] = "/tmp/wasi_p2_fs_directory_retry.XXXXXX";
    char *temp_path = nullptr;
    int directory_fd = -1;
    int first_fd = -1;
    int second_fd = -1;
    wasi_directory_entry_stream_t stream = nullptr;
    wasi_directory_entry_stream_t observed_stream = nullptr;
    std::string expected_name;

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
        temp_path = mkdtemp(temp_dir);
        if (!temp_path) {
            break;
        }
        directory_fd = open(temp_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        first_fd =
            openat(directory_fd, "first", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        second_fd =
            openat(directory_fd, "second", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        const int stream_fd =
            openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0 || first_fd < 0 || second_fd < 0 || stream_fd < 0
            || !(stream = fdopendir(stream_fd))) {
            if (stream_fd >= 0) {
                close(stream_fd);
            }
            break;
        }
        observed_stream = stream;
        wasi_directory_entry_t expected_entry = {};
        bool expected_is_some = false;
        int expected_error = 0;
        wasi_filesystem_read_directory_entry(
            stream, &expected_entry, &expected_is_some, &expected_error);
        if (expected_error != 0 || !expected_is_some || !expected_entry.name) {
            if (expected_entry.name) {
                wasm_runtime_free(expected_entry.name);
            }
            break;
        }
        expected_name = expected_entry.name;
        wasm_runtime_free(expected_entry.name);
        rewinddir(stream);

        LoadArgs2 args;
        wasm_runtime_load_args2_init(&args);
        args.name = const_cast<char *>("filesystem-directory-retry-test");
        args.no_resolve = 1;
        args.is_component = 1;
        wasm_runtime_load_args2_set_allocation_quota(
            &args, &quota, filesystem_quota_reserve, filesystem_quota_release,
            filesystem_quota_retain, filesystem_quota_attachment_release);
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
        if (!instance
            || !wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                                 component)) {
            break;
        }
        owner_scope_active = true;
        result.stage = 4;

        WASMFunctionInstance *lower =
            get_lower(instance, kReadDirectoryEntryLowerIndex);
        HostResource *resource = host_resource_create(
            WASI_P2_DIRECTORY_ENTRY_STREAM, sizeof(stream));
        if (!lower || !resource) {
            destroy_host_resource(resource);
            break;
        }
        *static_cast<wasi_directory_entry_stream_t *>(resource->data) = stream;
        host_resource_set_dtor(resource, directory_entry_stream_dtor);
        const uint32_t rep =
            host_resource_table_add(get_global_host_resource_table(), resource);
        if (rep == 0) {
            destroy_host_resource(resource);
            break;
        }
        stream = nullptr;
        uint32_t handle = 0;
        if (!create_guest_resource_handle(lower, rep, &handle)
            || !ensure_guest_memory(lower, kPayloadOffset + 64)) {
            break;
        }
        result.stage = 5;
        uint8_t *memory = lower_memory(lower)->memory_data;
        auto invoke_entry = [&]() {
            uint32_t argv[] = { handle, kResultOffset };
            return invoke_lower(lower, argv, 2);
        };

        if (!invoke_entry()) {
            break;
        }
        rewinddir(observed_stream);
        memset(memory + kResultOffset, 0xa5, 32);
        const uint32_t execution_start = quota.reserve_calls.load();
        if (denied_execution_request != kNoDeniedRequest) {
            quota.deny_at.store(execution_start + denied_execution_request);
        }
        result.invoked = invoke_entry();
        const uint32_t execution_end = quota.reserve_calls.load();
        quota.deny_at.store(kNoDeniedRequest);
        result.execution_requests = execution_end - execution_start;
        if (!result.invoked) {
            wasm_runtime_clear_exception(
                reinterpret_cast<WASMModuleInstanceCommon *>(
                    lower->module_instance));
        }

        if (denied_execution_request != kNoDeniedRequest) {
            wasi_directory_entry_t retry_entry = {};
            bool retry_is_some = false;
            int retry_error = 0;
            wasi_filesystem_read_directory_entry(observed_stream, &retry_entry,
                                                 &retry_is_some, &retry_error);
            result.first_entry_preserved = retry_error == 0 && retry_is_some
                                           && retry_entry.name
                                           && expected_name == retry_entry.name;
            if (retry_entry.name) {
                wasm_runtime_free(retry_entry.name);
            }
        }
        result.stage = 6;
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
    if (stream) {
        closedir(stream);
    }
    if (first_fd >= 0) {
        close(first_fd);
    }
    if (second_fd >= 0) {
        close(second_fd);
    }
    if (directory_fd >= 0) {
        (void)unlinkat(directory_fd, "first", 0);
        (void)unlinkat(directory_fd, "second", 0);
        close(directory_fd);
    }
    if (temp_path) {
        (void)rmdir(temp_path);
    }
    if (result.stage >= 1) {
        wasm_runtime_destroy();
    }
    result.denied_calls = quota.denied_calls.load();
    result.live_bytes = quota.live_bytes.load();
    result.live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    return result;
}

PublicationRun
run_mutation(const std::vector<uint8_t> &component_bytes, MutationKind kind,
             uint32_t denied_execution_request)
{
    static constexpr char kInitialBytes[] = "ORIGINAL";
    static constexpr char kMutationBytes[] = "MUTATE";
    PublicationRun result;
    FilesystemQuotaState quota;
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
    char temp_dir[] = "/tmp/wasi_p2_fs_publication.XXXXXX";
    char *temp_path = nullptr;
    int native_fd = -1;
    int observer_fd = -1;
    int target_fd = -1;
    const bool is_directory_mutation =
        kind == MutationKind::CreateDirectory
        || kind == MutationKind::CreateDirectoryNativeFailure;
    const bool is_descriptor_write =
        kind == MutationKind::DescriptorWrite
        || kind == MutationKind::DescriptorWriteNativeFailure;
    const bool is_open_mutation = kind == MutationKind::OpenCreate
                                  || kind == MutationKind::OpenTruncate
                                  || kind == MutationKind::OpenNativeFailure;

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
        temp_path = mkdtemp(temp_dir);
        if (!temp_path) {
            break;
        }

        LoadArgs2 args;
        wasm_runtime_load_args2_init(&args);
        args.name = const_cast<char *>("filesystem-publication-test");
        args.no_resolve = 1;
        args.is_component = 1;
        wasm_runtime_load_args2_set_allocation_quota(
            &args, &quota, filesystem_quota_reserve, filesystem_quota_release,
            filesystem_quota_retain, filesystem_quota_attachment_release);
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
        if (!instance
            || !wasm_allocation_quota_scope_enter_for_allocation(&owner_scope,
                                                                 component)) {
            break;
        }
        owner_scope_active = true;
        result.stage = 4;

        uint32_t lower_index = kCreateDirectoryLowerIndex;
        if (is_descriptor_write) {
            lower_index = kWriteLowerIndex;
            const std::string target_path = std::string(temp_path) + "/target";
            native_fd =
                open(target_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        }
        else {
            native_fd = open(temp_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (is_open_mutation) {
                lower_index = kOpenAtLowerIndex;
            }
        }
        if (native_fd < 0) {
            break;
        }
        observer_fd = fcntl(native_fd, F_DUPFD_CLOEXEC, 0);
        if (observer_fd < 0) {
            break;
        }
        if (is_descriptor_write
            && !reset_file(observer_fd, kInitialBytes,
                           sizeof(kInitialBytes) - 1)) {
            break;
        }
        if (kind == MutationKind::OpenTruncate
            || kind == MutationKind::OpenNativeFailure) {
            target_fd = openat(observer_fd, "target",
                               O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (target_fd < 0
                || !reset_file(target_fd, kInitialBytes,
                               sizeof(kInitialBytes) - 1)) {
                break;
            }
        }
        if (kind == MutationKind::CreateDirectoryNativeFailure
            && mkdirat(observer_fd, "existing", 0700) != 0) {
            break;
        }

        WASMFunctionInstance *lower = get_lower(instance, lower_index);
        const uint32_t rep = add_descriptor_resource(native_fd);
        if (rep != 0) {
            native_fd = -1;
        }
        uint32_t handle = 0;
        if (!lower || rep == 0
            || !create_guest_resource_handle(lower, rep, &handle)
            || !ensure_guest_memory(lower, kPayloadOffset + 64)) {
            break;
        }
        result.stage = 5;
        uint8_t *memory = lower_memory(lower)->memory_data;

        auto invoke_path = [&](const char *path, uint32_t open_flags) {
            const uint32_t path_length = static_cast<uint32_t>(strlen(path));
            memcpy(memory + kPathOffset, path, path_length);
            if (is_directory_mutation) {
                uint32_t argv[] = { handle, kPathOffset, path_length,
                                    kResultOffset };
                return invoke_lower(lower, argv, 4);
            }
            uint32_t argv[] = {
                handle,
                0,
                kPathOffset,
                path_length,
                open_flags,
                WASI_DESCRIPTOR_FLAGS_READ | WASI_DESCRIPTOR_FLAGS_WRITE,
                kResultOffset,
            };
            return invoke_lower(lower, argv, 7);
        };
        auto invoke_write = [&](uint64_t offset) {
            memcpy(memory + kPayloadOffset, kMutationBytes,
                   sizeof(kMutationBytes) - 1);
            uint32_t argv[] = { handle,
                                kPayloadOffset,
                                sizeof(kMutationBytes) - 1,
                                static_cast<uint32_t>(offset),
                                static_cast<uint32_t>(offset >> 32),
                                kResultOffset };
            return invoke_lower(lower, argv, 6);
        };

        bool warmup_ok = false;
        if (is_directory_mutation) {
            warmup_ok = invoke_path("warmup", 0)
                        && unlinkat(observer_fd, "warmup", AT_REMOVEDIR) == 0;
        }
        else if (is_descriptor_write) {
            warmup_ok = invoke_write(0)
                        && reset_file(observer_fd, kInitialBytes,
                                      sizeof(kInitialBytes) - 1);
        }
        else if (kind == MutationKind::OpenCreate
                 || kind == MutationKind::OpenNativeFailure) {
            warmup_ok = invoke_path("warmup", WASI_OPEN_FLAGS_CREATE)
                        && unlinkat(observer_fd, "warmup", 0) == 0;
        }
        else {
            warmup_ok = invoke_path("target", WASI_OPEN_FLAGS_TRUNCATE)
                        && reset_file(target_fd, kInitialBytes,
                                      sizeof(kInitialBytes) - 1);
        }
        if (!warmup_ok) {
            break;
        }
        result.stage = 6;

        memset(memory + kResultOffset, 0xa5, 16);
        const uint32_t execution_start = quota.reserve_calls.load();
        if (denied_execution_request != kNoDeniedRequest) {
            quota.deny_at.store(execution_start + denied_execution_request);
        }
        if (kind == MutationKind::CreateDirectory) {
            result.invoked = invoke_path("mutated", 0);
        }
        else if (kind == MutationKind::DescriptorWrite) {
            result.invoked = invoke_write(0);
        }
        else if (kind == MutationKind::OpenCreate) {
            result.invoked = invoke_path("mutated", WASI_OPEN_FLAGS_CREATE);
        }
        else if (kind == MutationKind::OpenTruncate) {
            result.invoked = invoke_path("target", WASI_OPEN_FLAGS_TRUNCATE);
        }
        else if (kind == MutationKind::CreateDirectoryNativeFailure) {
            result.invoked = invoke_path("existing", 0);
        }
        else if (kind == MutationKind::DescriptorWriteNativeFailure) {
            result.invoked = invoke_write(UINT64_MAX);
        }
        else {
            result.invoked = invoke_path(
                "target", WASI_OPEN_FLAGS_CREATE | WASI_OPEN_FLAGS_EXCLUSIVE
                              | WASI_OPEN_FLAGS_TRUNCATE);
        }
        const uint32_t execution_end = quota.reserve_calls.load();
        quota.deny_at.store(kNoDeniedRequest);
        result.execution_requests = execution_end - execution_start;
        result.result_discriminant = memory[kResultOffset];
        if (!result.invoked) {
            wasm_runtime_clear_exception(
                reinterpret_cast<WASMModuleInstanceCommon *>(
                    lower->module_instance));
        }

        if (kind == MutationKind::CreateDirectory) {
            struct stat statbuf = {};
            result.mutated =
                fstatat(observer_fd, "mutated", &statbuf, AT_SYMLINK_NOFOLLOW)
                == 0;
        }
        else if (kind == MutationKind::OpenCreate) {
            struct stat statbuf = {};
            result.mutated =
                fstatat(observer_fd, "mutated", &statbuf, AT_SYMLINK_NOFOLLOW)
                == 0;
        }
        else if (kind == MutationKind::DescriptorWrite
                 || kind == MutationKind::OpenTruncate) {
            result.mutated = !file_matches(
                kind == MutationKind::OpenTruncate ? target_fd : observer_fd,
                kInitialBytes, sizeof(kInitialBytes) - 1);
        }
        if (kind == MutationKind::CreateDirectoryNativeFailure) {
            struct stat statbuf = {};
            result.state_preserved =
                fstatat(observer_fd, "existing", &statbuf, AT_SYMLINK_NOFOLLOW)
                    == 0
                && S_ISDIR(statbuf.st_mode);
        }
        else if (kind == MutationKind::DescriptorWriteNativeFailure) {
            result.state_preserved = file_matches(observer_fd, kInitialBytes,
                                                  sizeof(kInitialBytes) - 1);
        }
        else if (kind == MutationKind::OpenNativeFailure) {
            result.state_preserved = file_matches(target_fd, kInitialBytes,
                                                  sizeof(kInitialBytes) - 1);
        }
        result.stage = 7;
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
    if (native_fd >= 0) {
        close(native_fd);
    }
    if (target_fd >= 0) {
        close(target_fd);
    }
    if (observer_fd >= 0) {
        (void)unlinkat(observer_fd, "warmup", 0);
        (void)unlinkat(observer_fd, "warmup", AT_REMOVEDIR);
        (void)unlinkat(observer_fd, "mutated", 0);
        (void)unlinkat(observer_fd, "mutated", AT_REMOVEDIR);
        (void)unlinkat(observer_fd, "existing", AT_REMOVEDIR);
        (void)unlinkat(observer_fd, "target", 0);
        close(observer_fd);
    }
    if (temp_path) {
        (void)rmdir(temp_path);
    }
    if (result.stage >= 1) {
        wasm_runtime_destroy();
    }
    result.denied_calls = quota.denied_calls.load();
    result.live_bytes = quota.live_bytes.load();
    result.live_allocations = quota.live_allocations.load();
    result.retain_calls = quota.retain_calls.load();
    result.attachment_release_calls = quota.attachment_release_calls.load();
    return result;
}

void
expect_all_denials_precede_mutation(const std::vector<uint8_t> &component,
                                    MutationKind kind)
{
    const PublicationRun baseline =
        run_mutation(component, kind, kNoDeniedRequest);
    ASSERT_EQ(baseline.stage, 7u);
    ASSERT_TRUE(baseline.invoked);
    ASSERT_TRUE(baseline.mutated);
    ASSERT_GT(baseline.execution_requests, 0u);
    EXPECT_EQ(baseline.live_bytes, 0u);
    EXPECT_EQ(baseline.live_allocations, 0u);
    EXPECT_EQ(baseline.retain_calls, baseline.attachment_release_calls);

    for (uint32_t denied_request = 0;
         denied_request < baseline.execution_requests; denied_request++) {
        const PublicationRun denied =
            run_mutation(component, kind, denied_request);
        ASSERT_EQ(denied.stage, 7u) << "denied request " << denied_request;
        EXPECT_GE(denied.denied_calls, 1u)
            << "denied request " << denied_request;
        EXPECT_FALSE(denied.mutated)
            << "native mutation preceded allocation request " << denied_request;
        EXPECT_EQ(denied.live_bytes, 0u) << "denied request " << denied_request;
        EXPECT_EQ(denied.live_allocations, 0u)
            << "denied request " << denied_request;
        EXPECT_EQ(denied.retain_calls, denied.attachment_release_calls)
            << "denied request " << denied_request;
    }
}

TEST(WasiP2FilesystemPublicationTest,
     NamespaceMutationHasNoSideEffectForEveryAllocationDenial)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());
    expect_all_denials_precede_mutation(component,
                                        MutationKind::CreateDirectory);
}

TEST(WasiP2FilesystemPublicationTest,
     DescriptorWriteHasNoSideEffectForEveryAllocationDenial)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());
    expect_all_denials_precede_mutation(component,
                                        MutationKind::DescriptorWrite);
}

TEST(WasiP2FilesystemPublicationTest,
     OpenCreateHasNoSideEffectForEveryAllocationDenial)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());
    expect_all_denials_precede_mutation(component, MutationKind::OpenCreate);
}

TEST(WasiP2FilesystemPublicationTest,
     OpenTruncateHasNoSideEffectForEveryAllocationDenial)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());
    expect_all_denials_precede_mutation(component, MutationKind::OpenTruncate);
}

TEST(WasiP2FilesystemPublicationTest,
     NativeFailuresReplaceStagedSuccessWithoutMutation)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());

    for (MutationKind kind : { MutationKind::CreateDirectoryNativeFailure,
                               MutationKind::DescriptorWriteNativeFailure,
                               MutationKind::OpenNativeFailure }) {
        const PublicationRun failure =
            run_mutation(component, kind, kNoDeniedRequest);
        ASSERT_EQ(failure.stage, 7u);
        ASSERT_TRUE(failure.invoked);
        EXPECT_NE(failure.result_discriminant, 0u);
        EXPECT_TRUE(failure.state_preserved);
        EXPECT_EQ(failure.live_bytes, 0u);
        EXPECT_EQ(failure.live_allocations, 0u);
        EXPECT_EQ(failure.retain_calls, failure.attachment_release_calls);
    }
}

TEST(WasiP2FilesystemPublicationTest,
     DirectoryEntryAllocationDenialsPreserveTheEntryForRetry)
{
    const std::vector<uint8_t> component = read_filesystem_component();
    ASSERT_FALSE(component.empty());
    const DirectoryRetryRun baseline =
        run_directory_entry_read(component, kNoDeniedRequest);
    ASSERT_EQ(baseline.stage, 6u);
    ASSERT_TRUE(baseline.invoked);
    ASSERT_GT(baseline.execution_requests, 0u);
    EXPECT_EQ(baseline.live_bytes, 0u);
    EXPECT_EQ(baseline.live_allocations, 0u);
    EXPECT_EQ(baseline.retain_calls, baseline.attachment_release_calls);

    for (uint32_t denied_request = 0;
         denied_request < baseline.execution_requests; denied_request++) {
        const DirectoryRetryRun denied =
            run_directory_entry_read(component, denied_request);
        ASSERT_EQ(denied.stage, 6u) << "denied request " << denied_request;
        EXPECT_GE(denied.denied_calls, 1u)
            << "denied request " << denied_request;
        EXPECT_FALSE(denied.invoked) << "denied request " << denied_request;
        EXPECT_TRUE(denied.first_entry_preserved)
            << "directory cursor advanced at denied request " << denied_request;
        EXPECT_EQ(denied.live_bytes, 0u) << "denied request " << denied_request;
        EXPECT_EQ(denied.live_allocations, 0u)
            << "denied request " << denied_request;
        EXPECT_EQ(denied.retain_calls, denied.attachment_release_calls)
            << "denied request " << denied_request;
    }
}

} // namespace
