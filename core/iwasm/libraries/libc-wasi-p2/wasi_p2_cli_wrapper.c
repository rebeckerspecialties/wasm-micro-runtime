/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_cli_wrapper.h"
#include "wasi_p2_common.h"
#include "wasm_runtime_common.h"
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_canonical.h"
#include <string.h>
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include "component-model/wasm_component_canonical.h"

static bool
wit_value_array_allocation_fits(uint64_t count)
{
    return count <= UINT32_MAX && count <= UINT32_MAX / sizeof(wit_value_t);
}

static void
free_wit_value_array(wit_value_t *elems, uint32_t constructed)
{
    if (!elems)
        return;

    while (constructed > 0)
        free_wit_value(elems[--constructed]);
    wasm_runtime_free(elems);
}

static void
free_environment(wasi_tuple_string_string_t *environment, uint32_t count)
{
    if (!environment)
        return;

    for (uint32_t i = 0; i < count; i++) {
        wasm_runtime_free(environment[i].key);
        wasm_runtime_free(environment[i].value);
    }
    wasm_runtime_free(environment);
}

static wit_value_t
make_string_value(wasm_exec_env_t exec_env, const char *utf8)
{
    StringEncoding encoding;
    uint8_t *encoded = NULL;
    uint32_t encoded_len = 0;
    uint32_t encoded_code_units = 0;
    size_t utf8_len;
    wit_value_t result;

    if (!exec_env || !exec_env->cx || !utf8)
        return NULL;
    utf8_len = strlen(utf8);
    if (utf8_len > UINT32_MAX)
        return NULL;

    encoding = wasm_get_string_encoding(exec_env);
    if (!encode_string(exec_env->cx, utf8, (uint32_t)utf8_len, encoding,
                       &encoded, &encoded_len, &encoded_code_units)) {
        return NULL;
    }
    result = wit_string_ctor((char *)encoded, encoded_len, encoded_code_units,
                             encoding);
    if (encoded != (const uint8_t *)utf8)
        wasm_runtime_free(encoded);
    return result;
}

static wit_value_t
make_optional_owned_resource(uint32_t rep)
{
    wit_value_t resource = wit_resource_ctor(rep);
    wit_value_t option;

    if (!resource)
        return NULL;
    option = wit_option_ctor(resource);
    if (!option)
        free_wit_value(resource);
    return option;
}

// Helper function to covert environment variable list tuple to wit_value_t
static wit_value_t
get_environ_val(const wasi_tuple_string_string_t *environ,
                wasm_exec_env_t exec_env)
{
    wit_value_t *elems;
    wit_value_t result;

    if (!environ || !environ->key || !environ->value)
        return NULL;
    elems = wasm_runtime_malloc(2 * sizeof(wit_value_t));
    if (!elems)
        return NULL;

    elems[0] = make_string_value(exec_env, environ->key);
    if (!elems[0]) {
        wasm_runtime_free(elems);
        return NULL;
    }
    elems[1] = make_string_value(exec_env, environ->value);
    if (!elems[1]) {
        free_wit_value(elems[0]);
        wasm_runtime_free(elems);
        return NULL;
    }

    result = wit_tuple_ctor(elems, 2);
    if (!result)
        free_wit_value_array(elems, 2);
    return result;
}

static bool
is_wasi_env_variable(const char *host_var,
                     const wasi_tuple_string_string_t *wasi_vars,
                     uint32_t wasi_var_count)
{
    uint32_t idx = 0;

    for (idx = 0; idx < wasi_var_count; idx++) {
        if (!strcmp(host_var, wasi_vars[idx].key)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Wrapper for the `get-environment` function of the
 * `wasi:cli/environment` interface.
 * @details This function retrieves the environment variables and copies them
 *          into the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset where output is stored
 */
void
wasi_cli_get_environment_wrapper(wasm_exec_env_t exec_env, uint32_t offset_addr)
{
    WASMModuleInstanceCommon *module_inst =
        wasm_runtime_get_module_inst(exec_env);
    uint32_t host_environ_count = 0;
    wasi_tuple_string_string_t *host_environ = NULL;
    uint32_t wasi_environ_count = 0;
    wasi_tuple_string_string_t *wasi_environ = NULL;
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t result = NULL;
    wit_value_t *aux_list = NULL;
    uint32_t total_env_count = 0;
    uint32_t max_env_count = 0;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    if (wasi_ctx->argv_environ) {
        wasi_environ_count = wasi_ctx->argv_environ->environ_count;
        wasi_environ = wasi_cli_environment_split_str(
            wasi_ctx->argv_environ->environ_list, wasi_environ_count);
        if (wasi_environ_count > 0 && !wasi_environ)
            goto construction_failed;
    }

    if (wasi_ctx->wasi_options->inherit_env) {
        host_environ = wasi_cli_get_environment(&host_environ_count);
        if (host_environ_count > 0 && !host_environ)
            goto construction_failed;
    }

    if (!host_environ && !wasi_environ) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    if (host_environ_count > UINT32_MAX - wasi_environ_count)
        goto construction_failed;
    max_env_count = host_environ_count + wasi_environ_count;
    if (!wit_value_array_allocation_fits(max_env_count))
        goto construction_failed;

    if (max_env_count > 0) {
        aux_list = (wit_value_t *)wasm_runtime_malloc(max_env_count
                                                      * sizeof(wit_value_t));
        if (!aux_list) {
            goto construction_failed;
        }

        for (uint32_t index = 0; index < host_environ_count; index++) {
            if (is_wasi_env_variable(host_environ[index].key, wasi_environ,
                                     wasi_environ_count)) {
                continue;
            }
            aux_list[total_env_count] =
                get_environ_val(&host_environ[index], exec_env);
            if (!aux_list[total_env_count])
                goto construction_failed;
            total_env_count++;
        }

        for (uint32_t index = 0; index < wasi_environ_count; index++) {
            aux_list[total_env_count] =
                get_environ_val(&wasi_environ[index], exec_env);
            if (!aux_list[total_env_count])
                goto construction_failed;
            total_env_count++;
        }
        result = wit_list_ctor(aux_list, total_env_count);
        if (!result)
            goto construction_failed;
        aux_list = NULL;
        total_env_count = 0;
    }
    else {
        result = wit_list_ctor(NULL, 0);
    }
    goto end;

construction_failed:
    if (wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
        == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct CLI environment");
    }

end:
    free_wit_value_array(aux_list, total_env_count);
    free_environment(host_environ, host_environ_count);
    free_environment(wasi_environ, wasi_environ_count);
    if (!result
        && wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
               == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct CLI environment");
    }
    if (result)
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    free_wit_value(result);
}

/**
 * @brief Wrapper for the `get-arguments` function of the `wasi:cli/environment`
 * interface.
 * @details This function retrieves the command-line arguments and copies them
 *          into the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset where output is stored
 */
void
wasi_cli_get_arguments_wrapper(wasm_exec_env_t exec_env, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t argc = 0;
    char **argv = NULL;
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    wit_value_t result = NULL;
    wit_value_t *aux_list = NULL;
    uint32_t constructed = 0;

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    if (wasi_ctx->argv_environ) {
        argc = wasi_ctx->argv_environ->argc;
        argv = wasi_ctx->argv_environ->argv_list;
    }

    if (!argv) {
        result = wit_list_ctor(NULL, 0);
        goto end;
    }

    if (argc > 0) {
        if (!wit_value_array_allocation_fits(argc))
            goto construction_failed;
        aux_list =
            (wit_value_t *)wasm_runtime_malloc(argc * sizeof(wit_value_t));
        if (!aux_list)
            goto construction_failed;

        for (; constructed < argc; constructed++) {
            aux_list[constructed] =
                make_string_value(exec_env, argv[constructed]);
            if (!aux_list[constructed])
                goto construction_failed;
        }
        result = wit_list_ctor(aux_list, argc);
        if (!result)
            goto construction_failed;
        aux_list = NULL;
        constructed = 0;
    }
    else {
        result = wit_list_ctor(NULL, 0);
    }
    goto end;

construction_failed:
    if (wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
        == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct CLI arguments");
    }

end:
    free_wit_value_array(aux_list, constructed);
    if (!result
        && wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
               == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct CLI arguments");
    }
    if (result)
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    free_wit_value(result);
}

/**
 * @brief Wrapper for the `initial-cwd` function of the `wasi:cli/environment`
 * interface.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset where output is stored
 */
void
wasi_cli_initial_cwd_wrapper(wasm_exec_env_t exec_env, uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    wit_value_t optional_result = NULL;
    wit_value_t string_result = NULL;
    char *cwd = NULL;

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    bool is_some;
    cwd = wasi_cli_initial_cwd(&is_some);

    if (!cwd || !is_some) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }
    string_result = make_string_value(exec_env, cwd);
    if (!string_result)
        goto end;
    optional_result = wit_option_ctor(string_result);
    if (optional_result)
        string_result = NULL;

end:
    free_wit_value(string_result);
    if (!optional_result
        && wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
               == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct initial cwd");
    }
    if (optional_result)
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    optional_result);
    free_wit_value(optional_result);
    if (cwd)
        wasm_runtime_free(cwd);
}

/**
 * @brief Wrapper for the `exit` function of the `wasi:cli/exit` interface.
 * @param exec_env The execution environment.
 * @param status A result indicating success (0) or error (1).
 */
void
wasi_cli_exit_wrapper(wasm_exec_env_t exec_env, int32_t status)
{
    WASMModuleInstanceCommon *module_inst =
        wasm_runtime_get_module_inst(exec_env);
    WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    /* Match Preview 1 proc_exit: terminate this guest invocation by exception
       and let the embedding application recover.  Calling libc exit() here
       would let an untrusted component terminate the entire host process. */
    if (wasi_ctx)
        wasi_ctx->exit_code = (uint32_t)status;
    wasm_runtime_set_exception(module_inst, "wasi proc exit");
}

/**
 * @brief Wrapper for the `get-stdin` function of the `wasi:cli/stdin`
 * interface.
 * @param exec_env The execution environment.
 * @return The resource handle for stdin.
 */
int32_t
wasi_cli_get_stdin_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }

    wasi_input_stream_t _stdin = wasi_cli_get_stdin();

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_create(WASI_P2_IO_INPUT_STREAM,
                                            sizeof(StreamResourceType));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create stdin resource");
        return 0;
    }

    ((StreamResourceType *)hr->data)->fd = _stdin;
    ((StreamResourceType *)hr->data)->type = STREAM_TYPE_GENERIC;
    ((StreamResourceType *)hr->data)->position = 0;
    ((StreamResourceType *)hr->data)->position_valid = false;
    ((StreamResourceType *)hr->data)->append = false;

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not add stdin resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }

    return (int32_t)index_rep;
}

/**
 * @brief Wrapper for the `get-stdout` function of the `wasi:cli/stdout`
 * interface.
 * @param exec_env The execution environment.
 * @return The resource handle for stdout.
 */
int32_t
wasi_cli_get_stdout_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }

    wasi_output_stream_t _stdout = wasi_cli_get_stdout();

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_create(WASI_P2_IO_OUTPUT_STREAM,
                                            sizeof(StreamResourceType));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create stdout resource");
        return 0;
    }

    ((StreamResourceType *)hr->data)->fd = _stdout;
    ((StreamResourceType *)hr->data)->type = STREAM_TYPE_GENERIC;
    ((StreamResourceType *)hr->data)->position = 0;
    ((StreamResourceType *)hr->data)->position_valid = false;
    ((StreamResourceType *)hr->data)->append = false;

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not add stdout resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }

    return (int32_t)index_rep;
}

/**
 * @brief Wrapper for the `get-stderr` function of the `wasi:cli/stderr`
 * interface.
 * @param exec_env The execution environment.
 * @return The resource handle for stderr.
 */
int32_t
wasi_cli_get_stderr_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }

    wasi_output_stream_t _stderr = wasi_cli_get_stderr();

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_create(WASI_P2_IO_OUTPUT_STREAM,
                                            sizeof(StreamResourceType));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create stderr resource");
        return 0;
    }

    ((StreamResourceType *)hr->data)->fd = _stderr;
    ((StreamResourceType *)hr->data)->type = STREAM_TYPE_GENERIC;
    ((StreamResourceType *)hr->data)->position = 0;
    ((StreamResourceType *)hr->data)->position_valid = false;
    ((StreamResourceType *)hr->data)->append = false;

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not add stderr resource to HR table");
        return 0;
    }

    if (!lower_owned_host_resource(
            exec_env,
            func_type->results->result[0].type_specific.resource_handle,
            hr_table, index_rep, &index_rep)) {
        return 0;
    }

    return (int32_t)index_rep;
}

/**
 * @brief Wrapper for the `get-terminal-stdin` function of the
 * `wasi:cli/terminal-stdin` interface.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset for stored terminal input handle or
 * an empty value.
 */
void
wasi_cli_get_terminal_stdin_wrapper(wasm_exec_env_t exec_env,
                                    uint32_t offset_addr)
{
    bool is_some;
    wit_value_t optional_result = NULL;
    uint32_t owned_rep = 0;
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    wasi_terminal_input_t handle = wasi_cli_get_terminal_stdin(&is_some);
    if (!is_some) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }
    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr =
        host_resource_create(WASI_P2_TERMINAL_INPUT, sizeof(handle));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create terminal input resource");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    *((wasi_terminal_input_t *)hr->data) = handle;

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add terminal input resource to HR table");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }
    owned_rep = index_rep;

    optional_result = make_optional_owned_resource(index_rep);

end:
    if (!optional_result) {
        wasi_p2_cleanup_failed_owned_host_resources(exec_env, &owned_rep,
                                                    owned_rep ? 1 : 0);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate terminal input option");
    }
    else {
        (void)wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, func_type->results->result, optional_result,
            &owned_rep, owned_rep ? 1 : 0);
    }
    free_wit_value(optional_result);
}

/**
 * @brief Wrapper for the `get-terminal-stdout` function of the
 * `wasi:cli/terminal-stdout` interface.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset for stored terminal output handle or
 * an empty value.
 */
void
wasi_cli_get_terminal_stdout_wrapper(wasm_exec_env_t exec_env,
                                     uint32_t offset_addr)
{
    bool is_some;
    wit_value_t optional_result = NULL;
    uint32_t owned_rep = 0;

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    wasi_terminal_output_t handle = wasi_cli_get_terminal_stdout(&is_some);
    if (!is_some) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr =
        host_resource_create(WASI_P2_TERMINAL_OUTPUT, sizeof(handle));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create terminal output resource");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    *((wasi_terminal_output_t *)hr->data) = handle;

    int32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        LOG_VERBOSE("NO index");
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add terminal output resource to HR table");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }
    owned_rep = (uint32_t)index_rep;

    optional_result = make_optional_owned_resource((uint32_t)index_rep);

end:
    if (!optional_result) {
        wasi_p2_cleanup_failed_owned_host_resources(exec_env, &owned_rep,
                                                    owned_rep ? 1 : 0);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate terminal output option");
    }
    else {
        (void)wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, func_type->results->result, optional_result,
            &owned_rep, owned_rep ? 1 : 0);
    }
    free_wit_value(optional_result);
}

/**
 * @brief Wrapper for the `get-terminal-stderr` function of the
 * `wasi:cli/terminal-stderr` interface.
 * @param exec_env The execution environment.
 * @param[out] offset_addr Memory's offset for stored terminal output handle or
 * an empty value.
 */
void
wasi_cli_get_terminal_stderr_wrapper(wasm_exec_env_t exec_env,
                                     uint32_t offset_addr)
{
    bool is_some;
    wit_value_t optional_result = NULL;
    uint32_t owned_rep = 0;

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    wasi_terminal_output_t handle = wasi_cli_get_terminal_stderr(&is_some);
    if (!is_some) {
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr =
        host_resource_create(WASI_P2_TERMINAL_OUTPUT, sizeof(handle));

    if (!hr) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "Could not create terminal error resource");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }

    *((wasi_terminal_output_t *)hr->data) = handle;

    uint32_t index_rep = host_resource_table_add(hr_table, hr);
    if (index_rep < 1) {
        LOG_VERBOSE("NO index");
        destroy_host_resource(hr); // Clean up the HostResource on failure
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "Could not add terminal error resource to HR table");
        optional_result = wit_option_ctor(NULL);
        goto end;
    }
    owned_rep = index_rep;

    optional_result = make_optional_owned_resource(index_rep);

end:
    if (!optional_result) {
        wasi_p2_cleanup_failed_owned_host_resources(exec_env, &owned_rep,
                                                    owned_rep ? 1 : 0);
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate terminal error option");
    }
    else {
        (void)wasi_p2_store_owned_host_resource_result(
            exec_env, offset_addr, func_type->results->result, optional_result,
            &owned_rep, owned_rep ? 1 : 0);
    }
    free_wit_value(optional_result);
}
