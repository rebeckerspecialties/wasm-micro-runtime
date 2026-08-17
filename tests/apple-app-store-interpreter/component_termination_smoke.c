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
#include <time.h>

#include <pthread.h>
#include <sched.h>

#include "wasm_export.h"

enum {
    ERROR_BUFFER_SIZE = 256,
    THREAD_START_TIMEOUT_MILLISECONDS = 1000,
    TERMINATION_TIMEOUT_MILLISECONDS = 2000,
};

/*
 * Generated from spin_component.wat with `wasm-tools parse`.  Embedding the
 * tiny fixture keeps the Apple smoke independent of a component toolchain.
 */
static const uint8_t spin_component_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00, 0x01, 0x43, 0x00, 0x61,
    0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x73, 0x70, 0x69, 0x6e,
    0x00, 0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b,
    0x0b, 0x00, 0x1a, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x07, 0x06, 0x6d,
    0x6f, 0x64, 0x75, 0x6c, 0x65, 0x03, 0x0a, 0x01, 0x00, 0x01, 0x00, 0x05,
    0x61, 0x67, 0x61, 0x69, 0x6e, 0x02, 0x04, 0x01, 0x00, 0x00, 0x00, 0x06,
    0x0a, 0x01, 0x00, 0x00, 0x01, 0x00, 0x04, 0x73, 0x70, 0x69, 0x6e, 0x07,
    0x05, 0x01, 0x40, 0x00, 0x01, 0x00, 0x08, 0x06, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0b, 0x0a, 0x01, 0x00, 0x04, 0x73, 0x70, 0x69, 0x6e, 0x01,
    0x00, 0x00, 0x00, 0x54, 0x0e, 0x63, 0x6f, 0x6d, 0x70, 0x6f, 0x6e, 0x65,
    0x6e, 0x74, 0x2d, 0x6e, 0x61, 0x6d, 0x65, 0x01, 0x09, 0x00, 0x00, 0x01,
    0x00, 0x04, 0x73, 0x70, 0x69, 0x6e, 0x01, 0x0b, 0x00, 0x11, 0x01, 0x00,
    0x06, 0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x01, 0x0d, 0x00, 0x12, 0x01,
    0x00, 0x08, 0x69, 0x6e, 0x73, 0x74, 0x61, 0x6e, 0x63, 0x65, 0x01, 0x0d,
    0x01, 0x01, 0x00, 0x09, 0x73, 0x70, 0x69, 0x6e, 0x2d, 0x66, 0x75, 0x6e,
    0x63, 0x01, 0x0d, 0x03, 0x01, 0x00, 0x09, 0x73, 0x70, 0x69, 0x6e, 0x2d,
    0x74, 0x79, 0x70, 0x65,
};

_Static_assert(sizeof(spin_component_wasm) == 208,
               "spin component fixture changed size");

typedef struct LoadedComponent {
    uint8_t *bytes;
    WASMComponent *component;
    WASMComponentInstance *instance;
} LoadedComponent;

typedef struct TerminationCall {
    _Atomic(WASMComponentInstance *) instance;
    atomic_bool ready;
    atomic_bool start;
    atomic_bool call_started;
    atomic_bool done;
    bool initialized;
    bool prepared;
    bool call_succeeded;
    char error[ERROR_BUFFER_SIZE];
    char exception[ERROR_BUFFER_SIZE];
} TerminationCall;

static uint64_t
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool
wait_for_flag(atomic_bool *flag, uint32_t timeout_milliseconds)
{
    const uint64_t start = monotonic_milliseconds();
    const struct timespec pause = {
        .tv_sec = 0,
        .tv_nsec = 1000 * 1000,
    };

    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        const uint64_t now = monotonic_milliseconds();

        if (!start || !now || now - start >= timeout_milliseconds)
            return false;
        nanosleep(&pause, NULL);
    }
    return true;
}

static bool
load_component(LoadedComponent *loaded, char *error)
{
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;

    memset(loaded, 0, sizeof(*loaded));
    loaded->bytes = malloc(sizeof(spin_component_wasm));
    if (!loaded->bytes) {
        snprintf(error, ERROR_BUFFER_SIZE, "failed to copy spin component");
        return false;
    }
    memcpy(loaded->bytes, spin_component_wasm, sizeof(spin_component_wasm));

    load_args.name = "termination-spin";
    load_args.wasm_binary_freeable = false;
    load_args.clone_wasm_binary = false;
    load_args.no_resolve = false;
    load_args.is_component = true;
    loaded->component =
        wasm_component_load(loaded->bytes, sizeof(spin_component_wasm),
                            &load_args, error, ERROR_BUFFER_SIZE);
    if (!loaded->component) {
        goto fail;
    }
    if (!wasm_runtime_instantiation_args_create(&instantiation_args)) {
        snprintf(error, ERROR_BUFFER_SIZE,
                 "failed to create termination instantiation arguments");
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

static void *
run_termination_worker(void *argument)
{
    TerminationCall *call = argument;
    LoadedComponent loaded = { 0 };
    WASMComponentPreparedCall *prepared_call = NULL;
    const char *exception = NULL;

    call->initialized = wasm_runtime_init_thread_env();
    if (!call->initialized) {
        snprintf(call->error, sizeof(call->error),
                 "worker thread environment initialization failed");
        atomic_store_explicit(&call->ready, true, memory_order_release);
        atomic_store_explicit(&call->done, true, memory_order_release);
        return NULL;
    }

    if (!load_component(&loaded, call->error)) {
        atomic_store_explicit(&call->ready, true, memory_order_release);
        goto done;
    }
    atomic_store_explicit(&call->instance, loaded.instance,
                          memory_order_release);
    prepared_call = wasm_component_prepare_export_call(
        loaded.instance, "spin", call->error, sizeof(call->error));
    call->prepared = prepared_call != NULL;
    atomic_store_explicit(&call->ready, true, memory_order_release);
    if (!prepared_call)
        goto done;

    while (!atomic_load_explicit(&call->start, memory_order_acquire))
        sched_yield();

    atomic_store_explicit(&call->call_started, true, memory_order_release);
    call->call_succeeded =
        wasm_component_call_prepared(prepared_call, 0, NULL, 0, NULL);
    exception = wasm_component_runtime_get_exception(loaded.instance);
    if (exception)
        snprintf(call->exception, sizeof(call->exception), "%s", exception);

    if (call->call_succeeded)
        wasm_component_prepared_call_post_return(prepared_call);
    wasm_component_destroy_prepared_call(prepared_call);

done:
    unload_component(&loaded);
    wasm_runtime_destroy_thread_env();
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

static void
fail_bounded_wait(WASMComponentInstance *instance, const char *message,
                  uint32_t timeout_milliseconds)
{
    fprintf(stderr, "%s within %u ms\n", message, timeout_milliseconds);
    wasm_component_terminate(instance);
    _Exit(1);
}

int
main(void)
{
    const struct timespec settle_time = {
        .tv_sec = 0,
        .tv_nsec = 20 * 1000 * 1000,
    };
    RuntimeInitArgs init_args;
    TerminationCall call = {
        .instance = ATOMIC_VAR_INIT(NULL),
        .ready = ATOMIC_VAR_INIT(false),
        .start = ATOMIC_VAR_INIT(false),
        .call_started = ATOMIC_VAR_INIT(false),
        .done = ATOMIC_VAR_INIT(false),
        .initialized = false,
        .prepared = false,
        .call_succeeded = false,
        .error = { 0 },
        .exception = { 0 },
    };
    pthread_t thread;
    int exit_code = 1;

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        return 1;
    }
    if (pthread_create(&thread, NULL, run_termination_worker, &call) != 0) {
        fprintf(stderr, "termination worker thread creation failed\n");
        goto destroy_runtime;
    }
    if (!wait_for_flag(&call.ready, THREAD_START_TIMEOUT_MILLISECONDS))
        fail_bounded_wait(
            atomic_load_explicit(&call.instance, memory_order_acquire),
            "termination worker did not become ready",
            THREAD_START_TIMEOUT_MILLISECONDS);
    if (!call.initialized || !call.prepared) {
        if (!wait_for_flag(&call.done, TERMINATION_TIMEOUT_MILLISECONDS))
            fail_bounded_wait(
                atomic_load_explicit(&call.instance, memory_order_acquire),
                "failed termination worker did not stop",
                TERMINATION_TIMEOUT_MILLISECONDS);
        pthread_join(thread, NULL);
        fprintf(stderr, "termination worker preparation failed: %s\n",
                call.error[0] ? call.error : "no error");
        goto destroy_runtime;
    }

    atomic_store_explicit(&call.start, true, memory_order_release);
    if (!wait_for_flag(&call.call_started, THREAD_START_TIMEOUT_MILLISECONDS))
        fail_bounded_wait(
            atomic_load_explicit(&call.instance, memory_order_acquire),
            "termination worker did not start its call",
            THREAD_START_TIMEOUT_MILLISECONDS);

    nanosleep(&settle_time, NULL);
    wasm_component_terminate(
        atomic_load_explicit(&call.instance, memory_order_acquire));
    if (!wait_for_flag(&call.done, TERMINATION_TIMEOUT_MILLISECONDS))
        fail_bounded_wait(
            atomic_load_explicit(&call.instance, memory_order_acquire),
            "component spin call did not terminate",
            TERMINATION_TIMEOUT_MILLISECONDS);
    if (pthread_join(thread, NULL) != 0) {
        fprintf(stderr, "termination worker thread join failed\n");
        goto destroy_runtime;
    }

    if (call.call_succeeded) {
        fprintf(stderr, "component spin call unexpectedly succeeded\n");
        goto destroy_runtime;
    }
    if (!strstr(call.exception, "terminated by user")) {
        fprintf(stderr, "component spin call exception: %s\n",
                call.exception[0] ? call.exception : "no exception");
        goto destroy_runtime;
    }

    puts("component cross-thread termination smoke passed");
    exit_code = 0;

destroy_runtime:
    wasm_runtime_destroy();
    return exit_code;
}
