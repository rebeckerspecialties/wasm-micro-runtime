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
    PROBE_OFFSET = 32,
    PROBE_SIZE = 4,
    WASM_PAGE_SIZE = 64 * 1024,
};

typedef enum ProbeMode {
    PROBE_VALID,
    PROBE_INVALID_RANGES,
} ProbeMode;

typedef struct MemoryProbeState {
    ProbeMode mode;
    wasm_exec_env_t callback_exec_env;
    bool valid_const;
    bool valid_mutable;
    bool pointers_match;
    bool mutable_round_trip;
    bool empty_at_end;
    bool null_output_rejected;
    bool bounds_rejected;
    bool overflow_rejected;
    bool failed_outputs_cleared;
} MemoryProbeState;

static bool saw_custom_data;
static int32_t exact_increment = 1;
static int32_t fallback_increment = 100;
static MemoryProbeState memory_probe;

static void
bump_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    const uint32_t *custom_data =
        wasm_component_get_custom_data_from_exec_env(exec_env);
    const int32_t *increment = wasm_runtime_get_function_attachment(exec_env);
    uint8_t *mutable_data = NULL;
    const uint8_t *const_data = NULL;

    saw_custom_data = custom_data && *custom_data == CUSTOM_DATA_COOKIE;
    memory_probe.callback_exec_env = exec_env;

    if (memory_probe.mode == PROBE_INVALID_RANGES) {
        uint8_t *mutable_sentinel = (uint8_t *)(uintptr_t)1;
        const uint8_t *const_sentinel = (const uint8_t *)(uintptr_t)1;

        memory_probe.bounds_rejected =
            !wasm_component_validate_memory_range(exec_env, 0, UINT32_MAX)
            && !wasm_component_get_memory_range(exec_env, 0, UINT32_MAX,
                                                &mutable_sentinel);
        memory_probe.overflow_rejected =
            !wasm_component_validate_memory_range(exec_env, UINT32_MAX, 2)
            && !wasm_component_get_memory_range_const(exec_env, UINT32_MAX, 2,
                                                      &const_sentinel);
        memory_probe.failed_outputs_cleared =
            mutable_sentinel == NULL && const_sentinel == NULL;
        return;
    }

    memory_probe.valid_const = wasm_component_get_memory_range_const(
        exec_env, PROBE_OFFSET, PROBE_SIZE, &const_data);
    memory_probe.valid_mutable = wasm_component_get_memory_range(
        exec_env, PROBE_OFFSET, PROBE_SIZE, &mutable_data);
    memory_probe.pointers_match =
        memory_probe.valid_const && memory_probe.valid_mutable
        && const_data == mutable_data
        && memcmp(const_data, "wamr", PROBE_SIZE) == 0;
    if (memory_probe.pointers_match) {
        mutable_data[0] = 'W';
        memory_probe.mutable_round_trip = const_data[0] == 'W';
        mutable_data[0] = 'w';
    }
    memory_probe.empty_at_end =
        wasm_component_validate_memory_range(exec_env, WASM_PAGE_SIZE, 0);
    memory_probe.null_output_rejected = !wasm_component_get_memory_range(
        exec_env, PROBE_OFFSET, PROBE_SIZE, NULL);
    canonical_cells[0] =
        (uint32_t)((int32_t)canonical_cells[0] + (increment ? *increment : 0));
}

static bool
rejects_non_callback_context(wasm_exec_env_t exec_env)
{
    uint8_t *mutable_sentinel = (uint8_t *)(uintptr_t)1;
    const uint8_t *const_sentinel = (const uint8_t *)(uintptr_t)1;

    return !wasm_component_validate_memory_range(exec_env, 0, 0)
           && !wasm_component_get_memory_range(exec_env, 0, 0,
                                               &mutable_sentinel)
           && !wasm_component_get_memory_range_const(exec_env, 0, 0,
                                                     &const_sentinel)
           && mutable_sentinel == NULL && const_sentinel == NULL;
}

static bool
rejects_standalone_core_context(char *error, uint32_t error_size)
{
    static uint8_t empty_core_module[] = { 0x00, 0x61, 0x73, 0x6d,
                                           0x01, 0x00, 0x00, 0x00 };
    wasm_module_t module = NULL;
    wasm_module_inst_t instance = NULL;
    wasm_exec_env_t exec_env = NULL;
    bool rejected = false;

    module = wasm_runtime_load(empty_core_module, sizeof(empty_core_module),
                               error, error_size);
    if (!module)
        return false;
    instance = wasm_runtime_instantiate(module, 8 * 1024, 0, error, error_size);
    if (!instance)
        goto unload;
    exec_env = wasm_runtime_create_exec_env(instance, 8 * 1024);
    if (!exec_env)
        goto deinstantiate;

    rejected = rejects_non_callback_context(exec_env);
    wasm_runtime_destroy_exec_env(exec_env);
deinstantiate:
    wasm_runtime_deinstantiate(instance);
unload:
    wasm_runtime_unload(module);
    return rejected;
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
    /* The fixture's one-page memory is deliberately exact so the empty-range
     * check can exercise the wasm32 end address. It does not allocate. */
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 0);
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
    if (!memory_probe.valid_const || !memory_probe.valid_mutable
        || !memory_probe.pointers_match || !memory_probe.mutable_round_trip
        || !memory_probe.empty_at_end || !memory_probe.null_output_rejected) {
        fprintf(stderr,
                "valid component callback memory probe failed: "
                "const=%d mutable=%d match=%d round-trip=%d end=%d null=%d\n",
                memory_probe.valid_const, memory_probe.valid_mutable,
                memory_probe.pointers_match, memory_probe.mutable_round_trip,
                memory_probe.empty_at_end, memory_probe.null_output_rejected);
        goto deinstantiate;
    }
    if (!rejects_non_callback_context(NULL)
        || !rejects_non_callback_context(memory_probe.callback_exec_env)
        || wasm_runtime_get_function_attachment(memory_probe.callback_exec_env)
               != NULL
        || !rejects_standalone_core_context(error, sizeof(error))) {
        fprintf(stderr,
                "component memory API accepted a non-callback context: %s\n",
                error);
        goto deinstantiate;
    }

    memset(&memory_probe, 0, sizeof(memory_probe));
    memory_probe.mode = PROBE_INVALID_RANGES;
    if (wasm_component_call_prepared(prepared_call, 1, &result, 1, &argument)
        || !memory_probe.bounds_rejected || !memory_probe.overflow_rejected
        || !memory_probe.failed_outputs_cleared) {
        fprintf(stderr, "invalid component callback memory probe failed\n");
        goto deinstantiate;
    }

    puts("component exact static-host import and memory smoke passed");
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
