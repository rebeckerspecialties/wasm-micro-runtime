/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* splice(2) below is a GNU extension; request it before any system header
   (see wasi_p2_io.c). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "wasi_p2_io_wrapper.h"
#include "wasi_p2_io.h"
#include "wasi_p2_error.h"
#include "wasi_p2_host_io.h"
#include "wasm_runtime_common.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_canonical.h"
#include "wasi_p2_common.h"
#include "bh_common.h"
#include "wasm_component_runtime.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"

static bool
array_allocation_fits(uint64_t count, size_t element_size)
{
    return element_size != 0 && element_size <= UINT32_MAX
           && count <= UINT32_MAX / element_size;
}

static bool
native_stream_length_fits(int64_t len)
{
    return len >= 0 && (uint64_t)len <= UINT32_MAX;
}

static wit_value_t
stream_error_result_ctor(wasm_module_inst_t module_inst,
                         IOStreamType stream_type,
                         const wasi_stream_error_t *stream_error,
                         uint32_t *owned_error_rep);

typedef enum StreamPublicationKind {
    STREAM_PUBLICATION_LIST_U8,
    STREAM_PUBLICATION_U64,
    STREAM_PUBLICATION_UNIT,
} StreamPublicationKind;

/*
 * A stream operation may irreversibly consume input, invoke a host callback,
 * advance a logical file cursor, or emit output.  Keep every fallible host/WIT
 * allocation, owned-error lowering, and guest realloc ahead of that boundary.
 *
 * The guest return area contains a staged last-operation-failed result while
 * the native operation runs.  A savepoint records the owned error handle in
 * the caller's canonical lower transaction without creating or completing a
 * nested transaction.  A native error retains it; success or closed rolls
 * back only to that savepoint and replaces the already validated fixed-width
 * return bytes.  No allocator or guest code is called by
 * finish_stream_publication().
 */
typedef struct StreamPublication {
    wasm_exec_env_t exec_env;
    WASMComponentTypeInstance *result_type;
    CanonicalResourceTransferScope lower_scope;
    CanonicalResourceTransferSavepoint lower_savepoint;
    wit_value_t error_result;
    WASMMemoryInstance *memory;
    IOStreamType stream_type;
    StreamPublicationKind kind;
    uint32_t result_offset;
    uint32_t payload_offset;
    uint32_t error_variant_offset;
    uint32_t closed_case;
    uint32_t error_rep;
    uint32_t list_ptr;
    uint32_t list_capacity;
    bool lower_scope_active;
    bool lower_savepoint_active;
} StreamPublication;

static bool
stream_publication_write_int(StreamPublication *publication, uint32_t offset,
                             uint32_t bytes, uint64_t value)
{
    uint32_t i;

    if (!publication || !publication->memory || bytes == 0 || bytes > 8
        || (uint64_t)offset + bytes > publication->memory->memory_data_size) {
        return false;
    }
    for (i = 0; i < bytes; i++) {
        publication->memory->memory_data[offset + i] =
            (uint8_t)(value >> (i * 8));
    }
    return true;
}

static bool
stream_publication_aligned_offset(uint32_t base, uint32_t alignment,
                                  uint32_t *out)
{
    uint64_t aligned;

    if (!out || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    aligned = ((uint64_t)base + alignment - 1) & ~((uint64_t)alignment - 1);
    if (aligned > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)aligned;
    return true;
}

static bool
stream_publication_find_error_cases(const WASMComponentVariantInstance *variant,
                                    uint32_t *closed_case,
                                    uint32_t *last_operation_failed_case)
{
    uint32_t i;
    bool found_closed = false;
    bool found_last_operation_failed = false;

    if (!variant || !closed_case || !last_operation_failed_case) {
        return false;
    }
    for (i = 0; i < variant->count; i++) {
        const WASMComponentCoreName *label = variant->cases[i].label;

        if (!label || !label->name) {
            continue;
        }
        if (label->name_len == 6 && memcmp(label->name, "closed", 6) == 0) {
            *closed_case = i;
            found_closed = true;
        }
        else if (label->name_len == 21
                 && memcmp(label->name, "last-operation-failed", 21) == 0) {
            *last_operation_failed_case = i;
            found_last_operation_failed = true;
        }
    }
    return found_closed && found_last_operation_failed;
}

static bool
stream_publication_validate_type(StreamPublication *publication,
                                 WASMComponentTypeInstance *result_type,
                                 StreamPublicationKind kind,
                                 uint32_t result_offset)
{
    WASMComponentResultInstance *result;
    WASMComponentTypeInstance *ok_type;
    WASMComponentTypeInstance *error_type;
    WASMComponentVariantInstance *error_variant;
    WASMComponentTypeInstance *last_operation_failed_type;
    uint32_t last_operation_failed_case = 0;
    uint32_t payload_alignment = 1;
    uint32_t error_payload_alignment = 1;

    if (!publication || !result_type
        || result_type->type != COMPONENT_VAL_TYPE_RESULT
        || !(result = result_type->type_specific.result)
        || !(error_type = result->error_type)
        || error_type->type != COMPONENT_VAL_TYPE_VARIANT
        || !(error_variant = error_type->type_specific.variant)
        || !stream_publication_find_error_cases(error_variant,
                                                &publication->closed_case,
                                                &last_operation_failed_case)) {
        return false;
    }

    ok_type = result->result_type;
    if ((kind == STREAM_PUBLICATION_UNIT && ok_type != NULL)
        || (kind != STREAM_PUBLICATION_UNIT && ok_type == NULL)) {
        return false;
    }
    if (kind == STREAM_PUBLICATION_U64
        && (ok_type->type != COMPONENT_VAL_TYPE_PRIMVAL
            || ok_type->type_specific.primval != WASM_COMP_PRIMVAL_U64)) {
        return false;
    }
    if (kind == STREAM_PUBLICATION_LIST_U8
        && (ok_type->type != COMPONENT_VAL_TYPE_LIST
            || !ok_type->type_specific.list
            || !ok_type->type_specific.list->element_type
            || ok_type->type_specific.list->element_type->type
                   != COMPONENT_VAL_TYPE_PRIMVAL
            || ok_type->type_specific.list->element_type->type_specific.primval
                   != WASM_COMP_PRIMVAL_U8)) {
        return false;
    }

    last_operation_failed_type =
        error_variant->cases[last_operation_failed_case].value_type;
    if (!last_operation_failed_type
        || last_operation_failed_type->type != COMPONENT_VAL_TYPE_OWN) {
        return false;
    }

    if (result_offset != align_to(result_offset, result_type->alignment)
        || (uint64_t)result_offset + result_type->elem_size
               > publication->memory->memory_data_size) {
        return false;
    }
    if (ok_type) {
        payload_alignment = ok_type->alignment;
    }
    if (error_type->alignment > payload_alignment) {
        payload_alignment = error_type->alignment;
    }
    if (!stream_publication_aligned_offset(
            result_offset + compute_discriminant_alignment(2),
            payload_alignment, &publication->payload_offset)) {
        return false;
    }
    publication->error_variant_offset = publication->payload_offset;
    error_payload_alignment =
        compute_max_case_alignment(error_type->type_specific.variant);
    if (!stream_publication_aligned_offset(
            publication->error_variant_offset
                + compute_discriminant_alignment(error_variant->count),
            error_payload_alignment, &publication->error_variant_offset)) {
        return false;
    }
    return true;
}

static void
abort_stream_publication(StreamPublication *publication)
{
    HostResourceTable *table;

    if (!publication) {
        return;
    }
    if (publication->lower_savepoint_active) {
        (void)canonical_resource_transfer_savepoint_rollback(
            &publication->lower_savepoint);
        publication->lower_savepoint_active = false;
    }
    if (publication->lower_scope_active) {
        bool leave_success = !publication->lower_scope.outer;

        /* A nested publication owns only the savepoint entries rolled back
         * above.  Failing the shared scope here would poison the caller's
         * ambient canonical transaction even though its prefix is intact.
         * A locally-created outer scope still must fail and roll back. */
        (void)canonical_resource_transfer_scope_leave(&publication->lower_scope,
                                                      leave_success);
        publication->lower_scope_active = false;
    }
    if (publication->error_rep != 0
        && (table = get_global_host_resource_table()) != NULL) {
        (void)host_resource_table_delete(table, publication->error_rep);
        publication->error_rep = 0;
    }
    free_wit_value(publication->error_result);
    publication->error_result = NULL;
}

static bool
prepare_stream_publication(wasm_exec_env_t exec_env, uint32_t result_offset,
                           WASMComponentTypeInstance *result_type,
                           IOStreamType stream_type, StreamPublicationKind kind,
                           uint32_t list_capacity,
                           StreamPublication *publication)
{
    wasi_stream_error_t placeholder_error = {
        .kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
        .payload.error = EINVAL,
    };
    uint32_t allocation_size;

    if (!publication || !exec_env || !exec_env->cx) {
        return false;
    }
    memset(publication, 0, sizeof(*publication));
    publication->exec_env = exec_env;
    publication->result_type = result_type;
    publication->memory = get_mem_from_cx(exec_env->cx);
    publication->stream_type = stream_type;
    publication->kind = kind;
    publication->result_offset = result_offset;
    publication->list_capacity = list_capacity;

    if (!publication->memory
        || !stream_publication_validate_type(publication, result_type, kind,
                                             result_offset)) {
        LOG_ERROR("stream publication type validation failed: kind=%u type=%u",
                  (unsigned)kind,
                  result_type ? (unsigned)result_type->type : UINT32_MAX);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Invalid stream result publication type");
        return false;
    }

    if (kind == STREAM_PUBLICATION_LIST_U8) {
        /* cabi_realloc cannot use a null result as a successful allocation.
           Reserve one byte for an empty read and publish a logical length of
           zero until the native read completes. */
        allocation_size = list_capacity == 0 ? 1 : list_capacity;
        publication->list_ptr = (uint32_t)wasm_runtime_call_realloc(
            exec_env->cx, 0, 0, 1, (int32_t)allocation_size);
        if (publication->list_ptr == 0
            || (uint64_t)publication->list_ptr + allocation_size
                   > publication->memory->memory_data_size) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "Could not reserve stream read result");
            return false;
        }
    }

    /* Validate and stage the success representation before the owned error
       overwrites it. The same fixed offsets are reused after the operation. */
    if (!stream_publication_write_int(publication, result_offset,
                                      compute_discriminant_alignment(2), 0)
        || (kind == STREAM_PUBLICATION_LIST_U8
            && (!stream_publication_write_int(publication,
                                              publication->payload_offset, 4,
                                              publication->list_ptr)
                || !stream_publication_write_int(
                    publication, publication->payload_offset + 4, 4, 0)))
        || (kind == STREAM_PUBLICATION_U64
            && !stream_publication_write_int(
                publication, publication->payload_offset, 8, 0))) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not stage stream success result");
        return false;
    }

    publication->error_result =
        stream_error_result_ctor(exec_env->module_inst, stream_type,
                                 &placeholder_error, &publication->error_rep);
    if (!publication->error_result) {
        abort_stream_publication(publication);
        return false;
    }
    if (!canonical_resource_transfer_scope_enter(
            &publication->lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not start stream publication");
        abort_stream_publication(publication);
        return false;
    }
    publication->lower_scope_active = true;
    if (!canonical_resource_transfer_savepoint_begin(
            &publication->lower_savepoint, exec_env->cx)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not start stream publication");
        abort_stream_publication(publication);
        return false;
    }
    publication->lower_savepoint_active = true;
    if (!store(exec_env->cx, result_offset, result_type,
               publication->error_result)
        || !canonical_resource_transfer_savepoint_can_commit(
            &publication->lower_savepoint)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not stage stream error result");
        abort_stream_publication(publication);
        return false;
    }
    return true;
}

static bool
finish_stream_publication(StreamPublication *publication,
                          const wasi_stream_error_t *error,
                          const uint8_t *read_bytes, uint64_t success_value)
{
    HostResourceTable *table;
    HostResource *error_resource;
    WasiErrorResource *error_data;
    bool native_error = error != NULL;
    bool last_operation_failed =
        native_error
        && error->kind == WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
    bool transaction_succeeded;

    if (!publication || !publication->lower_scope_active
        || !publication->lower_savepoint_active) {
        return false;
    }

    table = get_global_host_resource_table();
    if (last_operation_failed) {
        error_resource =
            table ? host_resource_table_get(table, publication->error_rep)
                  : NULL;
        if (!error_resource || error_resource->type != WASI_P2_ERROR
            || !error_resource->data) {
            wasm_runtime_set_exception(publication->exec_env->module_inst,
                                       "Lost staged stream error resource");
            abort_stream_publication(publication);
            return false;
        }
        error_data = (WasiErrorResource *)error_resource->data;
        error_data->type = publication->stream_type;
        error_data->error_code =
            publication->stream_type == STREAM_TYPE_SOCKET
                ? (int32_t)errno_to_wasi_network(error->payload.error)
                : (int32_t)errno_to_wasi_filesystem(error->payload.error);
        transaction_succeeded = canonical_resource_transfer_savepoint_commit(
            &publication->lower_savepoint);
        publication->lower_savepoint_active = false;
        transaction_succeeded =
            canonical_resource_transfer_scope_leave(&publication->lower_scope,
                                                    transaction_succeeded)
            && transaction_succeeded;
        publication->lower_scope_active = false;
        if (!transaction_succeeded) {
            wasm_runtime_set_exception(publication->exec_env->module_inst,
                                       "Could not publish stream error result");
            abort_stream_publication(publication);
            return false;
        }
        publication->error_rep = 0;
        free_wit_value(publication->error_result);
        publication->error_result = NULL;
        return true;
    }

    /* can_commit() validated the pending ownership graph before the native
       operation. Rollback is allocation-free and retires the unpublished
       placeholder handle before any success bytes become guest-visible. */
    transaction_succeeded = canonical_resource_transfer_savepoint_rollback(
        &publication->lower_savepoint);
    publication->lower_savepoint_active = false;
    transaction_succeeded =
        canonical_resource_transfer_scope_leave(&publication->lower_scope,
                                                transaction_succeeded)
        && transaction_succeeded;
    publication->lower_scope_active = false;
    if (!transaction_succeeded) {
        wasm_runtime_set_exception(publication->exec_env->module_inst,
                                   "Could not retire staged stream error");
        abort_stream_publication(publication);
        return false;
    }
    if (publication->error_rep != 0 && table) {
        (void)host_resource_table_delete(table, publication->error_rep);
        publication->error_rep = 0;
    }

    if (native_error) {
        if (!stream_publication_write_int(publication,
                                          publication->result_offset,
                                          compute_discriminant_alignment(2), 1)
            || !stream_publication_write_int(
                publication, publication->payload_offset,
                compute_discriminant_alignment(
                    publication->result_type->type_specific.result->error_type
                        ->type_specific.variant->count),
                publication->closed_case)) {
            wasm_runtime_set_exception(
                publication->exec_env->module_inst,
                "Could not publish closed stream result");
            abort_stream_publication(publication);
            return false;
        }
    }
    else {
        if (publication->kind == STREAM_PUBLICATION_LIST_U8) {
            if (success_value > publication->list_capacity
                || (success_value > 0 && !read_bytes)) {
                wasm_runtime_set_exception(
                    publication->exec_env->module_inst,
                    "Native stream read exceeded reserved result");
                abort_stream_publication(publication);
                return false;
            }
            if (success_value > 0) {
                memcpy(publication->memory->memory_data + publication->list_ptr,
                       read_bytes, (size_t)success_value);
            }
        }
        if (!stream_publication_write_int(publication,
                                          publication->result_offset,
                                          compute_discriminant_alignment(2), 0)
            || (publication->kind == STREAM_PUBLICATION_LIST_U8
                && (!stream_publication_write_int(publication,
                                                  publication->payload_offset,
                                                  4, publication->list_ptr)
                    || !stream_publication_write_int(
                        publication, publication->payload_offset + 4, 4,
                        success_value)))
            || (publication->kind == STREAM_PUBLICATION_U64
                && !stream_publication_write_int(publication,
                                                 publication->payload_offset, 8,
                                                 success_value))) {
            wasm_runtime_set_exception(
                publication->exec_env->module_inst,
                "Could not publish stream success result");
            abort_stream_publication(publication);
            return false;
        }
    }

    free_wit_value(publication->error_result);
    publication->error_result = NULL;
    return true;
}

static uint32_t
stream_read_capacity(uint64_t requested)
{
    return requested < WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES
               ? (uint32_t)requested
               : WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES;
}

static bool
init_owned_fd_pollable(wasm_exec_env_t exec_env, HostResource *resource,
                       int source_fd, wasi_pollable_type_t type)
{
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    int poll_fd;

    if (!resource || !resource->data || source_fd < 0
        || !wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        return false;
    }
    poll_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (poll_fd < 0) {
        wasi_p2_native_fd_quota_release(&fd_lease);
        return false;
    }
    SET_POLLABLE_CTX((wasi_pollable_context_t *)resource->data, poll_fd, true,
                     type);
    wasi_p2_native_fd_quota_transfer_to_host_resource(resource, &fd_lease);
    return true;
}

static void
free_wit_value_array(wit_value_t *elems, uint32_t count)
{
    if (!elems) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        free_wit_value(elems[i]);
    }
    wasm_runtime_free(elems);
}

/*
 * Construct result<_, stream-error> transactionally.  wasi:io/error is an
 * owned resource, so a failed WIT allocation must also remove the native host
 * resource instead of leaving it reachable only through the global table.
 */
static wit_value_t
stream_error_result_ctor(wasm_module_inst_t module_inst,
                         IOStreamType stream_type,
                         const wasi_stream_error_t *stream_error,
                         uint32_t *owned_error_rep)
{
    HostResourceTable *hr_table = get_global_host_resource_table();
    wit_value_t payload = NULL;
    wit_value_t variant = NULL;
    wit_value_t result = NULL;
    uint32_t error_rep = 0;
    uint32_t error_code = 0;
    bool is_closed =
        stream_error && stream_error->kind == WASI_STREAM_ERROR_KIND_CLOSED;

    if (owned_error_rep) {
        *owned_error_rep = 0;
    }
    if (!stream_error || !owned_error_rep || !hr_table) {
        goto fail;
    }

    if (is_closed) {
        variant = wit_variant_ctor("closed", 6, NULL);
    }
    else {
        error_code =
            stream_type == STREAM_TYPE_SOCKET
                ? (uint32_t)errno_to_wasi_network(stream_error->payload.error)
                : (uint32_t)errno_to_wasi_filesystem(
                    stream_error->payload.error);
        error_rep = wasi_error_new(stream_type, error_code);
        if (error_rep == 0) {
            goto fail;
        }
        payload = wit_resource_ctor(error_rep);
        if (!payload) {
            goto fail;
        }
        variant = wit_variant_ctor("last-operation-failed", 21, payload);
        if (!variant) {
            goto fail;
        }
        payload = NULL;
    }
    if (!variant) {
        goto fail;
    }

    result = wit_result_ctor(true, variant);
    if (!result) {
        goto fail;
    }
    variant = NULL;
    *owned_error_rep = error_rep;
    return result;

fail:
    free_wit_value(payload);
    free_wit_value(variant);
    if (error_rep != 0 && hr_table) {
        (void)host_resource_table_delete(hr_table, error_rep);
    }
    wasm_runtime_set_exception(module_inst,
                               "Could not allocate stream error result");
    return NULL;
}

static wit_value_t
invalid_stream_error_result_ctor(wasm_module_inst_t module_inst,
                                 uint32_t *owned_error_rep)
{
    wasi_stream_error_t stream_error = {
        .kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
        .payload.error = EINVAL,
    };

    return stream_error_result_ctor(module_inst, STREAM_TYPE_FILE,
                                    &stream_error, owned_error_rep);
}

static void
free_input_stream_result(wit_value_t result)
{
    free_wit_value(result);
}

static wit_value_t
stream_u64_result_ctor(wasm_module_inst_t module_inst, uint64_t value)
{
    wit_value_t payload = wit_u64_ctor(value);
    wit_value_t result;

    if (!payload) {
        goto fail;
    }
    result = wit_result_ctor(false, payload);
    if (!result) {
        free_wit_value(payload);
        goto fail;
    }
    return result;

fail:
    wasm_runtime_set_exception(module_inst, "Could not allocate stream result");
    return NULL;
}

static bool
store_stream_result(wasm_exec_env_t exec_env, uint32_t offset_addr,
                    WASMComponentTypeInstance *result_type, wit_value_t result,
                    uint32_t owned_error_rep)
{
    if (!result) {
        return false;
    }
    if (owned_error_rep != 0) {
        return wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, result_type, result, &owned_error_rep, 1);
    }
    return store(exec_env->cx, offset_addr, result_type, result);
}

/* wasi:io/error */

/**
 * @brief Wrapper for the `to-debug-string` method of the `wasi:io/error.error`
 * resource.
 * @details This function retrieves a human-readable string representation of an
 * error and copies it into the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param error_handle The handle of the error resource.
 * @param[out] offset_addr Memory's offset where the string wit_value_t will be
 * stored.
 */
void
wasi_io_error_to_debug_string_wrapper(wasm_exec_env_t exec_env,
                                      uint32_t error_handle,
                                      uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    StringEncoding encoding = exec_env->cx->canonical_opts->lift_lower_opts
                                  ->lift_opts->string_encoding;

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = wit_string_ctor("", 0, 0, encoding);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, error_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = wit_string_ctor("", 0, 0, encoding);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get error resource");
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_ERROR || !hr->data) {
        result = wit_string_ctor("", 0, 0, encoding);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get error resource");
        goto end;
    }

    const char *debug_string = wasi_error_to_debug_string(hr);
    if (!debug_string) {
        result = wit_string_ctor("", 0, 0, encoding);
        goto end;
    }

    result = wit_string_ctor((char *)debug_string, strlen(debug_string),
                             strlen(debug_string), encoding);

end:
    if (!result) {
        if (!wasm_runtime_get_exception(module_inst)) {
            wasm_runtime_set_exception(module_inst,
                                       "Could not allocate error debug string");
        }
        goto cleanup;
    }
    if (!store(exec_env->cx, offset_addr, func_type->results->result, result)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store error debug string");
    }

cleanup:
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/* wasi:io/poll */

/**
 * @brief Wrapper for the `ready` method of the `wasi:io/poll.pollable`
 * resource.
 * @param exec_env The execution environment.
 * @param pollable_handle The handle of the pollable resource.
 * @return `true` if the pollable is ready, `false` otherwise.
 */
uint32_t
wasi_io_poll_pollable_ready_wrapper(wasm_exec_env_t exec_env,
                                    uint32_t pollable_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    uint32_t result = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, pollable_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_POLLABLE) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get pollable resource");
        goto end;
    }

    result = wasi_pollable_ready((wasi_pollable_context_t *)hr->data);

end:
    free_wit_value(lifted_handle);
    return result;
}

/**
 * @brief Wrapper for the `block` method of the `wasi:io/poll.pollable`
 * resource.
 * @param exec_env The execution environment.
 * @param pollable_handle The handle of the pollable resource to block on.
 */
void
wasi_io_poll_pollable_block_wrapper(wasm_exec_env_t exec_env,
                                    uint32_t pollable_handle)
{

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wasi_p2_wait_status_t wait_status = WASI_P2_WAIT_FAILED;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, pollable_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_POLLABLE) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get pollable resource");
        goto end;
    }

    wait_status = wasi_pollable_block_interruptible(
        exec_env, (wasi_pollable_context_t *)hr->data);
    if (wait_status == WASI_P2_WAIT_FAILED) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not wait on pollable resource");
    }

end:
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `poll` function of the `wasi:io/poll` interface.
 * @details This function waits for a set of pollables to become ready and
 * returns a list of indices of the ready pollables.
 * @param exec_env The execution environment.
 * @param pollables_ptr A pointer to an array of pollable handles in the guest's
 * memory.
 * @param pollables_len The number of pollables in the array.
 * @param[out] offset_addr Memory's offset where the wit_value_t list will be
 * stored.
 */
void
wasi_io_poll_poll_wrapper(wasm_exec_env_t exec_env, uint32_t pollables,
                          uint32_t pollables_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t result = NULL;
    wit_value_t pollables_list = NULL;
    wit_value_t *elems = NULL;
    uint32_t constructed = 0;
    const wasi_pollable_context_t **my_pollables = NULL;
    wasi_list_u32_t wasi_ret = { 0 };
    wasi_p2_wait_status_t wait_status = WASI_P2_WAIT_FAILED;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    uint32_t idx = 0, handle = 0;

    if (!load_list_from_range(
            exec_env->cx, pollables, pollables_len,
            func_type->params->params[0].type->type_specific.list->element_type,
            &pollables_list)) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    if (!array_allocation_fits(pollables_len,
                               sizeof(wasi_pollable_context_t *))) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Pollable list is too large");
        goto end;
    }
    if (pollables_len > 0) {
        my_pollables = (const wasi_pollable_context_t **)wasm_runtime_malloc(
            sizeof(wasi_pollable_context_t *) * pollables_len);
        if (!my_pollables) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "Could not allocate pollable list");
            goto end;
        }
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = NULL;

    for (idx = 0; idx < pollables_len; idx++) {
        handle = pollables_list->value.list_value.elems[idx]
                     ->value.resource_value.value;
        hr = host_resource_table_get(hr_table, handle);
        if (!hr || hr->type != WASI_P2_POLLABLE) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "Could not get pollable resource");
            result = wit_list_ctor(NULL, 0);
            goto end;
        }
        my_pollables[idx] = (wasi_pollable_context_t *)hr->data;
    }

    wait_status = wasi_poll_interruptible(exec_env, my_pollables, pollables_len,
                                          &wasi_ret);
    if (wait_status == WASI_P2_WAIT_INTERRUPTED) {
        goto end;
    }
    if (wait_status == WASI_P2_WAIT_FAILED) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not wait on pollable list");
        goto end;
    }

    if (!array_allocation_fits(wasi_ret.len, sizeof(wit_value_t))) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Poll result is too large");
        goto end;
    }
    if (wasi_ret.len > 0) {
        elems = (wit_value_t *)wasm_runtime_malloc(sizeof(wit_value_t)
                                                   * wasi_ret.len);
        if (!elems) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "Could not allocate poll result");
            goto end;
        }
    }

    for (; constructed < wasi_ret.len; constructed++) {
        elems[constructed] = wit_u32_ctor(wasi_ret.buf[constructed]);
        if (!elems[constructed]) {
            wasm_runtime_set_exception(exec_env->module_inst,
                                       "Could not allocate poll result");
            goto end;
        }
    }

    result = wit_list_ctor(elems, wasi_ret.len);
    if (!result) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not allocate poll result");
        goto end;
    }
    elems = NULL;
    constructed = 0;

end:
    if (my_pollables) {
        wasm_runtime_free(my_pollables);
    }
    if (wasi_ret.buf) {
        wasm_runtime_free(wasi_ret.buf);
    }
    free_wit_value_array(elems, constructed);
    free_wit_value(pollables_list);
    if (result) {
        store(exec_env->cx, offset_addr, func_type->results->result, result);
    }
    free_wit_value(result);
}

/* wasi:io/streams */

/**
 * @brief Wrapper for the `read` method of the `wasi:io/streams.input-stream`
 * resource.
 * @details This function reads data from an input stream, copies it into the
 * WebAssembly guest's memory, and returns the result.
 * @param exec_env The execution environment.
 * @param input_stream_handle The handle of the input-stream resource.
 * @param len The maximum number of bytes to read.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_input_stream_read_wrapper(wasm_exec_env_t exec_env,
                                          uint32_t input_stream_handle,
                                          int64_t len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_list_u8_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };
    uint32_t read_capacity = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    read_capacity = stream_read_capacity((uint64_t)len);
    if (!prepare_stream_publication(
            exec_env, offset_addr, func_type->results->result,
            ((StreamResourceType *)hr->data)->type, STREAM_PUBLICATION_LIST_U8,
            read_capacity, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_read(hr, read_capacity, &wasi_ret);
    }
    else {
        wasi_p2_stream_resource_read((StreamResourceType *)hr->data,
                                     read_capacity, false, &wasi_ret);
    }
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL,
        wasi_ret.is_err ? NULL : wasi_ret.u.ok.buf,
        wasi_ret.is_err ? 0 : wasi_ret.u.ok.buf_len);
    if (!wasi_ret.is_err && wasi_ret.u.ok.buf) {
        wasm_runtime_free(wasi_ret.u.ok.buf);
        wasi_ret.u.ok.buf = NULL;
    }
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store input stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_input_stream_result(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `blocking-read` method of the
 * `wasi:io/streams.input-stream` resource.
 * @details This function performs a blocking read from an input stream, copies
 * the data into the WebAssembly guest's memory, and returns the result.
 * @param exec_env The execution environment.
 * @param input_stream_handle The handle of the input-stream resource.
 * @param len The maximum number of bytes to read.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_input_stream_blocking_read_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t input_stream_handle,
                                                   int64_t len,
                                                   uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_list_u8_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };
    uint32_t read_capacity = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    read_capacity = stream_read_capacity((uint64_t)len);
    if (!prepare_stream_publication(
            exec_env, offset_addr, func_type->results->result,
            ((StreamResourceType *)hr->data)->type, STREAM_PUBLICATION_LIST_U8,
            read_capacity, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_read(hr, read_capacity, &wasi_ret);
    }
    else {
        wasi_p2_stream_resource_read((StreamResourceType *)hr->data,
                                     read_capacity, true, &wasi_ret);
    }
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL,
        wasi_ret.is_err ? NULL : wasi_ret.u.ok.buf,
        wasi_ret.is_err ? 0 : wasi_ret.u.ok.buf_len);
    if (!wasi_ret.is_err && wasi_ret.u.ok.buf) {
        wasm_runtime_free(wasi_ret.u.ok.buf);
        wasi_ret.u.ok.buf = NULL;
    }
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store input stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_input_stream_result(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `skip` method of the `wasi:io/streams.input-stream`
 * resource.
 * @param exec_env The execution environment.
 * @param input_stream_handle The handle of the input-stream resource.
 * @param len The maximum number of bytes to skip.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_input_stream_skip_wrapper(wasm_exec_env_t exec_env,
                                          uint32_t input_stream_handle,
                                          int64_t len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_u64_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_U64, 0, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_skip(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_p2_stream_resource_skip((StreamResourceType *)hr->data,
                                     (uint64_t)len, false, &wasi_ret);
    }

    (void)finish_stream_publication(&publication,
                                    wasi_ret.is_err ? &wasi_ret.u.err : NULL,
                                    NULL, wasi_ret.is_err ? 0 : wasi_ret.u.ok);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store input stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `blocking-skip` method of the
 * `wasi:io/streams.input-stream` resource.
 * @param exec_env The execution environment.
 * @param input_stream_handle The handle of the input-stream resource.
 * @param len The maximum number of bytes to skip.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_input_stream_blocking_skip_wrapper(wasm_exec_env_t exec_env,
                                                   uint32_t input_stream_handle,
                                                   int64_t len,
                                                   uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_u64_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_U64, 0, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_skip(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_p2_stream_resource_skip((StreamResourceType *)hr->data,
                                     (uint64_t)len, true, &wasi_ret);
    }

    (void)finish_stream_publication(&publication,
                                    wasi_ret.is_err ? &wasi_ret.u.err : NULL,
                                    NULL, wasi_ret.is_err ? 0 : wasi_ret.u.ok);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store input stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:io/streams.input-stream` resource.
 * @param exec_env The execution environment.
 * @param input_stream_handle The handle of the input-stream resource.
 * @return A new pollable resource rep (index).
 */
uint32_t
wasi_io_streams_input_stream_subscribe_wrapper(wasm_exec_env_t exec_env,
                                               uint32_t input_stream_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    uint32_t result = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        goto end;
    }

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable resource");
        goto end;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        SET_ALWAYS_READY_POLLABLE((wasi_pollable_context_t *)hr_poll->data);
    }
    else if (!init_owned_fd_pollable(exec_env, hr_poll,
                                     ((StreamResourceType *)hr->data)->fd,
                                     WASI_POLLABLE_IN)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not duplicate input stream fd");
        goto end;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add pollable resource to HR table");
        goto end;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        goto end;
    }
    result = index_rep;

end:
    free_wit_value(lifted_handle);
    return result;
}

/**
 * @brief Wrapper for the `check-write` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_check_write_wrapper(wasm_exec_env_t exec_env,
                                                  uint32_t output_stream_handle,
                                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_u64_stream_error_t wasi_ret = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_check_write(output_stream_fd, &wasi_ret);

    if (wasi_ret.is_err) {
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    result = stream_u64_result_ctor(module_inst, wasi_ret.u.ok);

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `write` method of the `wasi:io/streams.output-stream`
 * resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param contents_ptr A pointer to the data to write in the guest's memory.
 * @param contents_len The length of the data to write.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_write_wrapper(wasm_exec_env_t exec_env,
                                            uint32_t output_stream_handle,
                                            uint32_t contents_ptr,
                                            uint32_t contents_len,
                                            uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t contents_val = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    wasi_list_u8_t contents;

    if (!load_string_from_range(exec_env->cx, contents_ptr, contents_len,
                                &contents_val)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    contents.buf = (uint8_t *)contents_val->value.string_value.chars;
    contents.buf_len = (uint64)contents_len;

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }

    wasi_p2_stream_resource_write((StreamResourceType *)hr->data, &contents,
                                  false, false, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(contents_val);
}

/**
 * @brief Wrapper for the `blocking-write-and-flush` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param contents_ptr A pointer to the data to write in the guest's memory.
 * @param contents_len The length of the data to write.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_blocking_write_and_flush_wrapper(
    wasm_exec_env_t exec_env, uint32_t output_stream_handle,
    uint32_t contents_ptr, uint32_t contents_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    wit_value_t contents_val = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    wasi_list_u8_t contents;

    if (!load_string_from_range(exec_env->cx, contents_ptr, contents_len,
                                &contents_val)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    contents.buf = (uint8_t *)contents_val->value.string_value.chars;
    contents.buf_len = (uint64)contents_len;

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }

    wasi_p2_stream_resource_write((StreamResourceType *)hr->data, &contents,
                                  true, true, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(contents_val);
}

/**
 * @brief Wrapper for the `flush` method of the `wasi:io/streams.output-stream`
 * resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_flush_wrapper(wasm_exec_env_t exec_env,
                                            uint32_t output_stream_handle,
                                            uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_output_stream_t output_stream_fd =
        ((StreamResourceType *)hr->data)->fd;

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }
    wasi_output_stream_flush(output_stream_fd, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `blocking-flush` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_blocking_flush_wrapper(
    wasm_exec_env_t exec_env, uint32_t output_stream_handle,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }
    wasi_output_stream_blocking_flush(output_stream_fd, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `subscribe` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @return A new pollable resource rep (index).
 */
uint32_t
wasi_io_streams_output_stream_subscribe_wrapper(wasm_exec_env_t exec_env,
                                                uint32_t output_stream_handle)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        free_wit_value(lifted_handle);
        return 0;
    }

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable stream resource");
        free_wit_value(lifted_handle);
        return 0;
    }

    if (!init_owned_fd_pollable(exec_env, hr_poll,
                                ((StreamResourceType *)hr->data)->fd,
                                WASI_POLLABLE_OUT)) {
        destroy_host_resource(hr_poll);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not duplicate output stream fd");
        free_wit_value(lifted_handle);
        return 0;
    }
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add output stream resource to HR table");
        free_wit_value(lifted_handle);
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        free_wit_value(lifted_handle);
        return 0;
    }
    free_wit_value(lifted_handle);
    return index_rep;
}

/**
 * @brief Wrapper for the `write-zeroes` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param len The number of zero bytes to write.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_write_zeroes_wrapper(
    wasm_exec_env_t exec_env, uint32_t output_stream_handle, int64_t len,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }

    wasi_p2_stream_resource_write_zeroes(
        (StreamResourceType *)hr->data, (uint64_t)len, false, false, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `blocking-write-zeroes-and-flush` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the output-stream resource.
 * @param len The number of zero bytes to write.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_blocking_write_zeroes_and_flush_wrapper(
    wasm_exec_env_t exec_env, uint32_t output_stream_handle, int64_t len,
    uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_void_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_OUTPUT_STREAM || !hr->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(exec_env, offset_addr,
                                    func_type->results->result,
                                    ((StreamResourceType *)hr->data)->type,
                                    STREAM_PUBLICATION_UNIT, 0, &publication)) {
        goto cleanup;
    }

    wasi_p2_stream_resource_write_zeroes((StreamResourceType *)hr->data,
                                         (uint64_t)len, true, true, &wasi_ret);
    (void)finish_stream_publication(
        &publication, wasi_ret.is_err ? &wasi_ret.u.err : NULL, NULL, 0);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `splice` method of the `wasi:io/streams.output-stream`
 * resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the destination output-stream.
 * @param src_input_stream_handle The handle of the source input-stream.
 * @param len The maximum number of bytes to splice.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_splice_wrapper(wasm_exec_env_t exec_env,
                                             uint32_t output_stream_handle,
                                             uint32_t src_input_stream_handle,
                                             int64_t len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_output_handle = NULL;
    wit_value_t lifted_input_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_u64_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_output_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, src_input_stream_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_input_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr_stream_out = host_resource_table_get(
        hr_table, lifted_output_handle->value.resource_value.value);

    if (!hr_stream_out || hr_stream_out->type != WASI_P2_IO_OUTPUT_STREAM
        || !hr_stream_out->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResource *hr_input_stream = host_resource_table_get(
        hr_table, lifted_input_handle->value.resource_value.value);

    if (!hr_input_stream || hr_input_stream->type != WASI_P2_IO_INPUT_STREAM
        || !hr_input_stream->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr_input_stream->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(
            exec_env, offset_addr, func_type->results->result,
            ((StreamResourceType *)hr_input_stream->data)->type,
            STREAM_PUBLICATION_U64, 0, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr_input_stream)) {
        StreamResourceType *output_stream =
            (StreamResourceType *)hr_stream_out->data;
        if (output_stream->type == STREAM_TYPE_FILE) {
            wasi_p2_callback_input_stream_to_resource_splice(
                hr_input_stream, output_stream, (uint64_t)len, false,
                &wasi_ret);
        }
        else {
            wasi_p2_callback_input_stream_splice(
                hr_input_stream, (wasi_output_stream_t)output_stream->fd,
                (uint64_t)len, false, &wasi_ret);
        }
    }
    else {
        wasi_p2_stream_resources_splice(
            (StreamResourceType *)hr_stream_out->data,
            (StreamResourceType *)hr_input_stream->data, (uint64_t)len, false,
            &wasi_ret);
    }

    (void)finish_stream_publication(&publication,
                                    wasi_ret.is_err ? &wasi_ret.u.err : NULL,
                                    NULL, wasi_ret.is_err ? 0 : wasi_ret.u.ok);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_input_handle);
    free_wit_value(lifted_output_handle);
}

/**
 * @brief Wrapper for the `blocking-splice` method of the
 * `wasi:io/streams.output-stream` resource.
 * @param exec_env The execution environment.
 * @param output_stream_handle The handle of the destination output-stream.
 * @param src_input_stream_handle The handle of the source input-stream.
 * @param len The number of bytes to splice.
 * @param[out] offset_addr Memory's offset where the result wit_value_t will be
 * stored.
 */
void
wasi_io_streams_output_stream_blocking_splice_wrapper(
    wasm_exec_env_t exec_env, uint32_t output_stream_handle,
    uint32_t src_input_stream_handle, int64_t len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wit_value_t lifted_output_handle = NULL;
    wit_value_t lifted_input_handle = NULL;
    wit_value_t result = NULL;
    uint32_t owned_error_rep = 0;
    wasi_result_u64_stream_error_t wasi_ret = { 0 };
    StreamPublication publication = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_output_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, src_input_stream_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_input_handle)) {
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr_stream_out = host_resource_table_get(
        hr_table, lifted_output_handle->value.resource_value.value);

    if (!hr_stream_out || hr_stream_out->type != WASI_P2_IO_OUTPUT_STREAM
        || !hr_stream_out->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    HostResource *hr_input_stream = host_resource_table_get(
        hr_table, lifted_input_handle->value.resource_value.value);

    if (!hr_input_stream || hr_input_stream->type != WASI_P2_IO_INPUT_STREAM
        || !hr_input_stream->data) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        result =
            invalid_stream_error_result_ctor(module_inst, &owned_error_rep);
        goto end;
    }

    if (!native_stream_length_fits(len)) {
        wasi_ret.is_err = true;
        wasi_ret.u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        wasi_ret.u.err.payload.error = EOVERFLOW;
        result = stream_error_result_ctor(
            module_inst, ((StreamResourceType *)hr_input_stream->data)->type,
            &wasi_ret.u.err, &owned_error_rep);
        goto end;
    }

    if (!prepare_stream_publication(
            exec_env, offset_addr, func_type->results->result,
            ((StreamResourceType *)hr_input_stream->data)->type,
            STREAM_PUBLICATION_U64, 0, &publication)) {
        goto cleanup;
    }

    if (wasi_p2_is_callback_input_stream(hr_input_stream)) {
        StreamResourceType *output_stream =
            (StreamResourceType *)hr_stream_out->data;
        if (output_stream->type == STREAM_TYPE_FILE) {
            wasi_p2_callback_input_stream_to_resource_splice(
                hr_input_stream, output_stream, (uint64_t)len, true, &wasi_ret);
        }
        else {
            wasi_p2_callback_input_stream_splice(
                hr_input_stream, (wasi_output_stream_t)output_stream->fd,
                (uint64_t)len, true, &wasi_ret);
        }
    }
    else {
        wasi_p2_stream_resources_splice(
            (StreamResourceType *)hr_stream_out->data,
            (StreamResourceType *)hr_input_stream->data, (uint64_t)len, true,
            &wasi_ret);
    }

    (void)finish_stream_publication(&publication,
                                    wasi_ret.is_err ? &wasi_ret.u.err : NULL,
                                    NULL, wasi_ret.is_err ? 0 : wasi_ret.u.ok);
    goto cleanup;

end:
    if (result
        && !store_stream_result(exec_env, offset_addr,
                                func_type->results->result, result,
                                owned_error_rep)
        && !wasm_runtime_get_exception(module_inst)) {
        wasm_runtime_set_exception(module_inst,
                                   "Could not store output stream result");
    }
cleanup:
    abort_stream_publication(&publication);
    free_wit_value(result);
    free_wit_value(lifted_input_handle);
    free_wit_value(lifted_output_handle);
}
