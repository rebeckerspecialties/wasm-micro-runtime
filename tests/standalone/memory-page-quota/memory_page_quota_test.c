/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "memory page quota check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition);                         \
            abort();                                                         \
        }                                                                    \
    } while (0)

typedef struct PageQuotaState {
    pthread_mutex_t lock;
    uint32_t limit;
    uint32_t committed;
    uint32_t peak;
    uint32_t reserve_calls;
    uint32_t release_calls;
    uint32_t denied_calls;
} PageQuotaState;

typedef struct LoadedModule {
    uint8_t bytes[64];
    wasm_module_t module;
} LoadedModule;

typedef struct ParallelGate {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint32_t arrivals;
} ParallelGate;

typedef struct ParallelInstantiateArgs {
    LoadedModule *loaded;
    PageQuotaState *quota;
    ParallelGate *gate;
    bool instantiated;
} ParallelInstantiateArgs;

static const uint8_t oversized_memory_module[] = { 0x00, 0x61, 0x73, 0x6d, 0x01,
                                                   0x00, 0x00, 0x00, 0x05, 0x04,
                                                   0x01, 0x01, 0x03, 0x03 };

static const uint8_t two_page_memory_module[] = { 0x00, 0x61, 0x73, 0x6d, 0x01,
                                                  0x00, 0x00, 0x00, 0x05, 0x04,
                                                  0x01, 0x01, 0x02, 0x02 };

static const uint8_t growing_memory_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x05, 0x04, 0x01, 0x01,
    0x01, 0x03, 0x07, 0x08, 0x01, 0x04, 0x67, 0x72, 0x6f, 0x77, 0x00, 0x00,
    0x0a, 0x08, 0x01, 0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b
};

static void
quota_init(PageQuotaState *quota, uint32_t limit)
{
    memset(quota, 0, sizeof(*quota));
    CHECK(pthread_mutex_init(&quota->lock, NULL) == 0);
    quota->limit = limit;
}

static void
quota_destroy(PageQuotaState *quota)
{
    CHECK(quota->committed == 0);
    CHECK(pthread_mutex_destroy(&quota->lock) == 0);
}

static void
parallel_gate_wait(ParallelGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->lock) == 0);
    gate->arrivals++;
    if (gate->arrivals == 2) {
        CHECK(pthread_cond_broadcast(&gate->condition) == 0);
    }
    else {
        while (gate->arrivals != 2)
            CHECK(pthread_cond_wait(&gate->condition, &gate->lock) == 0);
    }
    CHECK(pthread_mutex_unlock(&gate->lock) == 0);
}

static bool
reserve_pages(void *attachment, uint32_t pages)
{
    PageQuotaState *quota = attachment;
    bool allowed;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->reserve_calls++;
    allowed = quota->committed <= quota->limit
              && pages <= quota->limit - quota->committed;
    if (allowed) {
        quota->committed += pages;
        if (quota->committed > quota->peak)
            quota->peak = quota->committed;
    }
    else {
        quota->denied_calls++;
    }
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
    return allowed;
}

static void
release_pages(void *attachment, uint32_t pages)
{
    PageQuotaState *quota = attachment;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    CHECK(pages <= quota->committed);
    quota->committed -= pages;
    quota->release_calls++;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static void
load_module(LoadedModule *loaded, const uint8_t *bytes, uint32_t size)
{
    char error_buf[256] = { 0 };

    memset(loaded, 0, sizeof(*loaded));
    CHECK(size <= sizeof(loaded->bytes));
    memcpy(loaded->bytes, bytes, size);
    loaded->module = wasm_runtime_load(loaded->bytes, size, error_buf,
                                       (uint32_t)sizeof(error_buf));
    if (loaded->module == NULL)
        fprintf(stderr, "load failed: %s\n", error_buf);
    CHECK(loaded->module != NULL);
}

static wasm_module_inst_t
instantiate(LoadedModule *loaded, PageQuotaState *quota, uint32_t max_pages,
            char *error_buf, uint32_t error_buf_size)
{
    struct InstantiationArgs2 *args = NULL;
    wasm_module_inst_t instance;

    CHECK(wasm_runtime_instantiation_args_create(&args));
    CHECK(args != NULL);
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 0);
    wasm_runtime_instantiation_args_set_max_memory_pages(args, max_pages);
    wasm_runtime_instantiation_args_set_memory_page_quota(
        args, quota, reserve_pages, release_pages);
    instance = wasm_runtime_instantiate_ex2(loaded->module, args, error_buf,
                                            error_buf_size);
    wasm_runtime_instantiation_args_destroy(args);
    return instance;
}

static void *
parallel_instantiate(void *attachment)
{
    ParallelInstantiateArgs *thread_args = attachment;
    wasm_module_inst_t instance;
    char error_buf[256] = { 0 };

    instance = instantiate(thread_args->loaded, thread_args->quota, 2,
                           error_buf, (uint32_t)sizeof(error_buf));
    thread_args->instantiated = instance != NULL;
    if (!instance)
        CHECK(strstr(error_buf,
                     "aggregate memory page quota denied 2 initial pages")
              != NULL);
    parallel_gate_wait(thread_args->gate);
    if (instance)
        wasm_runtime_deinstantiate(instance);
    return NULL;
}

static void
test_partial_quota_callbacks_fail_before_reservation(void)
{
    LoadedModule loaded;
    PageQuotaState quota;
    struct InstantiationArgs2 *args = NULL;
    char error_buf[256] = { 0 };

    load_module(&loaded, two_page_memory_module,
                (uint32_t)sizeof(two_page_memory_module));
    quota_init(&quota, 4);
    CHECK(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_memory_page_quota(args, &quota,
                                                          reserve_pages, NULL);
    CHECK(wasm_runtime_instantiate_ex2(loaded.module, args, error_buf,
                                       (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "incomplete memory page quota callbacks") != NULL);
    CHECK(quota.reserve_calls == 0);
    CHECK(quota.committed == 0);
    wasm_runtime_instantiation_args_destroy(args);
    wasm_runtime_unload(loaded.module);
    quota_destroy(&quota);
}

static void
test_oversized_initial_memory_is_rejected_before_reservation(void)
{
    LoadedModule loaded;
    PageQuotaState quota;
    char error_buf[256] = { 0 };

    load_module(&loaded, oversized_memory_module,
                (uint32_t)sizeof(oversized_memory_module));
    quota_init(&quota, 8);
    CHECK(
        instantiate(&loaded, &quota, 2, error_buf, (uint32_t)sizeof(error_buf))
        == NULL);
    CHECK(strstr(error_buf,
                 "effective initial memory pages 3 exceed configured limit 2")
          != NULL);
    CHECK(quota.reserve_calls == 0);
    CHECK(quota.committed == 0);
    wasm_runtime_unload(loaded.module);
    quota_destroy(&quota);
}

static void
test_two_instances_share_one_aggregate_page_quota(void)
{
    LoadedModule first, second;
    PageQuotaState quota;
    char error_buf[256] = { 0 };
    wasm_module_inst_t first_instance, second_instance;

    load_module(&first, two_page_memory_module,
                (uint32_t)sizeof(two_page_memory_module));
    load_module(&second, two_page_memory_module,
                (uint32_t)sizeof(two_page_memory_module));
    quota_init(&quota, 3);
    first_instance =
        instantiate(&first, &quota, 4, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(first_instance != NULL);
    CHECK(quota.committed == 2);

    memset(error_buf, 0, sizeof(error_buf));
    CHECK(
        instantiate(&second, &quota, 4, error_buf, (uint32_t)sizeof(error_buf))
        == NULL);
    CHECK(
        strstr(error_buf, "aggregate memory page quota denied 2 initial pages")
        != NULL);
    CHECK(quota.denied_calls == 1);
    CHECK(quota.committed == 2);

    wasm_runtime_deinstantiate(first_instance);
    CHECK(quota.committed == 0);
    memset(error_buf, 0, sizeof(error_buf));
    second_instance =
        instantiate(&second, &quota, 4, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(second_instance != NULL);
    CHECK(quota.committed == 2);
    wasm_runtime_deinstantiate(second_instance);
    CHECK(quota.committed == 0);
    CHECK(quota.peak == 2);

    wasm_runtime_unload(first.module);
    wasm_runtime_unload(second.module);
    quota_destroy(&quota);
}

static void
test_parallel_instances_cannot_race_past_the_aggregate_limit(void)
{
    LoadedModule first, second;
    PageQuotaState quota;
    ParallelGate gate;
    ParallelInstantiateArgs first_args, second_args;
    pthread_t first_thread, second_thread;

    load_module(&first, two_page_memory_module,
                (uint32_t)sizeof(two_page_memory_module));
    load_module(&second, two_page_memory_module,
                (uint32_t)sizeof(two_page_memory_module));
    quota_init(&quota, 3);
    memset(&gate, 0, sizeof(gate));
    CHECK(pthread_mutex_init(&gate.lock, NULL) == 0);
    CHECK(pthread_cond_init(&gate.condition, NULL) == 0);
    first_args = (ParallelInstantiateArgs){ &first, &quota, &gate, false };
    second_args = (ParallelInstantiateArgs){ &second, &quota, &gate, false };

    CHECK(pthread_create(&first_thread, NULL, parallel_instantiate, &first_args)
          == 0);
    CHECK(
        pthread_create(&second_thread, NULL, parallel_instantiate, &second_args)
        == 0);
    CHECK(pthread_join(first_thread, NULL) == 0);
    CHECK(pthread_join(second_thread, NULL) == 0);

    CHECK(first_args.instantiated != second_args.instantiated);
    CHECK(quota.reserve_calls == 2);
    CHECK(quota.denied_calls == 1);
    CHECK(quota.peak == 2);
    CHECK(quota.committed == 0);
    CHECK(pthread_cond_destroy(&gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.lock) == 0);
    wasm_runtime_unload(first.module);
    wasm_runtime_unload(second.module);
    quota_destroy(&quota);
}

static void
test_growth_denial_refunds_on_deinstantiate_and_allows_reopen(void)
{
    LoadedModule loaded;
    PageQuotaState quota;
    char error_buf[256] = { 0 };
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    wasm_function_inst_t grow;
    uint32_t argv[1];

    load_module(&loaded, growing_memory_module,
                (uint32_t)sizeof(growing_memory_module));
    quota_init(&quota, 2);
    instance =
        instantiate(&loaded, &quota, 3, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(instance != NULL);
    CHECK(quota.committed == 1);
    exec_env = wasm_runtime_create_exec_env(instance, 64 * 1024);
    CHECK(exec_env != NULL);
    grow = wasm_runtime_lookup_function(instance, "grow");
    CHECK(grow != NULL);

    argv[0] = 1;
    CHECK(wasm_runtime_call_wasm(exec_env, grow, 1, argv));
    CHECK(argv[0] == 1);
    CHECK(quota.committed == 2);
    argv[0] = 1;
    CHECK(wasm_runtime_call_wasm(exec_env, grow, 1, argv));
    CHECK(argv[0] == UINT32_MAX);
    CHECK(quota.denied_calls == 1);
    CHECK(quota.committed == 2);

    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(instance);
    CHECK(quota.committed == 0);

    memset(error_buf, 0, sizeof(error_buf));
    instance =
        instantiate(&loaded, &quota, 3, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(instance != NULL);
    CHECK(quota.committed == 1);
    wasm_runtime_deinstantiate(instance);
    CHECK(quota.committed == 0);
    wasm_runtime_unload(loaded.module);
    quota_destroy(&quota);
}

int
main(void)
{
    CHECK(wasm_runtime_init());
    test_partial_quota_callbacks_fail_before_reservation();
    test_oversized_initial_memory_is_rejected_before_reservation();
    test_two_instances_share_one_aggregate_page_quota();
    test_parallel_instances_cannot_race_past_the_aggregate_limit();
    test_growth_denial_refunds_on_deinstantiate_and_allows_reopen();
    wasm_runtime_destroy();
    puts("memory page quota integration checks passed");
    return 0;
}
