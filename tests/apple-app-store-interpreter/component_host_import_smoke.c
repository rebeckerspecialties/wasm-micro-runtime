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
    ERROR_BUFFER_SIZE = 256,
    CUSTOM_DATA_COOKIE = 0xC0DEC0DE,
};

static bool saw_custom_data;
static int32_t exact_increment = 1;
static int32_t fallback_increment = 100;

static void
bump_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    const uint32_t *custom_data =
        wasm_component_get_custom_data_from_exec_env(exec_env);
    const int32_t *increment = wasm_runtime_get_function_attachment(exec_env);

    saw_custom_data = custom_data && *custom_data == CUSTOM_DATA_COOKIE;
    canonical_cells[0] =
        (uint32_t)((int32_t)canonical_cells[0] + (increment ? *increment : 0));
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
    static NativeSymbol exact_symbols[] = {
        { "bump", (void *)bump_raw, "(i)i", &exact_increment },
    };
    static NativeSymbol fallback_symbols[] = {
        { "bump", (void *)bump_raw, "(i)i", &fallback_increment },
    };
    RuntimeInitArgs init_args = { 0 };
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;
    WASMComponent *component = NULL;
    WASMComponentInstance *instance = NULL;
    WASMComponentPreparedCall *prepared_call = NULL;
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    uint32_t custom_data = CUSTOM_DATA_COOKIE;
    wasm_val_t argument = { .kind = WASM_I32, .of.i32 = 41 };
    wasm_val_t result = { 0 };
    char error[ERROR_BUFFER_SIZE] = { 0 };
    int exit_code = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <component.wasm>\n", argv[0]);
        return 2;
    }
    bytes = read_file(argv[1], &size);
    if (!bytes) {
        fprintf(stderr, "failed to read static-host component: %s\n", argv[1]);
        return 1;
    }

    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        goto cleanup;
    }
    if (!wasm_runtime_register_natives_raw(
            "test:project/static-host", fallback_symbols,
            (uint32_t)(sizeof(fallback_symbols) / sizeof(fallback_symbols[0])))
        || !wasm_runtime_register_natives_raw(
            "test:project/static-host@0.1.0", exact_symbols,
            (uint32_t)(sizeof(exact_symbols) / sizeof(exact_symbols[0])))) {
        fprintf(stderr, "static host registration failed\n");
        goto destroy_runtime;
    }

    load_args.name = "static-host-smoke";
    load_args.is_component = true;
    component =
        wasm_component_load(bytes, size, &load_args, error, sizeof(error));
    if (!component) {
        fprintf(stderr, "component load failed: %s\n", error);
        goto destroy_runtime;
    }
    if (!wasm_runtime_instantiation_args_create(&instantiation_args)) {
        fprintf(stderr, "component instantiation argument creation failed\n");
        goto unload_component;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(instantiation_args,
                                                           64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 64 * 1024);
    wasm_runtime_instantiation_args_set_custom_data(instantiation_args,
                                                    &custom_data);
    instance = wasm_component_instantiate_ex2(component, instantiation_args,
                                              error, sizeof(error));
    wasm_runtime_instantiation_args_destroy(instantiation_args);
    instantiation_args = NULL;
    if (!instance) {
        fprintf(stderr, "component instantiation failed: %s\n", error);
        goto unload_component;
    }

    prepared_call = wasm_component_prepare_export_call(instance, "call", error,
                                                       sizeof(error));
    if (!prepared_call
        || !wasm_component_call_prepared(prepared_call, 1, &result, 1,
                                         &argument)
        || !wasm_component_prepared_call_post_return(prepared_call)) {
        const char *exception = wasm_component_runtime_get_exception(instance);
        fprintf(stderr, "component host call failed: %s\n",
                exception ? exception : error);
        goto deinstantiate;
    }
    if (result.kind != WASM_I32 || result.of.i32 != 42 || !saw_custom_data) {
        fprintf(stderr,
                "component host call returned kind=%u value=%d context=%d\n",
                result.kind, result.of.i32, saw_custom_data);
        goto deinstantiate;
    }

    puts("component exact static-host import smoke passed");
    exit_code = 0;

deinstantiate:
    wasm_component_destroy_prepared_call(prepared_call);
    wasm_component_deinstantiate(instance);
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
