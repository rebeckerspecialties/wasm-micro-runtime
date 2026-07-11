/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "wasm_export.h"

enum { ERROR_BUFFER_SIZE = 512 };

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
    RuntimeInitArgs init_args = { 0 };
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;
    WASMComponent *component = NULL;
    WASMComponentInstance *instance = NULL;
    WASMComponentPreparedCall *prepared_call = NULL;
    wasm_val_t result = { 0 };
    uint8_t *bytes = NULL;
    uint32_t size = 0;
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

    load_args.name = "rust-wasm32-wasip2-default-options";
    load_args.is_component = true;
    component =
        wasm_component_load(bytes, size, &load_args, error, sizeof(error));
    if (!component) {
        fprintf(stderr, "wasm32-wasip2 component load failed: %s\n", error);
        goto destroy_runtime;
    }
    if (!wasm_runtime_instantiation_args_create(&instantiation_args)) {
        fprintf(stderr, "component instantiation argument creation failed\n");
        goto unload_component;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(instantiation_args,
                                                           256 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 256 * 1024);
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
        || !wasm_component_prepared_call_post_return(prepared_call)
        || result.kind != WASM_I32 || result.of.i32 != 0) {
        const char *exception = wasm_component_runtime_get_exception(instance);
        fprintf(stderr, "default wasi:cli/run failed: %s\n",
                exception ? exception : error);
        goto deinstantiate;
    }

    puts("Rust wasm32-wasip2 public default-options smoke passed");
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
