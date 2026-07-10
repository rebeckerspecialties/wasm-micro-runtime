/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "wasm_export.h"

/*
 * (module
 *   (import "env" "bump" (func $bump (param i32) (result i32)))
 *   (func (export "run") (result i32)
 *     i32.const 41
 *     call $bump)
 *   (func (export "spin")
 *     (loop $again
 *       br $again)))
 */
static uint8_t wasm_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x03,
    0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x00,
    0x00, 0x02, 0x0c, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x04, 0x62, 0x75,
    0x6d, 0x70, 0x00, 0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x07, 0x0e,
    0x02, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x01, 0x04, 0x73, 0x70, 0x69,
    0x6e, 0x00, 0x02, 0x0a, 0x10, 0x02, 0x06, 0x00, 0x41, 0x29, 0x10,
    0x00, 0x0b, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b,
};

static int32_t increment = 1;

static int32_t
bump(wasm_exec_env_t exec_env, int32_t value)
{
    const int32_t *attachment =
        (const int32_t *)wasm_runtime_get_function_attachment(exec_env);
    return attachment ? value + *attachment : value;
}

typedef struct SpinCall {
    wasm_exec_env_t exec_env;
    wasm_function_inst_t function;
    bool thread_env_initialized;
    bool call_succeeded;
} SpinCall;

static void *
call_spin(void *arg)
{
    SpinCall *call = (SpinCall *)arg;
    uint32_t argv[1] = { 0 };

    call->thread_env_initialized = wasm_runtime_init_thread_env();
    if (!call->thread_env_initialized) {
        call->call_succeeded = false;
        return NULL;
    }
    call->call_succeeded =
        wasm_runtime_call_wasm(call->exec_env, call->function, 0, argv);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

int
main(void)
{
    RuntimeInitArgs init_args;
    char error_buf[256] = { 0 };
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_exec_env_t spin_exec_env = NULL;
    wasm_function_inst_t run = NULL;
    wasm_function_inst_t spin = NULL;
    uint32_t argv[1] = { 0 };
    pthread_t spin_thread;
    bool spin_thread_started = false;
    int exit_code = 1;

    static NativeSymbol native_symbols[] = {
        { "bump", bump, "(i)i", &increment },
    };

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;
    init_args.n_native_symbols =
        (uint32_t)(sizeof(native_symbols) / sizeof(native_symbols[0]));

    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        return 1;
    }

    module = wasm_runtime_load(wasm_bytes, (uint32_t)sizeof(wasm_bytes),
                               error_buf, (uint32_t)sizeof(error_buf));
    if (!module) {
        fprintf(stderr, "wasm_runtime_load failed: %s\n", error_buf);
        goto cleanup;
    }

    module_inst = wasm_runtime_instantiate(
        module, 64 * 1024, 64 * 1024, error_buf, (uint32_t)sizeof(error_buf));
    if (!module_inst) {
        fprintf(stderr, "wasm_runtime_instantiate failed: %s\n", error_buf);
        goto cleanup;
    }

    exec_env = wasm_runtime_create_exec_env(module_inst, 64 * 1024);
    if (!exec_env) {
        fprintf(stderr, "wasm_runtime_create_exec_env failed\n");
        goto cleanup;
    }

    run = wasm_runtime_lookup_function(module_inst, "run");
    if (!run) {
        fprintf(stderr, "run export was not found\n");
        goto cleanup;
    }

    if (!wasm_runtime_call_wasm(exec_env, run, 0, argv)) {
        fprintf(stderr, "run failed: %s\n",
                wasm_runtime_get_exception(module_inst));
        goto cleanup;
    }

    if (argv[0] != 42) {
        fprintf(stderr, "run returned %u, expected 42\n", argv[0]);
        goto cleanup;
    }

    spin = wasm_runtime_lookup_function(module_inst, "spin");
    if (!spin) {
        fprintf(stderr, "spin export was not found\n");
        goto cleanup;
    }

    spin_exec_env = wasm_runtime_create_exec_env(module_inst, 64 * 1024);
    if (!spin_exec_env) {
        fprintf(stderr, "spin execution environment creation failed\n");
        goto cleanup;
    }

    SpinCall spin_call = {
        .exec_env = spin_exec_env,
        .function = spin,
        .thread_env_initialized = false,
        .call_succeeded = true,
    };
    if (pthread_create(&spin_thread, NULL, call_spin, &spin_call) != 0) {
        fprintf(stderr, "spin thread creation failed\n");
        goto cleanup;
    }
    spin_thread_started = true;

    const struct timespec settle_time = {
        .tv_sec = 0,
        .tv_nsec = 20 * 1000 * 1000,
    };
    nanosleep(&settle_time, NULL);
    wasm_runtime_terminate(module_inst);
    pthread_join(spin_thread, NULL);
    spin_thread_started = false;

    const char *exception = wasm_runtime_get_exception(module_inst);
    if (!spin_call.thread_env_initialized || spin_call.call_succeeded
        || !exception || !strstr(exception, "terminated by user")) {
        fprintf(stderr, "spin was not terminated cleanly: %s\n",
                exception ? exception : "no exception");
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    if (spin_thread_started) {
        wasm_runtime_terminate(module_inst);
        pthread_join(spin_thread, NULL);
    }
    if (spin_exec_env)
        wasm_runtime_destroy_exec_env(spin_exec_env);
    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    if (module_inst)
        wasm_runtime_deinstantiate(module_inst);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
    return exit_code;
}
