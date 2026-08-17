/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"

enum {
    ERROR_BUFFER_SIZE = 512,
    OUTPUT_BUFFER_SIZE = 256,
    CUSTOM_DATA_COOKIE = 0xC0DEC0DE,
    STDOUT_REPRESENTATION = 101,
    STDERR_REPRESENTATION = 102,
};

typedef struct CommandHostState {
    uint32_t cookie;
    uint32_t output_stream_drops;
    uint32_t error_drops;
    uint32_t calls_with_context;
    uint32_t exit_status;
    char output[OUTPUT_BUFFER_SIZE];
    size_t output_size;
} CommandHostState;

static CommandHostState *
host_state(wasm_exec_env_t exec_env)
{
    CommandHostState *state =
        wasm_component_get_custom_data_from_exec_env(exec_env);
    if (state && state->cookie == CUSTOM_DATA_COOKIE)
        state->calls_with_context++;
    return state;
}

static void *
guest_range(wasm_exec_env_t exec_env, uint32_t offset, uint32_t size)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);

    if (!module_inst
        || !wasm_runtime_validate_app_addr(module_inst, offset, size))
        return NULL;
    return wasm_runtime_addr_app_to_native(module_inst, offset);
}

static void
host_error_to_debug_string(wasm_exec_env_t exec_env, uint64_t *cells)
{
    uint32_t *result = guest_range(exec_env, (uint32_t)cells[1], 8);

    (void)host_state(exec_env);
    if (result) {
        result[0] = 0;
        result[1] = 0;
    }
}

static void
host_blocking_write_and_flush(wasm_exec_env_t exec_env, uint64_t *cells)
{
    CommandHostState *state = host_state(exec_env);
    uint32_t stream_representation = 0;
    uint32_t contents_offset = (uint32_t)cells[1];
    uint32_t contents_size = (uint32_t)cells[2];
    const char *contents =
        guest_range(exec_env, contents_offset, contents_size);
    uint32_t *result = guest_range(exec_env, (uint32_t)cells[3], 8);

    if (state && contents
        && !wasm_component_host_resource_rep(
            exec_env, "wasi:io/streams@0.2.5", "output-stream",
            (uint32_t)cells[0], &stream_representation)
        && wasm_component_host_resource_rep(exec_env, "wasi:io/streams@0.2.6",
                                            "output-stream", (uint32_t)cells[0],
                                            &stream_representation)
        && stream_representation == STDOUT_REPRESENTATION) {
        size_t available = sizeof(state->output) - state->output_size - 1;
        size_t copy_size =
            contents_size < available ? contents_size : available;

        memcpy(state->output + state->output_size, contents, copy_size);
        state->output_size += copy_size;
        state->output[state->output_size] = '\0';
    }
    if (result) {
        result[0] = 0;
        result[1] = 0;
    }
}

static void
host_get_environment(wasm_exec_env_t exec_env, uint64_t *cells)
{
    uint32_t *result = guest_range(exec_env, (uint32_t)cells[0], 8);

    (void)host_state(exec_env);
    if (result) {
        result[0] = 0;
        result[1] = 0;
    }
}

static void
host_exit(wasm_exec_env_t exec_env, uint64_t *cells)
{
    CommandHostState *state = host_state(exec_env);

    if (state)
        state->exit_status = (uint32_t)cells[0];
}

static void
host_get_stdout(wasm_exec_env_t exec_env, uint64_t *cells)
{
    uint32_t handle = 0;

    (void)host_state(exec_env);
    if (!wasm_component_host_resource_new(exec_env, "wasi:io/streams@0.2.5",
                                          "output-stream",
                                          STDOUT_REPRESENTATION, &handle)
        && wasm_component_host_resource_new(exec_env, "wasi:io/streams@0.2.6",
                                            "output-stream",
                                            STDOUT_REPRESENTATION, &handle)) {
        cells[0] = handle;
    }
}

static void
host_get_stderr(wasm_exec_env_t exec_env, uint64_t *cells)
{
    uint32_t handle = 0;

    (void)host_state(exec_env);
    if (wasm_component_host_resource_new(exec_env, "wasi:io/streams@0.2.6",
                                         "output-stream", STDERR_REPRESENTATION,
                                         &handle)) {
        cells[0] = handle;
    }
}

static bool
drop_output_stream(void *attachment, uint32_t representation)
{
    CommandHostState *state = attachment;

    if (!state || state->cookie != CUSTOM_DATA_COOKIE
        || (representation != STDOUT_REPRESENTATION
            && representation != STDERR_REPRESENTATION))
        return false;
    state->output_stream_drops++;
    return true;
}

static bool
drop_error(void *attachment, uint32_t representation)
{
    CommandHostState *state = attachment;

    if (!state || state->cookie != CUSTOM_DATA_COOKIE)
        return false;
    (void)representation;
    state->error_drops++;
    return true;
}

static uint8_t *
read_file(const char *path, uint32_t *size_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long size = 0;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
        || size > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (uint32_t)size;
    return bytes;
}

int
main(int argc, char **argv)
{
    static NativeSymbol error_symbols[] = {
        { "[method]error.to-debug-string", (void *)host_error_to_debug_string,
          NULL, NULL },
    };
    static NativeSymbol stream_symbols[] = {
        { "[method]output-stream.blocking-write-and-flush",
          (void *)host_blocking_write_and_flush, NULL, NULL },
    };
    static NativeSymbol environment_symbols[] = {
        { "get-environment", (void *)host_get_environment, NULL, NULL },
    };
    static NativeSymbol exit_symbols[] = {
        { "exit", (void *)host_exit, NULL, NULL },
    };
    static NativeSymbol stdout_symbols[] = {
        { "get-stdout", (void *)host_get_stdout, NULL, NULL },
    };
    static NativeSymbol stderr_symbols[] = {
        { "get-stderr", (void *)host_get_stderr, NULL, NULL },
    };
    RuntimeInitArgs init_args = { 0 };
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;
    WASMComponent *component = NULL;
    WASMComponentInstance *instance = NULL;
    WASMComponentPreparedCall *prepared_call = NULL;
    CommandHostState state = {
        .cookie = CUSTOM_DATA_COOKIE,
        .exit_status = UINT32_MAX,
    };
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    wasm_val_t result = { 0 };
    char error[ERROR_BUFFER_SIZE] = { 0 };
    int exit_code = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <wasm32-wasip2-command.wasm>\n", argv[0]);
        return 2;
    }
    bytes = read_file(argv[1], &size);
    if (!bytes) {
        fprintf(stderr, "failed to read wasm32-wasip2 command: %s\n", argv[1]);
        return 1;
    }

    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        goto cleanup;
    }
#define REGISTER_RAW(interface_name, symbols) \
    wasm_runtime_register_natives_raw(        \
        interface_name, symbols,              \
        (uint32_t)(sizeof(symbols) / sizeof((symbols)[0])))
    if (!REGISTER_RAW("wasi:io/error@0.2.6", error_symbols)
        || !REGISTER_RAW("wasi:io/streams@0.2.6", stream_symbols)
        || !REGISTER_RAW("wasi:cli/environment@0.2.6", environment_symbols)
        || !REGISTER_RAW("wasi:cli/exit@0.2.6", exit_symbols)
        || !REGISTER_RAW("wasi:cli/stdout@0.2.6", stdout_symbols)
        || !REGISTER_RAW("wasi:cli/stderr@0.2.6", stderr_symbols)) {
        fprintf(stderr, "wasm32-wasip2 host registration failed\n");
        goto destroy_runtime;
    }
#undef REGISTER_RAW

    load_args.name = "rust-wasm32-wasip2-command";
    load_args.is_component = true;
    component =
        wasm_component_load(bytes, size, &load_args, error, sizeof(error));
    if (!component) {
        fprintf(stderr, "wasm32-wasip2 component load failed: %s\n", error);
        goto destroy_runtime;
    }
    if (!wasm_component_register_host_resource_drop_callback(
            component, "wasi:io/error@0.2.6", "error", drop_error)
        || !wasm_component_register_host_resource_drop_callback(
            component, "wasi:io/streams@0.2.6", "output-stream",
            drop_output_stream)) {
        fprintf(stderr, "wasm32-wasip2 resource registration failed\n");
        goto unload_component;
    }
    if (!wasm_runtime_instantiation_args_create(&instantiation_args)) {
        fprintf(stderr, "component instantiation argument creation failed\n");
        goto unload_component;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(instantiation_args,
                                                           256 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 256 * 1024);
    wasm_runtime_instantiation_args_set_custom_data(instantiation_args, &state);
    instance = wasm_component_instantiate_ex2(component, instantiation_args,
                                              error, sizeof(error));
    wasm_runtime_instantiation_args_destroy(instantiation_args);
    instantiation_args = NULL;
    if (!instance) {
        fprintf(stderr, "wasm32-wasip2 component instantiation failed: %s\n",
                error);
        goto unload_component;
    }

    prepared_call = wasm_component_prepare_export_call_qualified(
        instance, "wasi:cli/run@0.2.6", "run", error, sizeof(error));
    if (!prepared_call
        || !wasm_component_call_prepared(prepared_call, 1, &result, 0, NULL)
        || !wasm_component_prepared_call_post_return(prepared_call)) {
        const char *exception = wasm_component_runtime_get_exception(instance);
        fprintf(stderr, "wasi:cli/run failed: %s\n",
                exception ? exception : error);
        goto deinstantiate;
    }
    if (result.kind != WASM_I32 || result.of.i32 != 0
        || strcmp(state.output, "wamr wasm32-wasip2 smoke\n") != 0
        || state.calls_with_context == 0 || state.exit_status != UINT32_MAX) {
        fprintf(stderr,
                "wasi:cli/run returned kind=%u value=%d output=%s calls=%u "
                "exit=%u\n",
                result.kind, result.of.i32, state.output,
                state.calls_with_context, state.exit_status);
        goto deinstantiate;
    }

    puts("Rust wasm32-wasip2 command smoke passed");
    exit_code = 0;

deinstantiate:
    wasm_component_destroy_prepared_call(prepared_call);
    wasm_component_deinstantiate(instance);
    if (exit_code == 0 && state.output_stream_drops == 0) {
        fprintf(stderr, "expected an output-stream drop, observed none\n");
        exit_code = 1;
    }
unload_component:
    if (instantiation_args)
        wasm_runtime_instantiation_args_destroy(instantiation_args);
    wasm_component_unload(component);
destroy_runtime:
    wasm_runtime_destroy();
cleanup:
    free(bytes);
    return exit_code;
}
