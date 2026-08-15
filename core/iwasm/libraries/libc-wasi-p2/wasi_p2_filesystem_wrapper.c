/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_filesystem_wrapper.h"
#include "wasi_p2_common.h"
#include "wasi_p2_types.h"
#include "wasm_runtime_common.h"
#include "wasi_p2_filesystem.h"
#include "wasi_p2_filesystem_quota.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_canonical_abi.h"
#include "component-model/wasm_component_canonical.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include <string.h>

#include "posix.h"
#include "errno.h"

wit_value_t
get_optional_datetime_val(wasi_optional_datetime_t *datetime);

static wit_value_t
make_owned_resource_result(uint32_t rep)
{
    wit_value_t resource = wit_resource_ctor(rep);
    wit_value_t result;

    if (!resource) {
        return NULL;
    }
    result = wit_result_ctor(false, resource);
    if (!result) {
        free_wit_value(resource);
    }
    return result;
}

static bool
runtime_array_allocation_fits(uint64_t count, size_t element_size)
{
    return element_size != 0 && count <= UINT32_MAX / element_size;
}

static void
free_wit_value_array(wit_value_t *values, uint32_t count)
{
    if (!values) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        free_wit_value(values[i]);
    }
    wasm_runtime_free(values);
}

static void
free_record_fields(ComponentWITRecordField *fields, uint32_t count)
{
    if (!fields) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        wasm_runtime_free(fields[i].key);
        free_wit_value(fields[i].value);
    }
    wasm_runtime_free(fields);
}

/* Transfer value ownership only after the field key allocation succeeds. */
static bool
init_record_field_take(ComponentWITRecordField *field, char *key,
                       uint32_t key_size, wit_value_t *value)
{
    if (!field || !value || !*value) {
        return false;
    }

    init_record_field(field, key, key_size, *value);
    if (!field->key) {
        return false;
    }
    *value = NULL;
    return true;
}

static wit_value_t
make_descriptor_flags_result(wasi_descriptor_flags_t flags)
{
    static char *const names[] = { "read",
                                   "write",
                                   "file-integrity-sync",
                                   "data-integrity-sync",
                                   "requested-write-sync",
                                   "mutate-directory" };
    static const uint32_t name_lengths[] = { 4, 5, 19, 19, 20, 16 };
    ComponentWITRecordField *fields = NULL;
    wit_value_t field_value = NULL;
    wit_value_t flags_value = NULL;
    wit_value_t result = NULL;
    uint32_t i;

    fields = wasm_runtime_calloc(6, sizeof(ComponentWITRecordField));
    if (!fields) {
        goto fail;
    }
    for (i = 0; i < 6; i++) {
        field_value = wit_bool_ctor((flags & (1U << i)) != 0);
        if (!field_value
            || !init_record_field_take(&fields[i], names[i], name_lengths[i],
                                       &field_value)) {
            goto fail;
        }
    }

    flags_value = wit_flag_ctor(fields, 6);
    if (!flags_value) {
        goto fail;
    }
    fields = NULL;
    result = wit_result_ctor(false, flags_value);
    if (!result) {
        goto fail;
    }
    return result;

fail:
    free_wit_value(field_value);
    free_wit_value(flags_value);
    free_record_fields(fields, 6);
    return NULL;
}

static wit_value_t
make_read_result(const wasi_list_u8_t *list, bool end_of_stream)
{
    wit_value_t *elems = NULL;
    wit_value_t *tuple_elems = NULL;
    wit_value_t list_value = NULL;
    wit_value_t tuple_value = NULL;
    wit_value_t result = NULL;
    uint32_t count;
    uint32_t constructed = 0;

    if (!list || (list->buf_len > 0 && !list->buf) || list->buf_len > UINT32_MAX
        || !runtime_array_allocation_fits(list->buf_len, sizeof(wit_value_t))) {
        return NULL;
    }
    count = (uint32_t)list->buf_len;

    if (count > 0) {
        elems = wasm_runtime_calloc(count, sizeof(wit_value_t));
        if (!elems) {
            goto fail;
        }
        for (; constructed < count; constructed++) {
            elems[constructed] = wit_u8_ctor(list->buf[constructed]);
            if (!elems[constructed]) {
                goto fail;
            }
        }
    }

    list_value = wit_list_ctor(elems, count);
    if (!list_value) {
        goto fail;
    }
    elems = NULL;

    tuple_elems = wasm_runtime_calloc(2, sizeof(wit_value_t));
    if (!tuple_elems) {
        goto fail;
    }
    tuple_elems[0] = list_value;
    list_value = NULL;
    tuple_elems[1] = wit_bool_ctor(end_of_stream);
    if (!tuple_elems[1]) {
        goto fail;
    }

    tuple_value = wit_tuple_ctor(tuple_elems, 2);
    if (!tuple_value) {
        goto fail;
    }
    tuple_elems = NULL;
    result = wit_result_ctor(false, tuple_value);
    if (!result) {
        goto fail;
    }
    return result;

fail:
    free_wit_value_array(elems, constructed);
    free_wit_value_array(tuple_elems, 2);
    free_wit_value(list_value);
    free_wit_value(tuple_value);
    return NULL;
}

static wit_value_t
make_descriptor_stat_result(const wasi_descriptor_stat_t *stat)
{
    ComponentWITRecordField *fields = NULL;
    wit_value_t field_value = NULL;
    wit_value_t record_value = NULL;
    wit_value_t result = NULL;

    if (!stat) {
        return NULL;
    }
    fields = wasm_runtime_calloc(6, sizeof(ComponentWITRecordField));
    if (!fields) {
        goto fail;
    }

    field_value = wit_enum_ctor(stat->type);
    if (!init_record_field_take(&fields[0], "type", 4, &field_value))
        goto fail;
    field_value = wit_u64_ctor(stat->link_count);
    if (!init_record_field_take(&fields[1], "link-count", 10, &field_value))
        goto fail;
    field_value = wit_u64_ctor(stat->size);
    if (!init_record_field_take(&fields[2], "size", 4, &field_value))
        goto fail;
    field_value = get_optional_datetime_val(
        (wasi_optional_datetime_t *)&stat->data_access_timestamp);
    if (!init_record_field_take(&fields[3], "data-access-timestamp", 21,
                                &field_value))
        goto fail;
    field_value = get_optional_datetime_val(
        (wasi_optional_datetime_t *)&stat->data_modification_timestamp);
    if (!init_record_field_take(&fields[4], "data-modification-timestamp", 27,
                                &field_value))
        goto fail;
    field_value = get_optional_datetime_val(
        (wasi_optional_datetime_t *)&stat->status_change_timestamp);
    if (!init_record_field_take(&fields[5], "status-change-timestamp", 23,
                                &field_value))
        goto fail;

    record_value = wit_record_ctor(fields, 6);
    if (!record_value) {
        goto fail;
    }
    fields = NULL;
    result = wit_result_ctor(false, record_value);
    if (!result) {
        goto fail;
    }
    return result;

fail:
    free_wit_value(field_value);
    free_wit_value(record_value);
    free_record_fields(fields, 6);
    return NULL;
}

static wit_value_t
make_metadata_hash_result(const wasi_metadata_hash_value_t *hash)
{
    ComponentWITRecordField *fields = NULL;
    wit_value_t field_value = NULL;
    wit_value_t record_value = NULL;
    wit_value_t result = NULL;

    if (!hash) {
        return NULL;
    }
    fields = wasm_runtime_calloc(2, sizeof(ComponentWITRecordField));
    if (!fields) {
        goto fail;
    }
    field_value = wit_u64_ctor(hash->lower);
    if (!init_record_field_take(&fields[0], "lower", 5, &field_value))
        goto fail;
    field_value = wit_u64_ctor(hash->upper);
    if (!init_record_field_take(&fields[1], "upper", 5, &field_value))
        goto fail;

    record_value = wit_record_ctor(fields, 2);
    if (!record_value) {
        goto fail;
    }
    fields = NULL;
    result = wit_result_ctor(false, record_value);
    if (!result) {
        goto fail;
    }
    return result;

fail:
    free_wit_value(field_value);
    free_wit_value(record_value);
    free_record_fields(fields, 2);
    return NULL;
}

static wit_value_t
make_empty_directory_entry_result(void)
{
    wit_value_t option = wit_option_ctor(NULL);
    wit_value_t result;

    if (!option) {
        return NULL;
    }
    result = wit_result_ctor(false, option);
    if (!result) {
        free_wit_value(option);
    }
    return result;
}

static wit_value_t
make_directory_entry_result(wasm_exec_env_t exec_env,
                            const wasi_directory_entry_t *entry)
{
    ComponentWITRecordField *fields = NULL;
    wit_value_t field_value = NULL;
    wit_value_t record_value = NULL;
    wit_value_t option_value = NULL;
    wit_value_t result = NULL;
    uint8_t *encoded_str = NULL;
    uint32_t encoded_str_len = 0;
    uint32_t encoded_code_units = 0;
    StringEncoding encoding;

    if (!entry || !entry->name || strlen(entry->name) > UINT32_MAX) {
        return NULL;
    }
    fields = wasm_runtime_calloc(2, sizeof(ComponentWITRecordField));
    if (!fields) {
        goto fail;
    }
    field_value = wit_enum_ctor(entry->type);
    if (!init_record_field_take(&fields[0], "type", 4, &field_value))
        goto fail;

    encoding = wasm_get_string_encoding(exec_env);
    if (!encode_string(exec_env->cx, entry->name, (uint32_t)strlen(entry->name),
                       encoding, &encoded_str, &encoded_str_len,
                       &encoded_code_units)) {
        goto fail;
    }
    field_value = wit_string_ctor((char *)encoded_str, encoded_str_len,
                                  encoded_code_units, encoding);
    if (encoded_str != (const uint8_t *)entry->name) {
        wasm_runtime_free(encoded_str);
    }
    encoded_str = NULL;
    if (!init_record_field_take(&fields[1], "name", 4, &field_value))
        goto fail;

    record_value = wit_record_ctor(fields, 2);
    if (!record_value) {
        goto fail;
    }
    fields = NULL;
    option_value = wit_option_ctor(record_value);
    if (!option_value) {
        goto fail;
    }
    record_value = NULL;
    result = wit_result_ctor(false, option_value);
    if (!result) {
        goto fail;
    }
    return result;

fail:
    if (encoded_str && encoded_str != (const uint8_t *)entry->name) {
        wasm_runtime_free(encoded_str);
    }
    free_wit_value(field_value);
    free_wit_value(record_value);
    free_wit_value(option_value);
    free_record_fields(fields, 2);
    return NULL;
}

static void
store_filesystem_result(wasm_exec_env_t exec_env, uint32_t offset_addr,
                        WASMComponentTypeInstance *result_type,
                        wit_value_t result)
{
    if (!result) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not allocate filesystem result");
        return;
    }
    (void)store(exec_env->cx, offset_addr, result_type, result);
}

/* A mutating filesystem call must not make a native change and then discover
   that its guest-visible Ok result cannot be allocated or lowered. Keep the
   complete canonical lower pending until the native operation has finished,
   and reserve the Err payload up front so a syscall failure can replace the
   staged Ok without allocating. */
static bool
stage_unit_filesystem_success(wasm_exec_env_t exec_env, uint32_t offset_addr,
                              WASMComponentTypeInstance *result_type,
                              wit_value_t *result, wit_value_t *error_payload,
                              CanonicalResourceTransferScope *lower_scope)
{
    if (!exec_env || !exec_env->cx || !result_type || !result || !error_payload
        || !lower_scope) {
        return false;
    }

    *error_payload = wit_enum_ctor(WASI_FILESYSTEM_CODE_INVALID);
    if (!*error_payload) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not allocate filesystem mutation error result");
        return false;
    }
    *result = wit_result_ctor(false, NULL);
    if (!*result) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not allocate filesystem mutation success result");
        return false;
    }
    if (!canonical_resource_transfer_scope_enter(
            lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not start filesystem mutation result transfer");
        return false;
    }
    if (!store(exec_env->cx, offset_addr, result_type, *result)
        || !canonical_resource_transfer_scope_can_commit(lower_scope)) {
        (void)canonical_resource_transfer_scope_leave(lower_scope, false);
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not stage filesystem mutation result");
        return false;
    }
    return true;
}

static bool
finish_unit_filesystem_result(wasm_exec_env_t exec_env, uint32_t offset_addr,
                              WASMComponentTypeInstance *result_type,
                              wit_value_t result, wit_value_t *error_payload,
                              CanonicalResourceTransferScope *lower_scope,
                              int native_error, const char *failure_message)
{
    if (native_error != WASI_ERROR_CODE_SUCCESS) {
        (*error_payload)->value.enum_value.value =
            errno_to_wasi_filesystem(native_error);
        result->value.result_value.is_err = true;
        result->value.result_value.result.err = *error_payload;
        *error_payload = NULL;

        /* This overwrites the staged Ok discriminant before the transaction
           can be committed, so a native failure can never leave an old Ok in
           guest memory. The replacement is inline scalar data only. */
        if (!store(exec_env->cx, offset_addr, result_type, result)
            || !canonical_resource_transfer_scope_can_commit(lower_scope)) {
            (void)canonical_resource_transfer_scope_leave(lower_scope, false);
            wasm_runtime_set_exception(exec_env->module_inst, failure_message);
            return false;
        }
    }

    if (!canonical_resource_transfer_scope_leave(lower_scope, true)) {
        wasm_runtime_set_exception(exec_env->module_inst, failure_message);
        return false;
    }
    return true;
}

/* wasi:filesystem/preopens */

/**
 * @brief Constructs an optional datetime wit_value_t.
 * @details Creates a WIT Option value from a wasi_optional_datetime_t
 * structure, representing it as a Record containing seconds and nanoseconds if
 * present.
 * @param datetime Pointer to the wasi_optional_datetime_t to convert.
 * @return A wit_value_t representing the Option<Datetime>.
 */

wit_value_t
get_optional_datetime_val(wasi_optional_datetime_t *datetime)
{
    if (datetime->has_value) {
        wit_value_t seconds_val = wit_u64_ctor(datetime->datetime.seconds);
        wit_value_t nanoseconds_val =
            wit_u32_ctor(datetime->datetime.nanoseconds);
        ComponentWITRecordField *datetime_fields = NULL;
        wit_value_t datetime_val = NULL;
        wit_value_t option = NULL;

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
        init_record_field(&datetime_fields[1], "nanoseconds", 11,
                          nanoseconds_val);
        if (!datetime_fields[1].key)
            goto fail;
        nanoseconds_val = NULL;

        datetime_val = wit_record_ctor(datetime_fields, 2);
        if (!datetime_val)
            goto fail;
        datetime_fields = NULL;
        option = wit_option_ctor(datetime_val);
        if (!option)
            goto fail;
        return option;

    fail:
        free_wit_value(datetime_val);
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
    return wit_option_ctor(NULL);
}

/**
 * @brief Wrapper for the `get-directories` function of the
 * `wasi:filesystem/preopens` interface.
 * @details This function retrieves the list of pre-opened directories and
 * copies it into the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset where list struct populated with
 *                        the offset and length of the resulting list of tuples
 * will be stored
 */
void
wasi_filesystem_get_directories_wrapper(wasm_exec_env_t exec_env,
                                        uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    struct fd_prestats *prestats = NULL;
    uint32_t count = 0;

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    uint32_t opened_dirs = 0;
    wit_value_t *elems = NULL;
    uint32_t *owned_reps = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    bh_assert(wasi_ctx);
    prestats = wasi_ctx->prestats;
    bh_assert(prestats);

    if (prestats->size) {
        if (prestats->size > UINT32_MAX / sizeof(wit_value_t)
            || prestats->size > UINT32_MAX / sizeof(uint32_t)) {
            goto construction_failed;
        }
        elems = (wit_value_t *)wasm_runtime_malloc(sizeof(wit_value_t)
                                                   * prestats->size);
        owned_reps =
            (uint32_t *)wasm_runtime_malloc(sizeof(uint32_t) * prestats->size);
        if (!elems || !owned_reps) {
            goto construction_failed;
        }
        memset(elems, 0, sizeof(wit_value_t) * prestats->size);
        memset(owned_reps, 0, sizeof(uint32_t) * prestats->size);

        struct fd_table *curfds = wasi_ctx->curfds;
        for (uint32_t i = 3; i < prestats->size; i++) {
            wit_value_t *tuple_elems = NULL;
            wit_value_t tuple = NULL;
            uint8_t *encoded_str = NULL;
            uint32_t encoded_str_len = 0;
            uint32_t encoded_code_units = 0;
            uint32_t fs_rep;

            if (!prestats->prestats[i].dir)
                continue;

            os_file_handle host_fd;
            if (!fd_table_get_host_handle(curfds, i, &host_fd))
                continue;

            const char *dir = prestats->prestats[i].dir;
            uint32_t dir_len = strlen(dir);
            HostResourceTable *hr_table = get_global_host_resource_table();
            HostResource *hr = host_resource_create(
                WASI_P2_FILESYSTEM_DESCRIPTOR, sizeof(uint32_t));
            if (!hr) {
                goto construction_failed;
            }

            // FD opened from initialization, no resource destructor needed
            *((wasi_descriptor_t *)hr->data) = (wasi_descriptor_t)host_fd;

            fs_rep = host_resource_table_add(hr_table, hr);
            if (fs_rep == 0) {
                destroy_host_resource(hr);
                goto construction_failed;
            }

            tuple_elems =
                (wit_value_t *)wasm_runtime_malloc(2 * sizeof(wit_value_t));
            if (!tuple_elems) {
                (void)host_resource_table_delete(hr_table, fs_rep);
                goto construction_failed;
            }
            memset(tuple_elems, 0, 2 * sizeof(wit_value_t));
            tuple_elems[0] = wit_resource_ctor(fs_rep);
            if (!tuple_elems[0]) {
                wasm_runtime_free(tuple_elems);
                (void)host_resource_table_delete(hr_table, fs_rep);
                goto construction_failed;
            }

            StringEncoding encoding = wasm_get_string_encoding(exec_env);
            if (!encode_string(exec_env->cx, dir, dir_len, encoding,
                               &encoded_str, &encoded_str_len,
                               &encoded_code_units)) {
                free_wit_value(tuple_elems[0]);
                wasm_runtime_free(tuple_elems);
                (void)host_resource_table_delete(hr_table, fs_rep);
                goto construction_failed;
            }
            tuple_elems[1] =
                wit_string_ctor((char *)encoded_str, encoded_str_len,
                                encoded_code_units, encoding);
            if (encoded_str != (const uint8_t *)dir) {
                wasm_runtime_free(encoded_str);
            }
            if (!tuple_elems[1]) {
                free_wit_value(tuple_elems[0]);
                wasm_runtime_free(tuple_elems);
                (void)host_resource_table_delete(hr_table, fs_rep);
                goto construction_failed;
            }
            tuple = wit_tuple_ctor(tuple_elems, 2);
            if (!tuple) {
                free_wit_value(tuple_elems[0]);
                free_wit_value(tuple_elems[1]);
                wasm_runtime_free(tuple_elems);
                (void)host_resource_table_delete(hr_table, fs_rep);
                goto construction_failed;
            }

            elems[opened_dirs] = tuple;
            owned_reps[opened_dirs] = fs_rep;
            opened_dirs++;
        }
    }

    result = wit_list_ctor(elems, opened_dirs);
    if (!result) {
        goto construction_failed;
    }
    elems = NULL;
    goto end;

construction_failed:
    if (elems) {
        for (uint32_t i = 0; i < opened_dirs; i++) {
            free_wit_value(elems[i]);
        }
        wasm_runtime_free(elems);
        elems = NULL;
    }
    wasi_p2_cleanup_failed_owned_host_resources(exec_env, owned_reps,
                                                opened_dirs);
    opened_dirs = 0;
    result = wit_list_ctor(NULL, 0);

end:
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, owned_reps,
        opened_dirs);
    free_wit_value(result);
    wasm_runtime_free(owned_reps);
}

/* wasi:filesystem/types */

/* descriptor */

/**
 * @brief Wrapper for the `read-via-stream` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param offset The offset to start reading from.
 * @param[out] offset_addr Memory's offset where new input stream handle or an
 * error code will be stored
 */
void
wasi_filesystem_read_via_stream_wrapper(wasm_exec_env_t exec_env,
                                        wasi_descriptor_t fd,
                                        wasi_filesize_t offset,
                                        uint32_t offset_addr)
{
    wasi_input_stream_t stream;
    int err = 0;
    HostResourceTable *hr_table = get_global_host_resource_table();

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_QUOTA);
        goto end;
    }
    wasi_filesystem_read_via_stream(descriptor_fd, offset, &stream, &err);
    if (err == 0) {
        HostResource *hr_stream = host_resource_create(
            WASI_P2_IO_INPUT_STREAM, sizeof(StreamResourceType));

        if (!hr_stream) {
            close(stream);
            wasi_p2_native_fd_quota_release(&fd_lease);
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not create stream input resource");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }

        ((StreamResourceType *)hr_stream->data)->fd = stream;
        ((StreamResourceType *)hr_stream->data)->type = STREAM_TYPE_FILE;
        ((StreamResourceType *)hr_stream->data)->position = offset;
        ((StreamResourceType *)hr_stream->data)->position_valid = true;
        ((StreamResourceType *)hr_stream->data)->append = false;
        host_resource_set_dtor(hr_stream, file_stream_dtor);
        wasi_p2_native_fd_quota_transfer_to_host_resource(hr_stream, &fd_lease);

        uint32_t index_rep = host_resource_table_add(hr_table, hr_stream);
        if (index_rep < 1) {
            destroy_host_resource(
                hr_stream); // Clean up the HostResource on failure
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not add stream input resource to HR table");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }
        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        }
        else {
            owned_rep = index_rep;
        }
    }
    else {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_filesystem(err));
    }

end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `write-via-stream` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param offset The offset to start writing to.
 * @param[out] offset_addr Memory's offset where new output stream handle or an
 * error code will be stored
 */
void
wasi_filesystem_write_via_stream_wrapper(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd,
                                         wasi_filesize_t offset,
                                         uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_input_stream_t stream;
    int err = 0;
    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_QUOTA);
        goto end;
    }
    wasi_filesystem_write_via_stream(descriptor_fd, offset, &stream, &err);
    if (err == 0) {
        HostResource *hr_stream = host_resource_create(
            WASI_P2_IO_OUTPUT_STREAM, sizeof(StreamResourceType));

        if (!hr_stream) {
            close(stream);
            wasi_p2_native_fd_quota_release(&fd_lease);
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not create stream input resource");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }

        ((StreamResourceType *)hr_stream->data)->fd = stream;
        ((StreamResourceType *)hr_stream->data)->type = STREAM_TYPE_FILE;
        ((StreamResourceType *)hr_stream->data)->position = offset;
        ((StreamResourceType *)hr_stream->data)->position_valid = true;
        ((StreamResourceType *)hr_stream->data)->append = false;
        host_resource_set_dtor(hr_stream, file_stream_dtor);
        wasi_p2_native_fd_quota_transfer_to_host_resource(hr_stream, &fd_lease);

        uint32_t index_rep = host_resource_table_add(hr_table, hr_stream);
        if (index_rep < 1) {
            destroy_host_resource(
                hr_stream); // Clean up the HostResource on failure
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not add stream input resource to HR table");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }
        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        }
        else {
            owned_rep = index_rep;
        }
    }
    else {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_filesystem(err));
    }

end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `append-via-stream` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where new output stream handle or an
 * error code will be stored
 */
void
wasi_filesystem_append_via_stream_wrapper(wasm_exec_env_t exec_env,
                                          wasi_descriptor_t fd,
                                          uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    wasi_output_stream_t stream = 0;
    int err = 0;
    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_QUOTA);
        goto end;
    }
    wasi_filesystem_append_via_stream(descriptor_fd, &stream, &err);
    if (err == 0) {
        HostResource *hr_stream = host_resource_create(
            WASI_P2_IO_OUTPUT_STREAM, sizeof(StreamResourceType));

        if (!hr_stream) {
            close(stream);
            wasi_p2_native_fd_quota_release(&fd_lease);
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not create stream input resource");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }

        ((StreamResourceType *)hr_stream->data)->fd = stream;
        ((StreamResourceType *)hr_stream->data)->type = STREAM_TYPE_FILE;
        ((StreamResourceType *)hr_stream->data)->position = 0;
        ((StreamResourceType *)hr_stream->data)->position_valid = true;
        ((StreamResourceType *)hr_stream->data)->append = true;
        host_resource_set_dtor(hr_stream, file_stream_dtor);
        wasi_p2_native_fd_quota_transfer_to_host_resource(hr_stream, &fd_lease);

        uint32_t index_rep = host_resource_table_add(hr_table, hr_stream);
        if (index_rep < 1) {
            destroy_host_resource(
                hr_stream); // Clean up the HostResource on failure
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not add stream input resource to HR table");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }
        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        }
        else {
            owned_rep = index_rep;
        }
    }
    else {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_filesystem(err));
    }

end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `advise` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param offset The offset to start the advisory region from.
 * @param length The length of the advisory region.
 * @param advice The type of advisory to give.
 * @param[out] offset_addr Memory's offset where error code on failure will be
 * stored
 */
void
wasi_filesystem_advise_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                               wasi_filesize_t offset, wasi_filesize_t length,
                               uint32_t advice, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope))
        goto cleanup;

    int err = wasi_filesystem_advise(descriptor_fd, offset, length,
                                     (wasi_advice_t)advice);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem advise result");
    goto cleanup;
end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
cleanup:
    if (lower_scope.active)
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `sync-data` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where error code on failure will be
 * stored
 */
void
wasi_filesystem_sync_data_wrapper(wasm_exec_env_t exec_env,
                                  wasi_descriptor_t fd, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope))
        goto cleanup;

    int err = wasi_filesystem_sync_data(descriptor_fd);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem sync-data result");
    goto cleanup;
end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
cleanup:
    if (lower_scope.active)
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `get-flags` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where the descriptor flags or an
 * error code will be stored
 */
void
wasi_filesystem_get_flags_wrapper(wasm_exec_env_t exec_env,
                                  wasi_descriptor_t fd, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_descriptor_flags_t flags;
    int err = 0;
    wasi_filesystem_get_flags(descriptor_fd, &flags, &err);
    if (err == 0) {
        result = make_descriptor_flags_result(flags);
        if (!result) {
            result =
                get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
        }
    }
    else {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }
end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `get-type` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where the descriptor type or an error
 * code will be stored
 */
void
wasi_filesystem_get_type_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                 uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t type_value = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_descriptor_type_t type;
    int err = 0;
    wasi_filesystem_get_type(descriptor_fd, &type, &err);
    if (err == 0) {
        type_value = wit_enum_ctor(type);
        if (type_value) {
            result = wit_result_ctor(false, type_value);
        }
        if (result) {
            type_value = NULL;
        }
    }
    else {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }
end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(type_value);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-size` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param size The new size of the file.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with an error code on failure will be stored
 */
void
wasi_filesystem_set_size_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                 wasi_filesize_t size, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    int err = wasi_filesystem_set_size(descriptor_fd, size);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem set-size result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `set-times` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param data_access_timestamp_tag The tag for the new data access timestamp.
 * @param data_access_timestamp_sec The seconds part of the new data access
 * timestamp.
 * @param data_access_timestamp_nsec The nanoseconds part of the new data access
 * timestamp.
 * @param data_modification_timestamp_tag The tag for the new data modification
 * timestamp.
 * @param data_modification_timestamp_sec The seconds part of the new data
 * modification timestamp.
 * @param data_modification_timestamp_nsec The nanoseconds part of the new data
 * modification timestamp.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with an error code on failure will be stored
 */
void
wasi_filesystem_set_times_wrapper(wasm_exec_env_t exec_env,
                                  wasi_descriptor_t fd,
                                  uint32_t data_access_timestamp_tag,
                                  int64_t data_access_timestamp_sec,
                                  uint32_t data_access_timestamp_nsec,
                                  uint32_t data_modification_timestamp_tag,
                                  int64_t data_modification_timestamp_sec,
                                  uint32_t data_modification_timestamp_nsec,
                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    wasi_new_timestamp_t data_access_timestamp;
    wasi_new_timestamp_t data_modification_timestamp;

    data_access_timestamp.tag =
        (wasi_new_timestamp_tag_t)data_access_timestamp_tag;
    data_access_timestamp.timestamp.seconds = data_access_timestamp_sec;
    data_access_timestamp.timestamp.nanoseconds = data_access_timestamp_nsec;

    data_modification_timestamp.tag =
        (wasi_new_timestamp_tag_t)data_modification_timestamp_tag;
    data_modification_timestamp.timestamp.seconds =
        data_modification_timestamp_sec;
    data_modification_timestamp.timestamp.nanoseconds =
        data_modification_timestamp_nsec;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    int err = wasi_filesystem_set_times(descriptor_fd, data_access_timestamp,
                                        data_modification_timestamp);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem set-times result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `read` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param length The maximum number of bytes to read.
 * @param offset The offset to start reading from.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with the list of bytes read and an end-of-stream flag, or an error
 * code will be stored
 */
void
wasi_filesystem_read_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                             wasi_filesize_t length, wasi_filesize_t offset,
                             uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wasi_list_u8_t list = { NULL, 0 };
    bool end_of_stream = false;
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    /* The canonical list uses uint32 lengths and one host pointer per byte. */
    if (length > UINT32_MAX
        || !runtime_array_allocation_fits(length, sizeof(wit_value_t))
        || offset > INT64_MAX) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_OVERFLOW);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_filesystem_read(descriptor_fd, length, offset, &list, &end_of_stream,
                         &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    result = make_read_result(&list, end_of_stream);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:

    if (list.buf) {
        wasm_runtime_free(list.buf);
    }
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `write` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param buffer_ptr A pointer to the data to write in the guest's memory.
 * @param buffer_len The length of the data to write.
 * @param offset The offset to start writing to.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with the number of bytes written or an error code will be stored
 */
void
wasi_filesystem_write_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                              uint32_t *buffer_ptr, uint32_t buffer_len,
                              wasi_filesize_t offset, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t bytes_written_value = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    wasi_filesize_t bytes_written;
    int err = 0;
    const uint8_t *buffer = (uint8_t *)buffer_ptr;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    /* Pre-lower both result alternatives before pwrite can change the file.
       The post-write lower only replaces an inline u64 or enum. */
    error_payload = wit_enum_ctor(WASI_FILESYSTEM_CODE_INVALID);
    bytes_written_value = wit_u64_ctor(0);
    if (bytes_written_value) {
        result = wit_result_ctor(false, bytes_written_value);
        if (result) {
            bytes_written_value = NULL;
        }
    }
    if (!error_payload || !result) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not allocate filesystem write result");
        store_result_at_end = false;
        goto end;
    }
    if (!canonical_resource_transfer_scope_enter(
            &lower_scope, exec_env->cx, WASM_COMPONENT_TABLE_TRANSACTION_LOWER)
        || !store(exec_env->cx, offset_addr, func_type->results->result, result)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope)) {
        if (lower_scope.active) {
            (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        }
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not stage filesystem write result");
        store_result_at_end = false;
        goto end;
    }
    store_result_at_end = false;

    wasi_filesystem_write(descriptor_fd, buffer, buffer_len, offset,
                          &bytes_written, &err);

    if (err != 0) {
        free_wit_value(result->value.result_value.result.ok);
        result->value.result_value.is_err = true;
        result->value.result_value.result.err = error_payload;
        error_payload->value.enum_value.value = errno_to_wasi_filesystem(err);
        error_payload = NULL;
    }
    else {
        result->value.result_value.result.ok->value.u64_value = bytes_written;
    }

    if (!store(exec_env->cx, offset_addr, func_type->results->result, result)
        || !canonical_resource_transfer_scope_can_commit(&lower_scope)
        || !canonical_resource_transfer_scope_leave(&lower_scope, true)) {
        if (lower_scope.active) {
            (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        }
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not publish filesystem write result");
    }
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(bytes_written_value);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `read-directory` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with the new directory entry stream handle or an error code will be
 * stored
 */
void
wasi_filesystem_read_directory_wrapper(wasm_exec_env_t exec_env,
                                       wasi_descriptor_t fd,
                                       uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    wasi_directory_entry_stream_t stream;
    int err = 0;
    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_QUOTA);
        goto end;
    }
    wasi_filesystem_read_directory(descriptor_fd, &stream, &err);
    if (err == 0) {
        HostResource *hr_stream = host_resource_create(
            WASI_P2_DIRECTORY_ENTRY_STREAM, sizeof(stream));

        if (!hr_stream) {
            closedir(stream);
            wasi_p2_native_fd_quota_release(&fd_lease);
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not create dir entry stream resource");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
            goto end;
        }

        *((wasi_directory_entry_stream_t *)hr_stream->data) = stream;
        host_resource_set_dtor(hr_stream, directory_entry_stream_dtor);
        wasi_p2_native_fd_quota_transfer_to_host_resource(hr_stream, &fd_lease);

        uint32_t index_rep = host_resource_table_add(hr_table, hr_stream);
        if (index_rep < 1) {
            destroy_host_resource(
                hr_stream); // Clean up the HostResource on failure
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not add dir entry stream resource to HR table");
            result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
            goto end;
        }

        result = make_owned_resource_result(index_rep);
        if (!result) {
            (void)host_resource_table_delete(hr_table, index_rep);
            result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        }
        else {
            owned_rep = index_rep;
        }
    }
    else {
        wasi_p2_native_fd_quota_release(&fd_lease);
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }
end:
    wasi_p2_native_fd_quota_release(&fd_lease);
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `sync` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with an error code on failure will be stored
 */
void
wasi_filesystem_sync_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                             uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope))
        goto cleanup;

    int err = wasi_filesystem_sync(descriptor_fd);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem sync result");
    goto cleanup;
end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
cleanup:
    if (lower_scope.active)
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `create-directory-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param path_ptr A pointer to the path in the guest's memory.
 * @param path_len The length of the path.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with an error code on failure will be stored
 */
void
wasi_filesystem_create_directory_at_wrapper(wasm_exec_env_t exec_env,
                                            wasi_descriptor_t fd,
                                            uint32_t path_ptr,
                                            uint32_t path_len,
                                            uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_create_directory_at_with_fd_quota(
        exec_env, descriptor_fd, path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem create-directory-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
}

/**
 * @brief Wrapper for the `stat` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with the descriptor stat information or an error code will be
 * stored
 */
void
wasi_filesystem_stat_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                             uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wasi_descriptor_stat_t stat = { 0 };
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_filesystem_stat(descriptor_fd, &stat, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    result = make_descriptor_stat_result(&stat);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `stat-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param path_flags The path flags.
 * @param path_ptr A pointer to the path in the guest's memory.
 * @param path_len The length of the path.
 * @param[out] offset_addr Memory's offset where result struct that will be
 * populated with the descriptor stat information or an error code will be
 * stored
 */
void
wasi_filesystem_stat_at_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                uint32_t path_flags, uint32_t path_ptr,
                                uint32_t path_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t name_val = NULL;
    wasi_descriptor_stat_t stat = { 0 };
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &name_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    char *path = name_val->value.string_value.chars;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_filesystem_stat_at_with_fd_quota(exec_env, descriptor_fd,
                                          (wasi_path_flags_t)path_flags, path,
                                          &stat, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    result = make_descriptor_stat_result(&stat);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(name_val);
}

/**
 * @brief Wrapper for the `set-times-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Adjusts the access and modification timestamps of a file or
 * directory resolved relative to the given directory descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_flags Flags determining how the path is resolved (e.g., following
 * symlinks).
 * @param path_ptr Memory offset to the path string in the guest's memory.
 * @param path_len Length of the path string.
 * @param data_access_timestamp_tag Tag indicating how to interpret the access
 * timestamp.
 * @param data_access_timestamp_sec Seconds part of the access timestamp.
 * @param data_access_timestamp_nsec Nanoseconds part of the access timestamp.
 * @param data_modification_timestamp_tag Tag indicating how to interpret the
 * modification timestamp.
 * @param data_modification_timestamp_sec Seconds part of the modification
 * timestamp.
 * @param data_modification_timestamp_nsec Nanoseconds part of the modification
 * timestamp.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */

void
wasi_filesystem_set_times_at_wrapper(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd, uint32_t path_flags,
    uint32_t path_ptr, uint32_t path_len, uint32_t data_access_timestamp_tag,
    int64_t data_access_timestamp_sec, uint32_t data_access_timestamp_nsec,
    uint32_t data_modification_timestamp_tag,
    int64_t data_modification_timestamp_sec,
    uint32_t data_modification_timestamp_nsec, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    wasi_new_timestamp_t data_access_timestamp;
    wasi_new_timestamp_t data_modification_timestamp;

    data_access_timestamp.tag =
        (wasi_new_timestamp_tag_t)data_access_timestamp_tag;
    data_access_timestamp.timestamp.seconds = data_access_timestamp_sec;
    data_access_timestamp.timestamp.nanoseconds = data_access_timestamp_nsec;

    data_modification_timestamp.tag =
        (wasi_new_timestamp_tag_t)data_modification_timestamp_tag;
    data_modification_timestamp.timestamp.seconds =
        data_modification_timestamp_sec;
    data_modification_timestamp.timestamp.nanoseconds =
        data_modification_timestamp_nsec;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_set_times_at_with_fd_quota(
        exec_env, descriptor_fd, (wasi_path_flags_t)path_flags, path,
        data_access_timestamp, data_modification_timestamp);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem set-times-at result");

end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
}

/**
 * @brief Wrapper for the `link-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Creates a hard link.
 * @param exec_env The execution environment.
 * @param old_fd The directory descriptor from which the old path is resolved.
 * @param old_path_flags Flags determining how the old path is resolved.
 * @param old_path_ptr Memory offset to the old path string.
 * @param old_path_len Length of the old path string.
 * @param new_fd The directory descriptor from which the new path is resolved.
 * @param new_path_ptr Memory offset to the new path string.
 * @param new_path_len Length of the new path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */
void
wasi_filesystem_link_at_wrapper(wasm_exec_env_t exec_env,
                                wasi_descriptor_t old_fd,
                                uint32_t old_path_flags, uint32_t old_path_ptr,
                                uint32_t old_path_len, wasi_descriptor_t new_fd,
                                uint32_t new_path_ptr, uint32_t new_path_len,
                                uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle_old = NULL;
    wit_value_t lifted_handle_new = NULL;
    wit_value_t old_path_val = NULL;
    wit_value_t new_path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, old_fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle_old)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, new_fd,
            func_type->params->params[3].type->type_specific.resource_handle,
            &lifted_handle_new)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, old_path_ptr, old_path_len,
                                &old_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *old_path = old_path_val->value.string_value.chars;

    if (!load_string_from_range(exec_env->cx, new_path_ptr, new_path_len,
                                &new_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *new_path = new_path_val->value.string_value.chars;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr1 = host_resource_table_get(
        hr_table, lifted_handle_old->value.resource_value.value);
    HostResource *hr2 = host_resource_table_get(
        hr_table, lifted_handle_new->value.resource_value.value);

    if (!(hr1 && hr2)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fds resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fds from the host resource
    wasi_descriptor_t descriptor_fd1 = *((wasi_descriptor_t *)hr1->data);
    wasi_descriptor_t descriptor_fd2 = *((wasi_descriptor_t *)hr2->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_link_at_with_fd_quota(
        exec_env, descriptor_fd1, (wasi_path_flags_t)old_path_flags, old_path,
        descriptor_fd2, new_path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem link-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle_old);
    free_wit_value(lifted_handle_new);
    free_wit_value(old_path_val);
    free_wit_value(new_path_val);
}

/**
 * @brief Wrapper for the `open-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Opens a file or directory resolved relative to the given directory
 * descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_flags Flags determining how the path is resolved (e.g., following
 * symlinks).
 * @param path_ptr Memory offset to the path string.
 * @param path_len Length of the path string.
 * @param open_flags Flags specifying how the file should be opened (e.g.,
 * create, truncate).
 * @param desc_flags Flags specifying the rights/permissions for the new
 * descriptor.
 * @param[out] offset_addr Memory offset where the Result struct (containing the
 * new descriptor handle or an error code) will be stored.
 */
void
wasi_filesystem_open_at_wrapper(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                uint32_t path_flags, uint32_t path_ptr,
                                uint32_t path_len, uint32_t open_flags,
                                uint32_t desc_flags, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t error_payload = NULL;
    uint32_t owned_rep = 0;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool lower_scope_active = false;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    wasi_descriptor_t new_fd = (wasi_descriptor_t)-1;
    int err = 0;
    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    /* Reserve the native-failure payload before O_CREAT/O_TRUNC can mutate
       namespace or file contents. It is later installed into the already
       validated result object without allocating. */
    error_payload = wit_enum_ctor(WASI_FILESYSTEM_CODE_INVALID);
    if (!error_payload) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not allocate open-at error result");
        goto cleanup;
    }

    /* Allocate and lower every guest-facing object before O_CREAT/O_TRUNC can
       mutate the filesystem.  UINT32_MAX is outside the encoded HostResource
       ID range, so it is a private placeholder in the unpublished lower
       transaction; it is rebound after the host resource exists. */
    result = make_owned_resource_result(UINT32_MAX);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }
    if (!canonical_resource_transfer_scope_enter(
            &lower_scope, exec_env->cx,
            WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not start descriptor transfer");
        goto cleanup;
    }
    lower_scope_active = true;
    if (!wasi_p2_store_owned_host_resource_result(exec_env, offset_addr,
                                                  func_type->results->result,
                                                  result, NULL, 0)) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        goto cleanup;
    }
    if (!canonical_resource_transfer_scope_can_commit(&lower_scope)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not prepare descriptor transfer");
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        goto cleanup;
    }

    HostResource *hr_new =
        host_resource_create(WASI_P2_FILESYSTEM_DESCRIPTOR, sizeof(new_fd));
    if (!hr_new) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        free_wit_value(result);
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }
    *((wasi_descriptor_t *)hr_new->data) = (wasi_descriptor_t)-1;
    host_resource_set_dtor(hr_new, filesystem_descriptor_dtor);

    owned_rep = host_resource_table_add(hr_table, hr_new);
    if (owned_rep < 1) {
        destroy_host_resource(hr_new);
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        free_wit_value(result);
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }
    if (!canonical_resource_transfer_scope_rebind_single_owned_rep(
            &lower_scope, UINT32_MAX, owned_rep)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not bind descriptor transfer");
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        (void)host_resource_table_delete(hr_table, owned_rep);
        owned_rep = 0;
        goto cleanup;
    }

    wasi_filesystem_open_at_with_fd_quota(
        exec_env, descriptor_fd, (wasi_path_flags_t)path_flags, path,
        (wasi_open_flags_t)open_flags, (wasi_descriptor_flags_t)desc_flags,
        0666, &new_fd, &fd_lease, &err);
    if (err != 0) {
        /* Replace the staged Ok before rolling its tentative owned handle
           back. Canonical lowering of this Err is inline and was prevalidated
           by the successful Ok lower, so quota denial cannot expose old Ok. */
        free_wit_value(result->value.result_value.result.ok);
        result->value.result_value.is_err = true;
        result->value.result_value.result.err = error_payload;
        error_payload->value.enum_value.value = errno_to_wasi_filesystem(err);
        error_payload = NULL;
        if (!store(exec_env->cx, offset_addr, func_type->results->result,
                   result)
            || !canonical_resource_transfer_scope_can_commit(&lower_scope)) {
            wasm_runtime_set_exception(
                exec_env->module_inst,
                "Could not publish open-at error result");
        }
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
        lower_scope_active = false;
        (void)host_resource_table_delete(hr_table, owned_rep);
        owned_rep = 0;
        goto cleanup;
    }

    *((wasi_descriptor_t *)hr_new->data) = new_fd;
    wasi_p2_native_fd_quota_transfer_to_host_resource(hr_new, &fd_lease);
    if (!canonical_resource_transfer_scope_leave(&lower_scope, true)) {
        lower_scope_active = false;
        (void)host_resource_table_delete(hr_table, owned_rep);
        owned_rep = 0;
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not publish descriptor transfer");
        goto cleanup;
    }
    lower_scope_active = false;
    goto cleanup;

end:
    (void)wasi_p2_store_owned_host_resource_result(
        exec_env, offset_addr, func_type->results->result, result, &owned_rep,
        owned_rep != 0 ? 1 : 0);
cleanup:
    wasi_p2_native_fd_quota_release(&fd_lease);
    if (lower_scope_active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
}

/**
 * @brief Wrapper for the `readlink-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Reads the contents of a symbolic link resolved relative to the given
 * directory descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_ptr Memory offset to the symbolic link path string.
 * @param path_len Length of the symbolic link path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing the
 * resolved link string or an error code) will be stored.
 */
void
wasi_filesystem_readlink_at_wrapper(wasm_exec_env_t exec_env,
                                    wasi_descriptor_t fd, uint32_t path_ptr,
                                    uint32_t path_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t str_val = NULL;
    char *link_content = NULL;
    uint8_t *encoded_str = NULL;
    uint32_t encoded_str_len = 0;
    uint32_t encoded_code_units = 0;
    size_t link_content_len;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    int err = 0;
    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_filesystem_readlink_at_with_fd_quota(exec_env, descriptor_fd, path,
                                              &link_content, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    if (!link_content) {
        goto end;
    }
    link_content_len = strlen(link_content);
    if (link_content_len > UINT32_MAX) {
        goto end;
    }

    StringEncoding encoding = wasm_get_string_encoding(exec_env);
    if (!encode_string(exec_env->cx, link_content, (uint32_t)link_content_len,
                       encoding, &encoded_str, &encoded_str_len,
                       &encoded_code_units)) {
        goto end;
    }
    str_val = wit_string_ctor((char *)encoded_str, encoded_str_len,
                              encoded_code_units, encoding);
    if (!str_val) {
        goto end;
    }
    result = wit_result_ctor(false, str_val);
    if (result) {
        str_val = NULL;
    }

end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
    free_wit_value(str_val);
    if (encoded_str && encoded_str != (uint8_t *)link_content) {
        wasm_runtime_free(encoded_str);
    }
    wasm_runtime_free(link_content);
}

/**
 * @brief Wrapper for the `remove-directory-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Removes a directory resolved relative to the given directory
 * descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_ptr Memory offset to the directory path string.
 * @param path_len Length of the directory path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */
void
wasi_filesystem_remove_directory_at_wrapper(wasm_exec_env_t exec_env,
                                            wasi_descriptor_t fd,
                                            uint32_t path_ptr,
                                            uint32_t path_len,
                                            uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_remove_directory_at_with_fd_quota(
        exec_env, descriptor_fd, path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem remove-directory-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
}

/**
 * @brief Wrapper for the `rename-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Renames a file or directory.
 * @param exec_env The execution environment.
 * @param old_fd The directory descriptor from which the old path is resolved.
 * @param old_path_ptr Memory offset to the old path string.
 * @param old_path_len Length of the old path string.
 * @param new_fd The directory descriptor from which the new path is resolved.
 * @param new_path_ptr Memory offset to the new path string.
 * @param new_path_len Length of the new path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */
void
wasi_filesystem_rename_at_wrapper(wasm_exec_env_t exec_env,
                                  wasi_descriptor_t old_fd,
                                  uint32_t old_path_ptr, uint32_t old_path_len,
                                  wasi_descriptor_t new_fd,
                                  uint32_t new_path_ptr, uint32_t new_path_len,
                                  uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle_old = NULL;
    wit_value_t lifted_handle_new = NULL;
    wit_value_t old_path_val = NULL;
    wit_value_t new_path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, old_fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle_old)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, new_fd,
            func_type->params->params[2].type->type_specific.resource_handle,
            &lifted_handle_new)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, old_path_ptr, old_path_len,
                                &old_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *old_path = old_path_val->value.string_value.chars;

    if (!load_string_from_range(exec_env->cx, new_path_ptr, new_path_len,
                                &new_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *new_path = new_path_val->value.string_value.chars;

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr1 = host_resource_table_get(
        hr_table, lifted_handle_old->value.resource_value.value);
    HostResource *hr2 = host_resource_table_get(
        hr_table, lifted_handle_new->value.resource_value.value);

    if (!(hr1 && hr2)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd1 = *((wasi_descriptor_t *)hr1->data);
    wasi_descriptor_t descriptor_fd2 = *((wasi_descriptor_t *)hr2->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_rename_at_with_fd_quota(
        exec_env, descriptor_fd1, old_path, descriptor_fd2, new_path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem rename-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle_old);
    free_wit_value(lifted_handle_new);
    free_wit_value(old_path_val);
    free_wit_value(new_path_val);
}

/**
 * @brief Wrapper for the `symlink-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Creates a symbolic link.
 * @param exec_env The execution environment.
 * @param fd The directory descriptor from which the new symbolic link is
 * resolved.
 * @param old_path_ptr Memory offset to the target path string of the symbolic
 * link.
 * @param old_path_len Length of the target path string.
 * @param new_path_ptr Memory offset to the name of the new symbolic link.
 * @param new_path_len Length of the new symbolic link name.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */
void
wasi_filesystem_symlink_at_wrapper(wasm_exec_env_t exec_env,
                                   wasi_descriptor_t fd, uint32_t old_path_ptr,
                                   uint32_t old_path_len, uint32_t new_path_ptr,
                                   uint32_t new_path_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t old_path_val = NULL;
    wit_value_t new_path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, old_path_ptr, old_path_len,
                                &old_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *old_path = old_path_val->value.string_value.chars;

    if (!load_string_from_range(exec_env->cx, new_path_ptr, new_path_len,
                                &new_path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *new_path = new_path_val->value.string_value.chars;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_symlink_at_with_fd_quota(exec_env, descriptor_fd,
                                                   old_path, new_path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem symlink-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(old_path_val);
    free_wit_value(new_path_val);
}

/**
 * @brief Wrapper for the `unlink-file-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Unlinks (deletes) a file resolved relative to the given directory
 * descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_ptr Memory offset to the file path string.
 * @param path_len Length of the file path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing an
 * error code on failure) will be stored.
 */
void
wasi_filesystem_unlink_file_at_wrapper(wasm_exec_env_t exec_env,
                                       wasi_descriptor_t fd, uint32_t path_ptr,
                                       uint32_t path_len, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t path_val = NULL;
    wit_value_t error_payload = NULL;
    CanonicalResourceTransferScope lower_scope = { 0 };
    bool store_result_at_end = true;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    int err = 0;

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &path_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    const char *path = path_val->value.string_value.chars;

    if (!path) {
        err = WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY;
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    store_result_at_end = false;
    if (!stage_unit_filesystem_success(exec_env, offset_addr,
                                       func_type->results->result, &result,
                                       &error_payload, &lower_scope)) {
        goto end;
    }
    err = wasi_filesystem_unlink_file_at_with_fd_quota(exec_env, descriptor_fd,
                                                       path);
    (void)finish_unit_filesystem_result(
        exec_env, offset_addr, func_type->results->result, result,
        &error_payload, &lower_scope, err,
        "Could not publish filesystem unlink-file-at result");
end:
    if (lower_scope.active) {
        (void)canonical_resource_transfer_scope_leave(&lower_scope, false);
    }
    if (store_result_at_end) {
        store_filesystem_result(exec_env, offset_addr,
                                func_type->results->result, result);
    }
    free_wit_value(error_payload);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(path_val);
}

/**
 * @brief Wrapper for the `is-same-object` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Checks if two file descriptors refer to the exact same underlying
 * file system object.
 * @param exec_env The execution environment.
 * @param fd1 The first file descriptor.
 * @param fd2 The second file descriptor.
 * @return uint32_t Returns 1 if they point to the same object, 0 otherwise.
 */
uint32_t
wasi_filesystem_is_same_object_wrapper(wasm_exec_env_t exec_env,
                                       wasi_descriptor_t fd1,
                                       wasi_descriptor_t fd2)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle_1 = NULL;
    wit_value_t lifted_handle_2 = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, fd1,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle_1)) {
        return 0;
    }

    if (!lift_borrow(
            exec_env->cx, fd2,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_handle_2)) {
        return 0;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr1 = host_resource_table_get(
        hr_table, lifted_handle_1->value.resource_value.value);
    HostResource *hr2 = host_resource_table_get(
        hr_table, lifted_handle_2->value.resource_value.value);

    if (!(hr1 && hr2)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        return 0;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd1 = *((wasi_descriptor_t *)hr1->data);
    wasi_descriptor_t descriptor_fd2 = *((wasi_descriptor_t *)hr2->data);

    return wasi_filesystem_is_same_object(descriptor_fd1, descriptor_fd2);
}

/**
 * @brief Wrapper for the `metadata-hash` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Calculates a hash of the file system object's metadata.
 * @param exec_env The execution environment.
 * @param fd The file descriptor.
 * @param[out] offset_addr Memory offset where the Result struct (containing the
 * upper and lower hash values, or an error code) will be stored.
 */
void
wasi_filesystem_metadata_hash_wrapper(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t fd,
                                      uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wasi_metadata_hash_value_t hash = { 0 };
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);

    wasi_filesystem_metadata_hash(descriptor_fd, &hash, &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    result = make_metadata_hash_result(&hash);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
}

/**
 * @brief Wrapper for the `metadata-hash-at` method of the
 * `wasi:filesystem/types.descriptor` resource.
 * @details Calculates a hash of the metadata for a file or directory resolved
 * relative to a directory descriptor.
 * @param exec_env The execution environment.
 * @param fd The directory file descriptor.
 * @param path_flags Flags determining how the path is resolved.
 * @param path_ptr Memory offset to the path string.
 * @param path_len Length of the path string.
 * @param[out] offset_addr Memory offset where the Result struct (containing the
 * upper and lower hash values, or an error code) will be stored.
 */
void
wasi_filesystem_metadata_hash_at_wrapper(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd,
                                         uint32_t path_flags, uint32_t path_ptr,
                                         uint32_t path_len,
                                         uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wit_value_t name_val = NULL;
    wasi_metadata_hash_value_t hash = { 0 };
    int err = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!load_string_from_range(exec_env->cx, path_ptr, path_len, &name_val)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    char *path = name_val->value.string_value.chars;

    if (!lift_borrow(
            exec_env->cx, fd,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get descriptor fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual descriptor fd from the host resource
    wasi_descriptor_t descriptor_fd = *((wasi_descriptor_t *)hr->data);
    wasi_filesystem_metadata_hash_at_with_fd_quota(
        exec_env, descriptor_fd, (wasi_path_flags_t)path_flags, path, &hash,
        &err);

    if (err != 0) {
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    result = make_metadata_hash_result(&hash);
    if (!result) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:
    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    free_wit_value(name_val);
}

/**
 * @brief Wrapper for the `read-directory-entry` method of the
 * `wasi:filesystem/types.directory-entry-stream` resource.
 * @details Reads a single directory entry from an open directory entry stream.
 * @param exec_env The execution environment.
 * @param stream The directory entry stream handle.
 * @param[out] offset_addr Memory offset where the Result struct will be stored.
 * On success, contains an Option wrapping the directory entry (type and name).
 * If the stream is exhausted, the Option will be None.
 * On failure, contains an error code.
 */
void
wasi_filesystem_read_directory_entry_wrapper(wasm_exec_env_t exec_env,
                                             uint32_t stream,
                                             uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    wit_value_t result = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t lifted_handle = NULL;
    wasi_directory_entry_t entry = { 0 };
    bool is_some = false;
    bool restore_position = false;
    int err = 0;
    long directory_position = -1;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_UNSUPPORTED);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, stream,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        result = get_result_error_val(WASI_NETWORK_ERROR_CODE_INVALID_ARGUMENT);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not get dir entry stream fd fd resource");
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INVALID);
        goto end;
    }

    // Get the actual dir entry stream fd from the host resource
    wasi_directory_entry_stream_t dir_entry_stream_fd =
        *((wasi_directory_entry_stream_t *)hr->data);

    errno = 0;
    directory_position = telldir(dir_entry_stream_fd);
    if (directory_position < 0) {
        result = get_result_error_val(
            errno_to_wasi_filesystem(errno != 0 ? errno : EIO));
        goto end;
    }

    wasi_filesystem_read_directory_entry(dir_entry_stream_fd, &entry, &is_some,
                                         &err);

    if (err != 0) {
        restore_position = true;
        result = get_result_error_val(errno_to_wasi_filesystem(err));
        goto end;
    }

    if (!err && !is_some) {
        // No more files in directory
        result = make_empty_directory_entry_result();
        if (!result) {
            result =
                get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
        }
        goto end;
    }

    if (!is_some) {
        result = get_result_error_val(WASI_FILESYSTEM_CODE_NO_ENTRY);
        goto end;
    }

    result = make_directory_entry_result(exec_env, &entry);
    if (!result) {
        restore_position = true;
        result = get_result_error_val(WASI_FILESYSTEM_CODE_INSUFFICIENT_MEMORY);
    }

end:
    if (!result
        || !store(exec_env->cx, offset_addr, func_type->results->result,
                  result)) {
        restore_position = directory_position >= 0;
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not publish directory entry result");
    }
    if (restore_position && directory_position >= 0)
        seekdir(dir_entry_stream_fd, directory_position);
    free_wit_value(result);
    free_wit_value(lifted_handle);
    if (entry.name)
        wasm_runtime_free(entry.name);
    return;
}

/**
 * @brief Wrapper for the `filesystem-error-code` method in
 * `wasi:filesystem/types`.
 * @details Retrieves the specific filesystem error code from a generic error
 * context, if available.
 * @param exec_env The execution environment.
 * @param err The opaque error context handle.
 * @param[out] offset_addr Memory offset where the Option struct (containing the
 * specific `error-code` Enum, or None) will be stored.
 */
void
wasi_filesystem_filesystem_error_code_wrapper(wasm_exec_env_t exec_env,
                                              uint32_t err,
                                              uint32_t offset_addr)
{

    wit_value_t result = NULL;
    wit_value_t error_value = NULL;
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    bool is_some;
    int error_code = wasi_filesystem_error_code(err, &is_some);
    if (is_some) {
        error_value = wit_enum_ctor((uint32_t)error_code);
        if (error_value) {
            result = wit_option_ctor(error_value);
        }
        if (result) {
            error_value = NULL;
        }
    }
    else {
        result = wit_option_ctor(NULL);
    }

    store_filesystem_result(exec_env, offset_addr, func_type->results->result,
                            result);
    free_wit_value(error_value);
    free_wit_value(result);
}
