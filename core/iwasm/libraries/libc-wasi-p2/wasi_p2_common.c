/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_common.h"
#include "wasi_p2_types.h"
#include "component-model/wasm_component_resource.h"
#include "component-model/wasm_component_resource_table.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>

bool
wasi_p2_native_fd_quota_reserve(wasm_exec_env_t exec_env, uint32_t fd_count,
                                WasiP2NativeFdQuotaLease *out_lease)
{
    const struct InstantiationArgs2 *args = NULL;

    if (!out_lease)
        return false;
    memset(out_lease, 0, sizeof(*out_lease));
    if (fd_count == 0 || !exec_env || !exec_env->component_inst)
        return true;

    args = &exec_env->component_inst->instantiation_args;
    if (!args->native_fd_quota_reserve || !args->native_fd_quota_release)
        return true;
    if (!args->native_fd_quota_reserve(args->native_fd_quota_attachment,
                                       fd_count))
        return false;

    out_lease->attachment = args->native_fd_quota_attachment;
    out_lease->release_callback = args->native_fd_quota_release;
    out_lease->fd_count = fd_count;
    return true;
}

bool
wasi_p2_native_fd_quota_lease_take(WasiP2NativeFdQuotaLease *lease,
                                   uint32_t fd_count,
                                   WasiP2NativeFdQuotaLease *out_lease)
{
    if (!lease || !out_lease || lease == out_lease)
        return false;
    if (fd_count > lease->fd_count) {
        if (!lease->release_callback) {
            memset(out_lease, 0, sizeof(*out_lease));
            return true;
        }
        return false;
    }
    memset(out_lease, 0, sizeof(*out_lease));
    if (fd_count == 0)
        return true;
    out_lease->attachment = lease->attachment;
    out_lease->release_callback = lease->release_callback;
    out_lease->fd_count = fd_count;
    lease->fd_count -= fd_count;
    if (lease->fd_count == 0) {
        lease->attachment = NULL;
        lease->release_callback = NULL;
    }
    return true;
}

void
wasi_p2_native_fd_quota_release(WasiP2NativeFdQuotaLease *lease)
{
    void *attachment;
    wasm_native_fd_quota_release_callback_t release_callback;
    uint32_t fd_count;

    if (!lease || !lease->release_callback || lease->fd_count == 0)
        return;
    attachment = lease->attachment;
    release_callback = lease->release_callback;
    fd_count = lease->fd_count;
    memset(lease, 0, sizeof(*lease));
    release_callback(attachment, fd_count);
}

void
wasi_p2_native_fd_quota_transfer_to_host_resource(
    HostResource *resource, WasiP2NativeFdQuotaLease *lease)
{
    if (!resource || !lease || !lease->release_callback || lease->fd_count == 0)
        return;
    host_resource_set_native_fd_quota(resource, lease->attachment,
                                      lease->release_callback, lease->fd_count);
    memset(lease, 0, sizeof(*lease));
}

static bool
drop_lowered_builtin_wasi_rep(WASMComponentResourceTable *table, uint32_t rep)
{
    uint32_t i;
    uint32_t scan_limit;

    if (!table || !table->array || rep == 0) {
        return false;
    }

    scan_limit = table->next_index < table->array_size ? table->next_index
                                                       : table->array_size;
    for (i = 1; i < scan_limit; i++) {
        WASMTableElement *element = table->array[i];
        WASMResourceHandle *handle;

        if (!element || element->transaction
            || element->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
            || !element->ptr) {
            continue;
        }
        handle = (WASMResourceHandle *)element->ptr;
        if (!handle->own || handle->rep != rep || !handle->rt
            || !handle->rt->is_builtin_wasi) {
            continue;
        }

        return wasm_component_table_drop_resource(table, i);
    }

    return false;
}

void
wasi_p2_cleanup_failed_owned_host_resources(wasm_exec_env_t exec_env,
                                            const uint32_t *reps,
                                            uint32_t rep_count)
{
    WASMComponentResourceTable *component_table = NULL;
    HostResourceTable *host_table = get_global_host_resource_table();
    uint32_t i;

    if (!reps || rep_count == 0 || !host_table) {
        return;
    }
    if (exec_env && exec_env->cx && exec_env->cx->inst) {
        component_table = exec_env->cx->inst->table;
    }

    for (i = 0; i < rep_count; i++) {
        uint32_t rep = reps[i];

        if (rep == 0) {
            continue;
        }
        if (!drop_lowered_builtin_wasi_rep(component_table, rep)) {
            (void)host_resource_table_delete(host_table, rep);
        }
    }
}

bool
wasi_p2_store_owned_host_resource_result(wasm_exec_env_t exec_env,
                                         uint32_t offset_addr,
                                         WASMComponentTypeInstance *result_type,
                                         wit_value_t result,
                                         const uint32_t *reps,
                                         uint32_t rep_count)
{
    bool stored = false;

    if (exec_env && exec_env->cx && result_type && result) {
        stored = store(exec_env->cx, offset_addr, result_type, result);
    }
    if (!stored) {
        wasi_p2_cleanup_failed_owned_host_resources(exec_env, reps, rep_count);
    }
    return stored;
}

bool
lower_owned_host_resource(wasm_exec_env_t exec_env,
                          WASMComponentResourceHandleInstance *resource_type,
                          HostResourceTable *table, uint32_t rep,
                          uint32_t *out_index)
{
    wit_value_t value;
    uint32_t lowered_index = 0;
    bool lowered;

    if (!exec_env || !exec_env->cx || !resource_type || !table || rep == 0
        || !out_index) {
        if (table && rep != 0) {
            host_resource_table_delete(table, rep);
        }
        return false;
    }

    value = wit_resource_ctor(rep);
    if (!value) {
        host_resource_table_delete(table, rep);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate owned resource value");
        return false;
    }

    lowered = lower_own(exec_env->cx, resource_type, value, &lowered_index);
    free_wit_value(value);
    if (!lowered) {
        host_resource_table_delete(table, rep);
        return false;
    }

    *out_index = lowered_index;
    return true;
}

wasi_network_error_code_t
errno_to_wasi_network(int err)
{
    static const struct {
        int err;
        wasi_network_error_code_t wasi_err;
    } error_map[] = {
        { EACCES, WASI_NETWORK_ERROR_CODE_ACCESS_DENIED },
        { EPERM, WASI_NETWORK_ERROR_CODE_ACCESS_DENIED },

        { EOPNOTSUPP, WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED },
        { ENOTSUP, WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED },

        { EINVAL, WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT },

        { ENOMEM, WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY },
        { ENOBUFS, WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY },
        { EAI_MEMORY, WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY },
        { ENOSPC, WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY },

        { ETIMEDOUT, WASI_NETWORK_ERROR_CODE_TIMEOUT }, // Not explicit in wit

        { EALREADY, WASI_NETWORK_ERROR_CODE_CONCURRENCY_CONFLICT },

        { ESRCH, WASI_NETWORK_ERROR_CODE_NOT_IN_PROGRESS }, // Not explicit in
                                                            // wit, maybe EINVAL

        { EAGAIN, WASI_NETWORK_ERROR_CODE_WOULD_BLOCK },
        { EINPROGRESS, WASI_NETWORK_ERROR_CODE_WOULD_BLOCK },
#if EWOULDBLOCK != EAGAIN
        { EWOULDBLOCK, WASI_NETWORK_ERROR_CODE_WOULD_BLOCK },
#endif

        { ENOTCONN, WASI_NETWORK_ERROR_CODE_INVALID_STATE },

        { EMFILE, WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT },
        { ENFILE, WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT },

        { EADDRNOTAVAIL, WASI_NETWORK_ERROR_CODE_ADDRESS_NOT_BINDABLE },

        { EADDRINUSE, WASI_NETWORK_ERROR_CODE_ADDRESS_IN_USE },

        { ENETUNREACH, WASI_NETWORK_ERROR_CODE_REMOTE_UNREACHABLE },
        { ENETDOWN, WASI_NETWORK_ERROR_CODE_REMOTE_UNREACHABLE },

        { ECONNREFUSED, WASI_NETWORK_ERROR_CODE_CONNECTION_REFUSED },

        { ECONNRESET, WASI_NETWORK_ERROR_CODE_CONNECTION_RESET },
#ifdef ENETRESET
        { ENETRESET, WASI_NETWORK_ERROR_CODE_CONNECTION_RESET },
#endif

        { ECONNABORTED, WASI_NETWORK_ERROR_CODE_CONNECTION_ABORTED },

        { EMSGSIZE, WASI_NETWORK_ERROR_CODE_DATAGRAM_TOO_LARGE },

        { EHOSTUNREACH,
          WASI_NETWORK_ERROR_CODE_NAME_UNRESOLVABLE }, // no true correspondent
                                                       // in errno.h

        { ETIMEDOUT, WASI_NETWORK_ERROR_CODE_TEMPORARY_RESOLVER_FAILURE },

        { EIO,
          WASI_NETWORK_ERROR_CODE_PERMANENT_RESOLVER_FAILURE } // no true
                                                               // correspondent
                                                               // in errno.h
    };

    for (size_t i = 0; i < sizeof(error_map) / sizeof(error_map[0]); i++) {
        if (error_map[i].err == err) {
            return error_map[i].wasi_err;
        }
    }

    return WASI_NETWORK_ERROR_CODE_UNKNOWN;
}

const char *
wasi_network_error_code_to_string(wasi_network_error_code_t error_code)
{
    switch (error_code) {
        case WASI_NETWORK_ERROR_CODE_UNKNOWN:
            return "unknown";
        case WASI_NETWORK_ERROR_CODE_ACCESS_DENIED:
            return "access-denied";
        case WASI_NETWORK_ERROR_CODE_NOT_SUPPORTED:
            return "not-supported";
        case WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT:
            return "invalid-argument";
        case WASI_NETWORK_ERROR_CODE_OUT_OF_MEMORY:
            return "out-of-memory";
        case WASI_NETWORK_ERROR_CODE_TIMEOUT:
            return "timeout";
        case WASI_NETWORK_ERROR_CODE_CONCURRENCY_CONFLICT:
            return "concurrency-conflict";
        case WASI_NETWORK_ERROR_CODE_NOT_IN_PROGRESS:
            return "not-in-progress";
        case WASI_NETWORK_ERROR_CODE_WOULD_BLOCK:
            return "would-block";
        case WASI_NETWORK_ERROR_CODE_INVALID_STATE:
            return "invalid-state";
        case WASI_NETWORK_ERROR_CODE_NEW_SOCKET_LIMIT:
            return "new-socket-limit";
        case WASI_NETWORK_ERROR_CODE_ADDRESS_NOT_BINDABLE:
            return "address-not-bindable";
        case WASI_NETWORK_ERROR_CODE_ADDRESS_IN_USE:
            return "address-in-use";
        case WASI_NETWORK_ERROR_CODE_REMOTE_UNREACHABLE:
            return "remote-unreachable";
        case WASI_NETWORK_ERROR_CODE_CONNECTION_REFUSED:
            return "connection-refused";
        case WASI_NETWORK_ERROR_CODE_CONNECTION_RESET:
            return "connection-reset";
        case WASI_NETWORK_ERROR_CODE_CONNECTION_ABORTED:
            return "connection-aborted";
        case WASI_NETWORK_ERROR_CODE_DATAGRAM_TOO_LARGE:
            return "datagram-too-large";
        case WASI_NETWORK_ERROR_CODE_NAME_UNRESOLVABLE:
            return "name-unresolvable";
        case WASI_NETWORK_ERROR_CODE_TEMPORARY_RESOLVER_FAILURE:
            return "temporary-resolver-failure";
        case WASI_NETWORK_ERROR_CODE_PERMANENT_RESOLVER_FAILURE:
            return "permanent-resolver-failure";
        default:
            return "unknown";
    }
}

/// @brief Copy string from host memory to wasm linear memory
/// @param exec_env exec env holding the wasm linear memory
/// @param wasm_offset offset to wasm linear memory where string will be copied
/// @param str string to be copied
/// @return true of copy was successfull, false otherwise
bool
copy_string_to_wasm(wasm_exec_env_t exec_env, int32_t *wasm_offset,
                    const char *str)
{
    int32_t str_len = strlen(str);
    uint32_t str_offset = 0;
    void *native_ptr = NULL;

    str_offset = wasm_runtime_call_realloc(exec_env->cx, 0, 0, 4, str_len + 1);
    native_ptr =
        wasm_runtime_addr_app_to_native_p2(exec_env, (uint64)str_offset);
    if (str_offset == 0) {
        return false;
    }

    memcpy(native_ptr, str, str_len + 1);

    wasm_offset[0] = (int32_t)str_offset;
    wasm_offset[1] = str_len;

    return true;
}

/// @brief Write IP address information to wasm linear memory
/// @param exec_env execution environment holding the desired wasm linear memory
/// @param is_some true if ip adrress info is not empty
/// @param ip_address ip adrress information to copy
/// @return
int32_t
wasi_p2_write_option_ip_address(wasm_exec_env_t exec_env, bool is_some,
                                const wasi_ip_address_t *ip_address)
{
    uint32_t wasm_offset = 0;
    uint8_t *native_ptr = NULL;

    wasm_offset = wasm_runtime_call_realloc(
        exec_env->cx, 0, 0, 4, is_some ? 1 + sizeof(wasi_ip_address_t) : 1);
    native_ptr = wasm_runtime_addr_app_to_native_p2(exec_env, wasm_offset);
    if (wasm_offset == 0) {
        return 0;
    }

    native_ptr[0] = is_some;
    if (is_some) {
        memcpy(native_ptr + 1, ip_address, sizeof(wasi_ip_address_t));
    }

    return (int32_t)wasm_offset;
}

/// @brief Copy string from wasm linear memory to host meory
/// @param exec_env execution environment holding the desired wasm linear memory
/// @param wasm_str_ptr address of string to be copied
/// @param wasm_str_len size of string
/// @return memory address where the string was written
char *
copy_wasm_string_to_native(wasm_exec_env_t exec_env,
                           const int32_t *wasm_str_ptr, int32_t wasm_str_len)
{
    char *native_str = wasm_runtime_malloc(wasm_str_len + 1);
    if (!native_str) {
        return NULL;
    }

    memcpy(native_str, wasm_str_ptr, wasm_str_len);
    native_str[wasm_str_len] = '\0';
    return native_str;
}

StringEncoding
wasm_get_string_encoding(WASMExecEnv *exec_env)
{
    StringEncoding target_encoding;

    if (!exec_env || !exec_env->cx || !exec_env->cx->canonical_opts
        || !exec_env->cx->canonical_opts->lift_lower_opts
        || !exec_env->cx->canonical_opts->lift_lower_opts->lift_opts) {
        return ENCODING_UTF_8;
    }

    target_encoding = exec_env->cx->canonical_opts->lift_lower_opts->lift_opts
                          ->string_encoding;
    switch (target_encoding) {
        case ENCODING_UTF_8:
        case ENCODING_UTF_16:
        case ENCODING_LATIN_1_UTF_16:
            /* Wrapper-created ComponentWITString values are host-side UTF-8.
               The canonical store path subsequently transcodes them to this
               validated target encoding. Returning the target encoding here
               would pre-encode and then double-encode UTF-16/compact strings.
             */
            return ENCODING_UTF_8;
        default:
            if (exec_env->module_inst) {
                wasm_runtime_set_exception(exec_env->module_inst,
                                           "unknown canonical string encoding");
            }
            return ENCODING_UTF_8;
    }
}

wasi_filesystem_error_code_t
errno_to_wasi_filesystem(int err)
{
    static const struct {
        int err;
        wasi_filesystem_error_code_t wasi_err;
    } error_map[] = {
        { EACCES, WASI_FILESYSTEM_CODE_ACCESS },
        { EAGAIN, WASI_FILESYSTEM_CODE_WOULD_BLOCK },
        { EWOULDBLOCK, WASI_FILESYSTEM_CODE_WOULD_BLOCK },
        { EALREADY, WASI_FILESYSTEM_CODE_ALREADY },
        { EBADF, WASI_FILESYSTEM_CODE_BAD_DESCRIPTOR },
        { EBUSY, WASI_FILESYSTEM_CODE_BUSY },
        { EDEADLK, WASI_FILESYSTEM_CODE_DEADLOCK },
        { EDQUOT, WASI_FILESYSTEM_CODE_QUOTA },
        { EEXIST, WASI_FILESYSTEM_CODE_EXISTS },
        { EFBIG, WASI_FILESYSTEM_CODE_FILE_TOO_LARGE },
        { EILSEQ, WASI_FILESYSTEM_CODE_ILLEGAL_BYTE_SEQUENCE },
        { EINPROGRESS, WASI_FILESYSTEM_CODE_IN_PROGRESS },
        { EINTR, WASI_FILESYSTEM_CODE_INTERRUPTED },
        { EINVAL, WASI_FILESYSTEM_CODE_INVALID },
        { EIO, WASI_FILESYSTEM_CODE_IO },
        { EISDIR, WASI_FILESYSTEM_CODE_IS_DIRECTORY },
        { ELOOP, WASI_FILESYSTEM_CODE_LOOP },
        { EMLINK, WASI_FILESYSTEM_CODE_TOO_MANY_LINKS },
        { EMSGSIZE, WASI_FILESYSTEM_CODE_MESSAGE_SIZE },
        { ENAMETOOLONG, WASI_FILESYSTEM_CODE_NAME_TOO_LONG },
        { ENODEV, WASI_FILESYSTEM_CODE_NO_DEVICE },
        { ENOENT, WASI_FILESYSTEM_CODE_NO_ENTRY },
        { ENOLCK, WASI_FILESYSTEM_CODE_NO_LOCK },
        { ENOMEM, WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY },
        { ENOSPC, WASI_FILESYSTEM_CODE_INSUFFICIENT_SPACE },
        { ENOTDIR, WASI_FILESYSTEM_CODE_NOT_DIRECTORY },
        { ENOTEMPTY, WASI_FILESYSTEM_CODE_NOT_EMPTY },
        { ENOTRECOVERABLE, WASI_FILESYSTEM_CODE_NOT_RECOVERABLE },
        { ENOTSUP, WASI_FILESYSTEM_CODE_UNSUPPORTED },
        { ENOSYS, WASI_FILESYSTEM_CODE_UNSUPPORTED },
        { ENOTTY, WASI_FILESYSTEM_CODE_NO_TTY },
        { ENXIO, WASI_FILESYSTEM_CODE_NO_SUCH_DEVICE },
        { EOVERFLOW, WASI_FILESYSTEM_CODE_OVERFLOW },
        { EPERM, WASI_FILESYSTEM_CODE_NOT_PERMITTED },
        { EPIPE, WASI_FILESYSTEM_CODE_PIPE },
        { EROFS, WASI_FILESYSTEM_CODE_READ_ONLY },
        { ESPIPE, WASI_FILESYSTEM_CODE_INVALID_SEEK },
        { ETXTBSY, WASI_FILESYSTEM_CODE_TEXT_FILE_BUSY },
        { EXDEV, WASI_FILESYSTEM_CODE_CROSS_DEVICE },
    };

    for (size_t i = 0; i < sizeof(error_map) / sizeof(error_map[0]); i++) {
        if (error_map[i].err == err) {
            return error_map[i].wasi_err;
        }
    }

    return WASI_FILESYSTEM_CODE_INVALID;
}

const char *
wasi_filesystem_error_code_to_string(wasi_filesystem_error_code_t error_code)
{
    switch (error_code) {
        case WASI_FILESYSTEM_CODE_ACCESS:
            return "access";
        case WASI_FILESYSTEM_CODE_WOULD_BLOCK:
            return "would-block";
        case WASI_FILESYSTEM_CODE_ALREADY:
            return "already";
        case WASI_FILESYSTEM_CODE_BAD_DESCRIPTOR:
            return "bad-descriptor";
        case WASI_FILESYSTEM_CODE_BUSY:
            return "busy";
        case WASI_FILESYSTEM_CODE_DEADLOCK:
            return "deadlock";
        case WASI_FILESYSTEM_CODE_QUOTA:
            return "quota";
        case WASI_FILESYSTEM_CODE_EXISTS:
            return "exist";
        case WASI_FILESYSTEM_CODE_FILE_TOO_LARGE:
            return "file-too-large";
        case WASI_FILESYSTEM_CODE_ILLEGAL_BYTE_SEQUENCE:
            return "illegal-byte-sequence";
        case WASI_FILESYSTEM_CODE_IN_PROGRESS:
            return "in-progress";
        case WASI_FILESYSTEM_CODE_INTERRUPTED:
            return "interrupted";
        case WASI_FILESYSTEM_CODE_INVALID:
            return "invalid";
        case WASI_FILESYSTEM_CODE_IO:
            return "io";
        case WASI_FILESYSTEM_CODE_IS_DIRECTORY:
            return "is-directory";
        case WASI_FILESYSTEM_CODE_LOOP:
            return "loop";
        case WASI_FILESYSTEM_CODE_TOO_MANY_LINKS:
            return "too-many-links";
        case WASI_FILESYSTEM_CODE_MESSAGE_SIZE:
            return "message-size";
        case WASI_FILESYSTEM_CODE_NAME_TOO_LONG:
            return "name-too-long";
        case WASI_FILESYSTEM_CODE_NO_DEVICE:
            return "no-device";
        case WASI_FILESYSTEM_CODE_NO_ENTRY:
            return "no-entry";
        case WASI_FILESYSTEM_CODE_NO_LOCK:
            return "no-lock";
        case WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY:
            return "insufficient-memory";
        case WASI_FILESYSTEM_CODE_INSUFFICIENT_SPACE:
            return "insufficient-space";
        case WASI_FILESYSTEM_CODE_NOT_DIRECTORY:
            return "not-directory";
        case WASI_FILESYSTEM_CODE_NOT_EMPTY:
            return "not-empty";
        case WASI_FILESYSTEM_CODE_NOT_RECOVERABLE:
            return "not-recoverable";
        case WASI_FILESYSTEM_CODE_UNSUPPORTED:
            return "unsupported";
        case WASI_FILESYSTEM_CODE_NO_TTY:
            return "no-tty";
        case WASI_FILESYSTEM_CODE_NO_SUCH_DEVICE:
            return "no-such-device";
        case WASI_FILESYSTEM_CODE_OVERFLOW:
            return "overflow";
        case WASI_FILESYSTEM_CODE_NOT_PERMITTED:
            return "not-permitted";
        case WASI_FILESYSTEM_CODE_PIPE:
            return "pipe";
        case WASI_FILESYSTEM_CODE_READ_ONLY:
            return "read-only";
        case WASI_FILESYSTEM_CODE_INVALID_SEEK:
            return "invalid-seek";
        case WASI_FILESYSTEM_CODE_TEXT_FILE_BUSY:
            return "text-file-busy";
        case WASI_FILESYSTEM_CODE_CROSS_DEVICE:
            return "cross-device";
        default:
            return "invalid";
    }
}

wit_value_t
get_result_error_val(uint32_t error_code)
{
    wit_value_t error_val = wit_enum_ctor(error_code);
    wit_value_t result;

    if (!error_val)
        return NULL;
    result = wit_result_ctor(true, error_val);
    if (!result)
        free_wit_value(error_val);
    return result;
}

wit_value_t
get_datetime(uint64_t seconds, uint32_t nanoseconds)
{
    wit_value_t seconds_val = wit_u64_ctor(seconds);
    wit_value_t nanoseconds_val = wit_u32_ctor(nanoseconds);
    ComponentWITRecordField *datetime_fields = NULL;
    wit_value_t datetime = NULL;

    if (!seconds_val || !nanoseconds_val)
        goto fail;
    datetime_fields = (ComponentWITRecordField *)wasm_runtime_calloc(
        2, sizeof(ComponentWITRecordField));
    if (!datetime_fields)
        goto fail;
    init_record_field(&datetime_fields[0], "seconds", 7, seconds_val);
    if (!datetime_fields[0].key)
        goto fail;
    seconds_val = NULL;
    init_record_field(&datetime_fields[1], "nanoseconds", 11, nanoseconds_val);
    if (!datetime_fields[1].key)
        goto fail;
    nanoseconds_val = NULL;

    datetime = wit_record_ctor(datetime_fields, 2);
    if (!datetime)
        goto fail;
    return datetime;

fail:
    free_wit_value(seconds_val);
    free_wit_value(nanoseconds_val);
    if (datetime_fields) {
        free_wit_value(datetime_fields[0].value);
        wasm_runtime_free(datetime_fields[0].key);
        free_wit_value(datetime_fields[1].value);
        wasm_runtime_free(datetime_fields[1].key);
        wasm_runtime_free(datetime_fields);
    }
    return NULL;
}

wit_value_t
get_result_datetime(uint64_t seconds, uint32_t nanoseconds)
{
    wit_value_t datetime_val = get_datetime(seconds, nanoseconds);
    wit_value_t result;

    if (!datetime_val)
        return NULL;
    result = wit_result_ctor(false, datetime_val);
    if (!result)
        free_wit_value(datetime_val);
    return result;
}
