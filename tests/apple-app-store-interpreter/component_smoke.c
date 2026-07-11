/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <sched.h>

#include "wasm_export.h"

enum {
    ERROR_BUFFER_SIZE = 256,
    CALLS_PER_CYCLE = 8,
    WORKER_START_STOP_CYCLES = 4,
};

typedef struct LoadedComponent {
    uint8_t *bytes;
    WASMComponent *component;
    WASMComponentInstance *instance;
} LoadedComponent;

typedef struct WorkerCall {
    const uint8_t *bytes;
    uint32_t size;
    atomic_bool ready;
    atomic_bool start;
    bool initialized;
    bool succeeded;
    char error[ERROR_BUFFER_SIZE];
} WorkerCall;

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

static bool
load_component(const uint8_t *source_bytes, uint32_t size, const char *name,
               LoadedComponent *loaded, char *error)
{
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;

    memset(loaded, 0, sizeof(*loaded));
    loaded->bytes = malloc(size);
    if (!loaded->bytes) {
        snprintf(error, ERROR_BUFFER_SIZE, "failed to copy %s bytes", name);
        return false;
    }
    memcpy(loaded->bytes, source_bytes, size);

    load_args.name = (char *)name;
    load_args.wasm_binary_freeable = false;
    load_args.clone_wasm_binary = false;
    load_args.no_resolve = false;
    load_args.is_component = true;
    loaded->component = wasm_component_load(loaded->bytes, size, &load_args,
                                            error, ERROR_BUFFER_SIZE);
    if (!loaded->component) {
        goto fail;
    }
    if (!wasm_runtime_instantiation_args_create(&instantiation_args)) {
        snprintf(error, ERROR_BUFFER_SIZE,
                 "failed to create %s instantiation arguments", name);
        goto fail;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(instantiation_args,
                                                           64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 64 * 1024);

    loaded->instance = wasm_component_instantiate_ex2(
        loaded->component, instantiation_args, error, ERROR_BUFFER_SIZE);
    wasm_runtime_instantiation_args_destroy(instantiation_args);
    instantiation_args = NULL;
    if (!loaded->instance) {
        goto fail;
    }
    return true;

fail:
    if (instantiation_args)
        wasm_runtime_instantiation_args_destroy(instantiation_args);
    if (loaded->component)
        wasm_component_unload(loaded->component);
    free(loaded->bytes);
    memset(loaded, 0, sizeof(*loaded));
    return false;
}

static void
unload_component(LoadedComponent *loaded)
{
    if (loaded->instance)
        wasm_component_deinstantiate(loaded->instance);
    if (loaded->component)
        wasm_component_unload(loaded->component);
    free(loaded->bytes);
    memset(loaded, 0, sizeof(*loaded));
}

static bool
call_add(WASMComponentPreparedCall *prepared_call,
         WASMComponentInstance *instance, char *error)
{
    wasm_val_t arguments[2] = {
        { .kind = WASM_I32, .of.i32 = 20 },
        { .kind = WASM_I32, .of.i32 = 22 },
    };
    wasm_val_t result = { 0 };

    if (!wasm_component_call_prepared(prepared_call, 1, &result, 2,
                                      arguments)) {
        const char *exception = wasm_component_runtime_get_exception(instance);
        snprintf(error, ERROR_BUFFER_SIZE, "component add failed: %s",
                 exception ? exception : "no exception");
        return false;
    }
    if (!wasm_component_prepared_call_post_return(prepared_call)) {
        const char *exception = wasm_component_runtime_get_exception(instance);
        snprintf(error, ERROR_BUFFER_SIZE, "component post-return failed: %s",
                 exception ? exception : "no exception");
        return false;
    }
    if (result.kind != WASM_I32 || result.of.i32 != 42) {
        snprintf(error, ERROR_BUFFER_SIZE,
                 "component add returned kind=%u result=%d", result.kind,
                 result.of.i32);
        return false;
    }
    return true;
}

static void *
run_worker(void *argument)
{
    WorkerCall *worker = argument;
    LoadedComponent loaded = { 0 };
    WASMComponentPreparedCall *prepared_call = NULL;
    uint32_t call_index = 0;

    worker->initialized = wasm_runtime_init_thread_env();
    if (!worker->initialized) {
        snprintf(worker->error, sizeof(worker->error),
                 "worker thread environment initialization failed");
        atomic_store_explicit(&worker->ready, true, memory_order_release);
        return NULL;
    }

    if (!load_component(worker->bytes, worker->size, "audio-worker", &loaded,
                        worker->error)) {
        atomic_store_explicit(&worker->ready, true, memory_order_release);
        wasm_runtime_destroy_thread_env();
        return NULL;
    }
    prepared_call = wasm_component_prepare_export_call_qualified(
        loaded.instance, "test:project/my-interface@0.1.0", "add",
        worker->error, sizeof(worker->error));
    if (!prepared_call) {
        unload_component(&loaded);
        atomic_store_explicit(&worker->ready, true, memory_order_release);
        wasm_runtime_destroy_thread_env();
        return NULL;
    }

    atomic_store_explicit(&worker->ready, true, memory_order_release);
    while (!atomic_load_explicit(&worker->start, memory_order_acquire))
        sched_yield();

    worker->succeeded = true;
    for (call_index = 0; call_index < CALLS_PER_CYCLE; ++call_index) {
        if (!call_add(prepared_call, loaded.instance, worker->error)) {
            worker->succeeded = false;
            break;
        }
    }

    wasm_component_destroy_prepared_call(prepared_call);
    unload_component(&loaded);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

int
main(int argc, char **argv)
{
    RuntimeInitArgs init_args;
    LoadedComponent main_component = { 0 };
    WASMComponentPreparedCall *main_call = NULL;
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    uint32_t cycle = 0;
    int exit_code = 1;
    char error[ERROR_BUFFER_SIZE] = { 0 };

    if (argc != 2) {
        fprintf(stderr, "usage: %s <component.wasm>\n", argv[0]);
        return 2;
    }
    bytes = read_file(argv[1], &size);
    if (!bytes) {
        fprintf(stderr, "failed to read component fixture: %s\n", argv[1]);
        return 1;
    }

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        goto cleanup;
    }
    if (!load_component(bytes, size, "main", &main_component, error)) {
        fprintf(stderr, "main component load failed: %s\n", error);
        goto destroy_runtime;
    }
    main_call = wasm_component_prepare_export_call_qualified(
        main_component.instance, "test:project/my-interface@0.1.0", "add",
        error, sizeof(error));
    if (!main_call) {
        fprintf(stderr, "main component prepare failed: %s\n", error);
        goto unload_main;
    }

    for (cycle = 0; cycle < WORKER_START_STOP_CYCLES; ++cycle) {
        WorkerCall worker = {
            .bytes = bytes,
            .size = size,
            .ready = ATOMIC_VAR_INIT(false),
            .start = ATOMIC_VAR_INIT(false),
            .initialized = false,
            .succeeded = false,
            .error = { 0 },
        };
        pthread_t thread;
        uint32_t call_index = 0;

        if (pthread_create(&thread, NULL, run_worker, &worker) != 0) {
            fprintf(stderr, "worker thread creation failed\n");
            goto unload_main;
        }
        while (!atomic_load_explicit(&worker.ready, memory_order_acquire))
            sched_yield();
        if (!worker.initialized) {
            pthread_join(thread, NULL);
            fprintf(stderr, "%s\n", worker.error);
            goto unload_main;
        }

        atomic_store_explicit(&worker.start, true, memory_order_release);
        for (call_index = 0; call_index < CALLS_PER_CYCLE; ++call_index) {
            if (!call_add(main_call, main_component.instance, error)) {
                pthread_join(thread, NULL);
                fprintf(stderr, "%s\n", error);
                goto unload_main;
            }
        }
        pthread_join(thread, NULL);
        if (!worker.succeeded) {
            fprintf(stderr, "worker component failed: %s\n", worker.error);
            goto unload_main;
        }
    }

    puts("component main/worker start-stop smoke passed");
    exit_code = 0;

unload_main:
    wasm_component_destroy_prepared_call(main_call);
    unload_component(&main_component);
destroy_runtime:
    wasm_runtime_destroy();
cleanup:
    free(bytes);
    return exit_code;
}
