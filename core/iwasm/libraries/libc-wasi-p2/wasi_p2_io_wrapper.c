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
#include <string.h>

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
    return element_size != 0 && count <= SIZE_MAX / element_size;
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

/* Consume bytes->buf and construct result<list<u8>, stream-error>. */
static wit_value_t
u8_list_result_ctor(wasm_module_inst_t module_inst, wasi_list_u8_t *bytes)
{
    wit_value_t *elems = NULL;
    wit_value_t list = NULL;
    wit_value_t result = NULL;
    uint32_t constructed = 0;

    if ((bytes->buf_len > 0 && !bytes->buf) || bytes->buf_len > UINT32_MAX
        || !array_allocation_fits(bytes->buf_len, sizeof(wit_value_t))) {
        goto oom;
    }

    uint32_t count = (uint32_t)bytes->buf_len;
    if (count > 0) {
        elems = wasm_runtime_malloc(sizeof(wit_value_t) * count);
        if (!elems) {
            goto oom;
        }

        for (; constructed < count; constructed++) {
            elems[constructed] = wit_u8_ctor(bytes->buf[constructed]);
            if (!elems[constructed]) {
                goto oom;
            }
        }
    }

    list = wit_list_ctor(elems, count);
    if (!list) {
        goto oom;
    }
    elems = NULL;

    result = wit_result_ctor(false, list);
    if (!result) {
        goto oom;
    }
    list = NULL;

    if (bytes->buf) {
        wasm_runtime_free(bytes->buf);
    }
    bytes->buf = NULL;
    bytes->buf_len = 0;
    return result;

oom:
    free_wit_value(list);
    free_wit_value_array(elems, constructed);
    if (bytes->buf) {
        wasm_runtime_free(bytes->buf);
    }
    bytes->buf = NULL;
    bytes->buf_len = 0;
    wasm_runtime_set_exception(module_inst,
                               "Could not allocate input stream result");
    return NULL;
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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

    if (!hr) {
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
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_list_u8_stream_error_t wasi_ret;

    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_read(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_input_stream_t input_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;
        wasi_input_stream_read(input_stream_fd, len, &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = u8_list_result_ctor(module_inst, &wasi_ret.u.ok);

end:
    if (result) {
        store(exec_env->cx, offset_addr, func_type->results->result, result);
    }
    free_wit_value(result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_list_u8_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_read(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_input_stream_t input_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;
        wasi_input_stream_blocking_read(input_stream_fd, len, &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = u8_list_result_ctor(module_inst, &wasi_ret.u.ok);

end:
    if (result) {
        store(exec_env->cx, offset_addr, func_type->results->result, result);
    }
    free_wit_value(result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_u64_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_skip(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_input_stream_t input_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;
        wasi_input_stream_skip(input_stream_fd, len, &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, wit_u64_ctor(wasi_ret.u.ok));

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_u64_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, input_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr || hr->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (wasi_p2_is_callback_input_stream(hr)) {
        wasi_p2_callback_input_stream_skip(hr, (uint64_t)len, &wasi_ret);
    }
    else {
        wasi_input_stream_t input_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;
        wasi_input_stream_blocking_skip(input_stream_fd, len, &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, wit_u64_ctor(wasi_ret.u.ok));

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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
    else {
        SET_INPUT_POLLABLE((wasi_pollable_context_t *)hr_poll->data,
                           ((StreamResourceType *)hr->data)->fd, false);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_u64_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_check_write(output_stream_fd, &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, wit_u64_ctor(wasi_ret.u.ok));

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;
    wasi_list_u8_t contents;

    if (!load_string_from_range(exec_env->cx, contents_ptr, contents_len,
                                &contents_val)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    contents.buf = (uint8_t *)contents_val->value.string_value.chars;
    contents.buf_len = (uint64)contents_len;

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_write(output_stream_fd, &contents, &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;
    wasi_list_u8_t contents;

    if (!load_string_from_range(exec_env->cx, contents_ptr, contents_len,
                                &contents_val)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    contents.buf = (uint8_t *)contents_val->value.string_value.chars;
    contents.buf_len = (uint64)contents_len;

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_blocking_write_and_flush(output_stream_fd, &contents,
                                                &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_output_stream_t output_stream_fd =
        ((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_flush(output_stream_fd, &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_blocking_flush(output_stream_fd, &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
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
        return 0;
    }

    HostResource *hr_poll =
        host_resource_create(WASI_P2_POLLABLE, sizeof(wasi_pollable_context_t));

    if (!hr_poll) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create pollable stream resource");
        return 0;
    }

    SET_OUTPUT_POLLABLE((wasi_pollable_context_t *)hr_poll->data,
                        ((StreamResourceType *)hr->data)->fd, false);
    host_resource_set_dtor(hr_poll, pollable_dtor);

    uint32_t index_rep = host_resource_table_add(hr_table, hr_poll);
    if (index_rep < 1) {
        destroy_host_resource(hr_poll); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add output stream resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_output_stream_t output_stream_fd =
        (wasi_output_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_write_zeroes(output_stream_fd, len, &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_void_stream_error_t wasi_ret;

    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(
        hr_table, lifted_handle->value.resource_value.value);

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_output_stream_t output_stream_fd =
        (wasi_output_stream_t)((StreamResourceType *)hr->data)->fd;

    wasi_output_stream_blocking_write_zeroes_and_flush(output_stream_fd, len,
                                                       &wasi_ret);

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, NULL);

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_u64_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_output_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, src_input_stream_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_input_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr_stream_out = host_resource_table_get(
        hr_table, lifted_output_handle->value.resource_value.value);

    if (!hr_stream_out || hr_stream_out->type != WASI_P2_IO_OUTPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResource *hr_input_stream = host_resource_table_get(
        hr_table, lifted_input_handle->value.resource_value.value);

    if (!hr_input_stream || hr_input_stream->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr_stream_out->data)->fd;
    if (wasi_p2_is_callback_input_stream(hr_input_stream)) {
        wasi_p2_callback_input_stream_splice(hr_input_stream, output_stream_fd,
                                             (uint64_t)len, false, &wasi_ret);
    }
    else {
        wasi_input_stream_t in_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr_input_stream->data)
                ->fd;
        wasi_output_stream_splice(output_stream_fd, in_stream_fd, (uint64_t)len,
                                  &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr_input_stream, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, wit_u64_ctor(wasi_ret.u.ok));

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
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

    if (!wasi_ctx->wasi_options->cli || !wasi_ctx->wasi_options->common) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    wasi_result_u64_stream_error_t wasi_ret;
    if (!lift_borrow(
            exec_env->cx, output_stream_handle,
            func_type->params->params[0].type->type_specific.resource_handle,
            &lifted_output_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    if (!lift_borrow(
            exec_env->cx, src_input_stream_handle,
            func_type->params->params[1].type->type_specific.resource_handle,
            &lifted_input_handle)) {
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr_stream_out = host_resource_table_get(
        hr_table, lifted_output_handle->value.resource_value.value);

    if (!hr_stream_out || hr_stream_out->type != WASI_P2_IO_OUTPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get output stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    HostResource *hr_input_stream = host_resource_table_get(
        hr_table, lifted_input_handle->value.resource_value.value);

    if (!hr_input_stream || hr_input_stream->type != WASI_P2_IO_INPUT_STREAM) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not get input stream resource");
        uint32_t new_err =
            wasi_error_new(STREAM_TYPE_FILE, WASI_FILESYSTEM_CODE_INVALID);
        result = get_stream_error_val(false, new_err);
        goto end;
    }

    // Get the actual output stream fd from the host resource
    wasi_input_stream_t output_stream_fd =
        (wasi_input_stream_t)((StreamResourceType *)hr_stream_out->data)->fd;
    if (wasi_p2_is_callback_input_stream(hr_input_stream)) {
        wasi_p2_callback_input_stream_splice(hr_input_stream, output_stream_fd,
                                             (uint64_t)len, true, &wasi_ret);
    }
    else {
        wasi_input_stream_t in_stream_fd =
            (wasi_input_stream_t)((StreamResourceType *)hr_input_stream->data)
                ->fd;
        wasi_output_stream_blocking_splice(output_stream_fd, in_stream_fd,
                                           (uint64_t)len, &wasi_ret);
    }

    if (wasi_ret.is_err) {
        result = get_hr_stream_error_val(hr_input_stream, &wasi_ret.u.err);
        goto end;
    }

    result = wit_result_ctor(false, wit_u64_ctor(wasi_ret.u.ok));

end:
    store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
    free_wit_value(lifted_input_handle);
    free_wit_value(lifted_output_handle);
}
