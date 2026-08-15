/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "wasm_allocation_quota.h"
#include "wasm.h"
#include "wasm_c_api.h"
#include "wasm_c_api_internal.h"
#include "wasm_exec_env.h"
#include "wasm_export.h"
#include "wasm_runtime.h"
#include "wasm_runtime_common.h"
#include "thread_manager.h"

#define CHECK(condition)                                          \
    do {                                                          \
        if (!(condition)) {                                       \
            fprintf(stderr,                                       \
                    "allocation quota execution check failed at " \
                    "%s:%d: %s\n",                                \
                    __FILE__, __LINE__, #condition);              \
            abort();                                              \
        }                                                         \
    } while (0)

typedef struct QuotaState {
    pthread_mutex_t lock;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t released_bytes;
    uint64_t byte_limit;
    uint32_t live_allocations;
    uint32_t reserved_allocations;
    uint32_t released_allocations;
    uint32_t allocation_limit;
    uint32_t denied_calls;
    uint32_t forced_denials;
    uint64_t reserve_calls;
    uint64_t deny_reserve_call;
    uint32_t attachment_retain_calls;
    uint32_t attachment_release_calls;
} QuotaState;

typedef struct QuotaSnapshot {
    uint64_t bytes;
    uint32_t allocations;
    uint32_t denied_calls;
} QuotaSnapshot;

typedef struct AllocatorState {
    pthread_mutex_t lock;
    uint64_t live_allocations;
} AllocatorState;

typedef struct RuntimeModeState {
    uint8_t *pool;
    AllocatorState allocator;
    bool allocator_inited;
} RuntimeModeState;

typedef enum ExternrefCleanupPath {
    EXTERNREF_CLEANUP_OBJDEL,
    EXTERNREF_CLEANUP_RECLAIM,
    EXTERNREF_CLEANUP_MODULE,
} ExternrefCleanupPath;

typedef struct ExternrefCleanupProbe {
    wasm_module_inst_t instance;
    QuotaState *quota;
    uint32_t initial_ref;
    uint32_t replacement_ref;
    uint32_t cleanup_calls;
    uint32_t replacement_cleanup_calls;
    bool saw_owner;
    bool replacement_obj2ref_succeeded;
    bool replacement_set_cleanup_succeeded;
    bool replacement_objdel_succeeded;
    bool initial_ref_was_detached;
    bool replacement_ref_was_deleted;
    bool retain_replacement;
    bool replacement_registration_rejected;
} ExternrefCleanupProbe;

/*
 * (module
 *   (memory (export "memory") 4 4)
 *   (global $__stack_pointer (mut i32) (i32.const 262144))
 *   (global $__data_end i32 (i32.const 1024))
 *   (global $__heap_base i32 (i32.const 262144))
 *   (export "__stack_pointer" (global $__stack_pointer))
 *   (export "__data_end" (global $__data_end))
 *   (export "__heap_base" (global $__heap_base))
 *   (table 1 funcref)
 *   (elem (i32.const 0) $answer)
 *   (func $answer (result i32) i32.const 42)
 *   (func (export "many") (param i32 x 20) (result i32) local.get 19)
 *   (func (export "answer") (result i32) call $answer))
 */
static const uint8_t execution_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1d, 0x02, 0x60,
    0x00, 0x01, 0x7f, 0x60, 0x14, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x01, 0x7f, 0x03, 0x04, 0x03, 0x00, 0x01, 0x00, 0x04, 0x04, 0x01,
    0x70, 0x00, 0x01, 0x05, 0x04, 0x01, 0x01, 0x04, 0x04, 0x06, 0x15, 0x03,
    0x7f, 0x01, 0x41, 0x80, 0x80, 0x10, 0x0b, 0x7f, 0x00, 0x41, 0x80, 0x08,
    0x0b, 0x7f, 0x00, 0x41, 0x80, 0x80, 0x10, 0x0b, 0x07, 0x47, 0x06, 0x06,
    0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0f, 0x5f, 0x5f, 0x73,
    0x74, 0x61, 0x63, 0x6b, 0x5f, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x65, 0x72,
    0x03, 0x00, 0x0a, 0x5f, 0x5f, 0x64, 0x61, 0x74, 0x61, 0x5f, 0x65, 0x6e,
    0x64, 0x03, 0x01, 0x0b, 0x5f, 0x5f, 0x68, 0x65, 0x61, 0x70, 0x5f, 0x62,
    0x61, 0x73, 0x65, 0x03, 0x02, 0x04, 0x6d, 0x61, 0x6e, 0x79, 0x00, 0x01,
    0x06, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x00, 0x02, 0x09, 0x07, 0x01,
    0x00, 0x41, 0x00, 0x0b, 0x01, 0x00, 0x0a, 0x10, 0x03, 0x04, 0x00, 0x41,
    0x2a, 0x0b, 0x04, 0x00, 0x20, 0x13, 0x0b, 0x04, 0x00, 0x10, 0x00, 0x0b
};

/*
 * (module
 *   (func (export "many") (param i32 x 20) (result i32) local.get 19)
 *   (func (export "boom") unreachable))
 *
 * `many` exceeds wasm_func_call's inline argv capacity.  `boom` provides a
 * stable call stack whose trap/frame copies can outlive later guest calls.
 */
static const uint8_t c_api_call_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1c, 0x02, 0x60,
    0x14, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60,
    0x00, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x0f, 0x02, 0x04, 0x6d,
    0x61, 0x6e, 0x79, 0x00, 0x00, 0x04, 0x62, 0x6f, 0x6f, 0x6d, 0x00, 0x01,
    0x0a, 0x0a, 0x02, 0x04, 0x00, 0x20, 0x13, 0x0b, 0x03, 0x00, 0x00, 0x0b
};

/*
 * (module
 *   (type $callback-type (func))
 *   (import "host" "callback-a" (func $callback-a (type $callback-type)))
 *   (import "host" "callback-b" (func $callback-b (type $callback-type)))
 *   (func (export "invoke") call $callback-a call $callback-b))
 */
static const uint8_t c_api_import_binding_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60,
    0x00, 0x00, 0x02, 0x25, 0x02, 0x04, 0x68, 0x6f, 0x73, 0x74, 0x0a, 0x63,
    0x61, 0x6c, 0x6c, 0x62, 0x61, 0x63, 0x6b, 0x2d, 0x61, 0x00, 0x00, 0x04,
    0x68, 0x6f, 0x73, 0x74, 0x0a, 0x63, 0x61, 0x6c, 0x6c, 0x62, 0x61, 0x63,
    0x6b, 0x2d, 0x62, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x07, 0x0a, 0x01,
    0x06, 0x69, 0x6e, 0x76, 0x6f, 0x6b, 0x65, 0x00, 0x02, 0x0a, 0x08, 0x01,
    0x06, 0x00, 0x10, 0x00, 0x10, 0x01, 0x0b
};

/* (module (table 1048576 funcref)) */
static const uint8_t huge_table_module[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00,
                                             0x00, 0x00, 0x04, 0x06, 0x01, 0x70,
                                             0x00, 0x80, 0x80, 0x40 };

static void
quota_init(QuotaState *quota)
{
    memset(quota, 0, sizeof(*quota));
    CHECK(pthread_mutex_init(&quota->lock, NULL) == 0);
    quota->byte_limit = UINT64_MAX;
    quota->allocation_limit = UINT32_MAX;
}

static QuotaSnapshot
quota_snapshot(QuotaState *quota)
{
    QuotaSnapshot snapshot;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    snapshot.bytes = quota->live_bytes;
    snapshot.allocations = quota->live_allocations;
    snapshot.denied_calls = quota->denied_calls;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
    return snapshot;
}

static void
quota_set_limits(QuotaState *quota, uint64_t bytes, uint32_t allocations)
{
    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->byte_limit = bytes;
    quota->allocation_limit = allocations;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static void
quota_force_next_denials(QuotaState *quota, uint32_t denials)
{
    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->forced_denials = denials;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static uint64_t
quota_reserve_call_count(QuotaState *quota)
{
    uint64_t reserve_calls;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    reserve_calls = quota->reserve_calls;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
    return reserve_calls;
}

static void
quota_deny_reserve_call(QuotaState *quota, uint64_t reserve_call)
{
    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    CHECK(reserve_call > quota->reserve_calls);
    CHECK(quota->deny_reserve_call == 0);
    quota->deny_reserve_call = reserve_call;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static void
quota_check_live(QuotaState *quota, QuotaSnapshot expected)
{
    QuotaSnapshot actual = quota_snapshot(quota);

    CHECK(actual.bytes == expected.bytes);
    CHECK(actual.allocations == expected.allocations);
}

static void
quota_destroy(QuotaState *quota)
{
    QuotaSnapshot final = quota_snapshot(quota);

    CHECK(final.bytes == 0);
    CHECK(final.allocations == 0);
    CHECK(quota->reserved_bytes == quota->released_bytes);
    CHECK(quota->reserved_allocations == quota->released_allocations);
    CHECK(quota->forced_denials == 0);
    CHECK(quota->deny_reserve_call == 0);
    CHECK(quota->attachment_retain_calls == quota->attachment_release_calls);
    CHECK(pthread_mutex_destroy(&quota->lock) == 0);
}

static void
externref_replacement_cleanup(void *extern_obj)
{
    ExternrefCleanupProbe *probe = extern_obj;

    CHECK(probe != NULL);
    CHECK(wasm_allocation_quota_get_current() != NULL);
    probe->replacement_cleanup_calls++;
}

static void
externref_reentrant_cleanup(void *extern_obj)
{
    ExternrefCleanupProbe *probe = extern_obj;
    QuotaSnapshot callback_entry;
    QuotaSnapshot replacement_live;
    QuotaSnapshot replacement_deleted;
    void *resolved = NULL;

    CHECK(probe != NULL);
    CHECK(probe->instance != NULL);
    CHECK(probe->quota != NULL);
    probe->cleanup_calls++;
    probe->saw_owner = wasm_allocation_quota_get_current() != NULL;
    CHECK(probe->saw_owner);

    callback_entry = quota_snapshot(probe->quota);
    probe->replacement_obj2ref_succeeded =
        wasm_externref_obj2ref(probe->instance, probe, &probe->replacement_ref);
    if (!probe->replacement_obj2ref_succeeded) {
        CHECK(probe->retain_replacement);
        probe->replacement_registration_rejected = true;
        CHECK(!wasm_externref_set_cleanup(probe->instance, probe,
                                          externref_replacement_cleanup));
        CHECK(!wasm_externref_objdel(probe->instance, probe));
        probe->initial_ref_was_detached =
            !wasm_externref_ref2obj(probe->initial_ref, &resolved);
        CHECK(probe->initial_ref_was_detached);
        CHECK(probe->replacement_ref == NULL_REF);
        probe->replacement_ref_was_deleted = true;
        quota_check_live(probe->quota, callback_entry);
        return;
    }
    CHECK(probe->replacement_ref != probe->initial_ref);
    replacement_live = quota_snapshot(probe->quota);
    CHECK(replacement_live.bytes > callback_entry.bytes);
    CHECK(replacement_live.allocations > callback_entry.allocations);

    probe->replacement_set_cleanup_succeeded = wasm_externref_set_cleanup(
        probe->instance, probe, externref_replacement_cleanup);
    CHECK(probe->replacement_set_cleanup_succeeded);
    if (probe->retain_replacement) {
        probe->initial_ref_was_detached =
            !wasm_externref_ref2obj(probe->initial_ref, &resolved);
        CHECK(probe->initial_ref_was_detached);
        return;
    }
    probe->replacement_objdel_succeeded =
        wasm_externref_objdel(probe->instance, probe);
    CHECK(probe->replacement_objdel_succeeded);
    replacement_deleted = quota_snapshot(probe->quota);
    CHECK(replacement_deleted.bytes == callback_entry.bytes);
    CHECK(replacement_deleted.allocations == callback_entry.allocations);

    resolved = NULL;
    probe->initial_ref_was_detached =
        !wasm_externref_ref2obj(probe->initial_ref, &resolved);
    CHECK(probe->initial_ref_was_detached);
    resolved = NULL;
    probe->replacement_ref_was_deleted =
        !wasm_externref_ref2obj(probe->replacement_ref, &resolved);
    CHECK(probe->replacement_ref_was_deleted);
}

static bool
quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;
    bool allowed;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->reserve_calls++;
    if (quota->deny_reserve_call == quota->reserve_calls) {
        quota->deny_reserve_call = 0;
        allowed = false;
    }
    else if (quota->forced_denials > 0) {
        quota->forced_denials--;
        allowed = false;
    }
    else {
        allowed =
            quota->live_bytes <= quota->byte_limit
            && bytes <= quota->byte_limit - quota->live_bytes
            && quota->live_allocations <= quota->allocation_limit
            && allocations <= quota->allocation_limit - quota->live_allocations;
    }
    if (allowed) {
        quota->live_bytes += bytes;
        quota->live_allocations += allocations;
        quota->reserved_bytes += bytes;
        quota->reserved_allocations += allocations;
    }
    else {
        quota->denied_calls++;
    }
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
    return allowed;
}

static uint32_t c_api_import_callback_calls;

typedef struct CApiCallbackEnv {
    uint32_t magic;
    uint32_t *callback_calls;
    uint32_t *finalizer_calls;
} CApiCallbackEnv;

#define C_API_CALLBACK_ENV_MAGIC UINT32_C(0x43415049)

static wasm_trap_t *
c_api_import_callback(const wasm_val_vec_t *args, wasm_val_vec_t *results)
{
    CHECK(args != NULL);
    CHECK(results != NULL);
    CHECK(args->num_elems == 0);
    c_api_import_callback_calls++;
    return NULL;
}

static wasm_trap_t *
c_api_import_callback_with_env(void *opaque_env, const wasm_val_vec_t *args,
                               wasm_val_vec_t *results)
{
    CApiCallbackEnv *env = opaque_env;

    CHECK(env != NULL);
    CHECK(env->magic == C_API_CALLBACK_ENV_MAGIC);
    CHECK(env->callback_calls != NULL);
    CHECK(args != NULL);
    CHECK(results != NULL);
    CHECK(args->num_elems == 0);
    (*env->callback_calls)++;
    return NULL;
}

static void
c_api_callback_env_finalizer(void *opaque_env)
{
    CApiCallbackEnv *env = opaque_env;

    CHECK(env != NULL);
    CHECK(env->magic == C_API_CALLBACK_ENV_MAGIC);
    CHECK(env->finalizer_calls != NULL);
    (*env->finalizer_calls)++;
    env->magic = 0;
    free(env);
}

static CApiCallbackEnv *
c_api_callback_env_new(uint32_t *callback_calls, uint32_t *finalizer_calls)
{
    CApiCallbackEnv *env = malloc(sizeof(*env));

    CHECK(env != NULL);
    env->magic = C_API_CALLBACK_ENV_MAGIC;
    env->callback_calls = callback_calls;
    env->finalizer_calls = finalizer_calls;
    return env;
}

static void
quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    CHECK(bytes <= quota->live_bytes);
    CHECK(allocations <= quota->live_allocations);
    quota->live_bytes -= bytes;
    quota->live_allocations -= allocations;
    quota->released_bytes += bytes;
    quota->released_allocations += allocations;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static bool
quota_attachment_retain(void *attachment)
{
    QuotaState *quota = attachment;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->attachment_retain_calls++;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
    return true;
}

static void
quota_attachment_release(void *attachment)
{
    QuotaState *quota = attachment;

    CHECK(pthread_mutex_lock(&quota->lock) == 0);
    quota->attachment_release_calls++;
    CHECK(pthread_mutex_unlock(&quota->lock) == 0);
}

static LoadArgs2
quota_load_args(QuotaState *quota, const char *name)
{
    LoadArgs2 args;

    wasm_runtime_load_args2_init(&args);
    args.name = (char *)name;
    args.clone_wasm_binary = true;
    wasm_runtime_load_args2_set_allocation_quota(
        &args, quota, quota_reserve, quota_release, quota_attachment_retain,
        quota_attachment_release);
    return args;
}

static wasm_module_t
load_owned_module(const uint8_t *source, uint32_t size, QuotaState *quota,
                  const char *name)
{
    char error_buf[256] = { 0 };
    uint8_t *copy = malloc(size);
    LoadArgs2 args = quota_load_args(quota, name);
    wasm_module_t module;

    CHECK(copy != NULL);
    memcpy(copy, source, size);
    module = wasm_runtime_load_ex2(copy, size, &args, error_buf,
                                   (uint32_t)sizeof(error_buf));
    free(copy);
    if (!module)
        fprintf(stderr, "quota module load failed: %s\n", error_buf);
    return module;
}

static void *
custom_malloc(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    unsigned int size)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    extern RuntimeModeState *active_mode_state;
    AllocatorState *state = &active_mode_state->allocator;
#endif
    void *ptr = malloc(size);

    if (ptr) {
        CHECK(pthread_mutex_lock(&state->lock) == 0);
        state->live_allocations++;
        CHECK(pthread_mutex_unlock(&state->lock) == 0);
    }
    return ptr;
}

static void *
custom_realloc(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    void *ptr, unsigned int size)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    extern RuntimeModeState *active_mode_state;
    AllocatorState *state = &active_mode_state->allocator;
#endif
    void *new_ptr = realloc(ptr, size);

    (void)state;
    return new_ptr;
}

static void
custom_free(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    void *ptr)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    extern RuntimeModeState *active_mode_state;
    AllocatorState *state = &active_mode_state->allocator;
#endif

    if (ptr) {
        CHECK(pthread_mutex_lock(&state->lock) == 0);
        CHECK(state->live_allocations > 0);
        state->live_allocations--;
        CHECK(pthread_mutex_unlock(&state->lock) == 0);
    }
    free(ptr);
}

#if WASM_MEM_ALLOC_WITH_USER_DATA == 0
RuntimeModeState *active_mode_state;
#endif

static void
runtime_mode_init(mem_alloc_type_t mode, RuntimeModeState *state)
{
    RuntimeInitArgs init_args;

    memset(state, 0, sizeof(*state));
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = mode;
    init_args.max_thread_num = 4;

    if (mode == Alloc_With_Pool) {
        state->pool = malloc(32 * 1024 * 1024);
        CHECK(state->pool != NULL);
        init_args.mem_alloc_option.pool.heap_buf = state->pool;
        init_args.mem_alloc_option.pool.heap_size = 32 * 1024 * 1024;
    }
    else if (mode == Alloc_With_Allocator) {
        CHECK(pthread_mutex_init(&state->allocator.lock, NULL) == 0);
        state->allocator_inited = true;
        init_args.mem_alloc_option.allocator.malloc_func =
            (void *)custom_malloc;
        init_args.mem_alloc_option.allocator.realloc_func =
            (void *)custom_realloc;
        init_args.mem_alloc_option.allocator.free_func = (void *)custom_free;
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
        init_args.mem_alloc_option.allocator.user_data = &state->allocator;
#else
        active_mode_state = state;
#endif
    }

    CHECK(wasm_runtime_full_init(&init_args));
}

static void
runtime_mode_destroy(mem_alloc_type_t mode, RuntimeModeState *state)
{
    wasm_runtime_destroy();
    if (mode == Alloc_With_Allocator) {
        CHECK(state->allocator.live_allocations == 0);
        CHECK(pthread_mutex_destroy(&state->allocator.lock) == 0);
#if WASM_MEM_ALLOC_WITH_USER_DATA == 0
        active_mode_state = NULL;
#endif
    }
    free(state->pool);
}

static void
call_answer(wasm_exec_env_t exec_env, wasm_module_inst_t instance)
{
    wasm_function_inst_t function =
        wasm_runtime_lookup_function(instance, "answer");
    uint32_t argv[1] = { 0 };

    CHECK(function != NULL);
    CHECK(wasm_runtime_call_wasm(exec_env, function, 0, argv));
    CHECK(argv[0] == 42);
    argv[0] = 0;
    CHECK(wasm_runtime_call_indirect(exec_env, 0, 0, argv));
    CHECK(argv[0] == 42);
}

static void
test_instantiation_exec_env_calls_and_refunds(void)
{
    char error_buf[256] = { 0 };
    QuotaState quota;
    QuotaState wrong_quota;
    QuotaSnapshot loaded;
    QuotaSnapshot instantiated;
    QuotaSnapshot exec_ready;
    LoadArgs2 wrong_args;
    WASMAllocationQuotaToken *wrong_token;
    WASMAllocationQuotaScope allocation_scope;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    wasm_function_inst_t many;
    wasm_val_t args[20] = { 0 };
    wasm_val_t result = { 0 };
    uint8_t *scratch;
    uint8_t *grown;
    uint64_t native_thread_charge =
        wasm_exec_env_native_thread_stack_reservation_size(
            APP_THREAD_STACK_SIZE_DEFAULT);
    uint32_t native_thread_allocations =
        wasm_exec_env_native_thread_stack_reservation_count();
    uint32_t i;

    quota_init(&quota);
    module =
        load_owned_module(execution_module, (uint32_t)sizeof(execution_module),
                          &quota, "execution-main");
    CHECK(module != NULL);
    loaded = quota_snapshot(&quota);

    quota_set_limits(&quota, loaded.bytes, loaded.allocations);
    instance = wasm_runtime_instantiate(module, 8192, 0, error_buf,
                                        (uint32_t)sizeof(error_buf));
    CHECK(instance == NULL);
    CHECK(quota_snapshot(&quota).denied_calls > loaded.denied_calls);
    quota_check_live(&quota, loaded);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);

    memset(error_buf, 0, sizeof(error_buf));
    instance = wasm_runtime_instantiate(module, 8192, 0, error_buf,
                                        (uint32_t)sizeof(error_buf));
    if (!instance)
        fprintf(stderr, "quota instantiate failed: %s\n", error_buf);
    CHECK(instance != NULL);
    CHECK(wasm_allocation_quota_allocations_share_owner(module, instance));
    instantiated = quota_snapshot(&quota);
    CHECK(instantiated.bytes > loaded.bytes);
    CHECK(instantiated.allocations > loaded.allocations);

    quota_set_limits(&quota, instantiated.bytes, instantiated.allocations);
    exec_env = wasm_runtime_create_exec_env(instance, 16384);
    CHECK(exec_env == NULL);
    CHECK(quota_snapshot(&quota).denied_calls > instantiated.denied_calls);
    quota_check_live(&quota, instantiated);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);

    exec_env = wasm_runtime_create_exec_env(instance, 16384);
    CHECK(exec_env != NULL);
    CHECK(wasm_allocation_quota_allocations_share_owner(instance, exec_env));
    exec_ready = quota_snapshot(&quota);
    CHECK(exec_ready.bytes > instantiated.bytes);
    CHECK(exec_ready.allocations > instantiated.allocations);

    call_answer(exec_env, instance);
    many = wasm_runtime_lookup_function(instance, "many");
    CHECK(many != NULL);
    for (i = 0; i < 20; i++) {
        args[i].kind = WASM_I32;
        args[i].of.i32 = (int32_t)i;
    }

    quota_set_limits(&quota, exec_ready.bytes, exec_ready.allocations);
    CHECK(!wasm_runtime_call_wasm_a(exec_env, many, 1, &result, 20, args));
    CHECK(quota_snapshot(&quota).denied_calls > exec_ready.denied_calls);
    quota_check_live(&quota, exec_ready);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);
    wasm_runtime_clear_exception(instance);

    CHECK(wasm_runtime_call_wasm_a(exec_env, many, 1, &result, 20, args));
    CHECK(result.kind == WASM_I32);
    CHECK(result.of.i32 == 19);
    memset(&result, 0, sizeof(result));
    CHECK(wasm_runtime_call_wasm_v(
        exec_env, many, 1, &result, 20, (uint32_t)0, (uint32_t)1, (uint32_t)2,
        (uint32_t)3, (uint32_t)4, (uint32_t)5, (uint32_t)6, (uint32_t)7,
        (uint32_t)8, (uint32_t)9, (uint32_t)10, (uint32_t)11, (uint32_t)12,
        (uint32_t)13, (uint32_t)14, (uint32_t)15, (uint32_t)16, (uint32_t)17,
        (uint32_t)18, (uint32_t)19));
    CHECK(result.kind == WASM_I32);
    CHECK(result.of.i32 == 19);
    quota_check_live(&quota, exec_ready);

    quota_set_limits(&quota, exec_ready.bytes + native_thread_charge - 1,
                     UINT32_MAX);
    CHECK(!wasm_exec_env_reserve_native_thread_stack(
        exec_env, APP_THREAD_STACK_SIZE_DEFAULT));
    quota_check_live(&quota, exec_ready);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);
    CHECK(wasm_exec_env_reserve_native_thread_stack(
        exec_env, APP_THREAD_STACK_SIZE_DEFAULT));
    CHECK(quota_snapshot(&quota).bytes
          == exec_ready.bytes + native_thread_charge);
    CHECK(quota_snapshot(&quota).allocations
          == exec_ready.allocations + native_thread_allocations);
    wasm_exec_env_release_native_thread_stack(exec_env);
    quota_check_live(&quota, exec_ready);

    CHECK(wasm_allocation_quota_scope_enter_for_allocation(&allocation_scope,
                                                           instance));
    scratch = wasm_runtime_malloc(32);
    CHECK(scratch != NULL);
    wasm_allocation_quota_scope_leave(&allocation_scope);
    quota_set_limits(&quota, quota_snapshot(&quota).bytes, UINT32_MAX);
    CHECK(wasm_runtime_realloc(scratch, 4096) == NULL);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);
    grown = wasm_runtime_realloc(scratch, 4096);
    CHECK(grown != NULL);
    scratch = wasm_runtime_realloc(grown, 8);
    CHECK(scratch != NULL);
    wasm_runtime_free(scratch);
    quota_check_live(&quota, exec_ready);

    quota_init(&wrong_quota);
    wrong_args = quota_load_args(&wrong_quota, "wrong-ambient");
    wrong_token = wasm_allocation_quota_token_create(&wrong_args);
    CHECK(wrong_token != NULL);
    CHECK(wasm_allocation_quota_set_current(wrong_token) == NULL);
    CHECK(!wasm_runtime_call_wasm_a(exec_env, many, 1, &result, 20, args));
    CHECK(wasm_runtime_create_exec_env(instance, 8192) == NULL);
    wasm_runtime_deinstantiate(instance);
    CHECK(wasm_allocation_quota_set_current(NULL) == wrong_token);
    wasm_allocation_quota_token_release(wrong_token);
    quota_destroy(&wrong_quota);
    call_answer(exec_env, instance);

    wasm_runtime_destroy_exec_env(exec_env);
    quota_check_live(&quota, instantiated);
    wasm_runtime_deinstantiate(instance);
    quota_check_live(&quota, loaded);
    wasm_runtime_unload(module);
    quota_destroy(&quota);
}

static void
test_externref_reentrant_cleanup_and_refunds(ExternrefCleanupPath path)
{
    static const char *const module_names[] = {
        "externref-objdel",
        "externref-reclaim",
        "externref-module-cleanup",
    };
    char error_buf[256] = { 0 };
    QuotaState quota;
    QuotaSnapshot loaded;
    QuotaSnapshot instantiated;
    QuotaSnapshot registered;
    ExternrefCleanupProbe probe = { 0 };
    wasm_module_t module;
    wasm_module_inst_t instance;

    CHECK((uint32_t)path
          < (uint32_t)(sizeof(module_names) / sizeof(module_names[0])));
    quota_init(&quota);
    module =
        load_owned_module(execution_module, (uint32_t)sizeof(execution_module),
                          &quota, module_names[path]);
    CHECK(module != NULL);
    loaded = quota_snapshot(&quota);
    instance = wasm_runtime_instantiate(module, 8192, 0, error_buf,
                                        (uint32_t)sizeof(error_buf));
    if (!instance)
        fprintf(stderr, "externref instantiate failed: %s\n", error_buf);
    CHECK(instance != NULL);
    instantiated = quota_snapshot(&quota);

    probe.instance = instance;
    probe.quota = &quota;
    probe.replacement_ref = NULL_REF;
    probe.retain_replacement = path == EXTERNREF_CLEANUP_MODULE;
    CHECK(wasm_externref_obj2ref(instance, &probe, &probe.initial_ref));
    CHECK(wasm_externref_set_cleanup(instance, &probe,
                                     externref_reentrant_cleanup));
    registered = quota_snapshot(&quota);
    CHECK(registered.bytes > instantiated.bytes);
    CHECK(registered.allocations > instantiated.allocations);

    switch (path) {
        case EXTERNREF_CLEANUP_OBJDEL:
            CHECK(wasm_externref_objdel(instance, &probe));
            quota_check_live(&quota, instantiated);
            break;
        case EXTERNREF_CLEANUP_RECLAIM:
            wasm_externref_reclaim((WASMModuleInstanceCommon *)instance);
            quota_check_live(&quota, instantiated);
            break;
        case EXTERNREF_CLEANUP_MODULE:
            wasm_runtime_deinstantiate(instance);
            instance = NULL;
            quota_check_live(&quota, loaded);
            break;
        default:
            CHECK(false);
    }

    CHECK(probe.cleanup_calls == 1);
    CHECK(probe.saw_owner);
    CHECK(probe.initial_ref_was_detached);
    CHECK(probe.replacement_ref_was_deleted);
    if (path == EXTERNREF_CLEANUP_MODULE) {
        CHECK(probe.replacement_cleanup_calls == 0);
        CHECK(!probe.replacement_obj2ref_succeeded);
        CHECK(probe.replacement_registration_rejected);
        CHECK(!probe.replacement_set_cleanup_succeeded);
        CHECK(!probe.replacement_objdel_succeeded);
        CHECK(!wasm_externref_objdel(NULL, &probe));
    }
    else {
        CHECK(probe.replacement_cleanup_calls == 1);
        CHECK(probe.replacement_obj2ref_succeeded);
        CHECK(probe.replacement_set_cleanup_succeeded);
        CHECK(probe.replacement_objdel_succeeded);
        CHECK(!probe.replacement_registration_rejected);
    }

    if (instance) {
        wasm_runtime_deinstantiate(instance);
        quota_check_live(&quota, loaded);
    }
    wasm_runtime_unload(module);
    quota_destroy(&quota);
}

static void
test_huge_table_instantiation_denial_refunds(void)
{
    char error_buf[256] = { 0 };
    QuotaState quota;
    QuotaSnapshot loaded;
    wasm_module_t module;
    wasm_module_inst_t instance;

    quota_init(&quota);
    module = load_owned_module(huge_table_module,
                               (uint32_t)sizeof(huge_table_module), &quota,
                               "huge-table");
    CHECK(module != NULL);
    loaded = quota_snapshot(&quota);
    quota_set_limits(&quota, loaded.bytes + 64 * 1024, UINT32_MAX);
    instance = wasm_runtime_instantiate(module, 8192, 0, error_buf,
                                        (uint32_t)sizeof(error_buf));
    CHECK(instance == NULL);
    CHECK(quota_snapshot(&quota).denied_calls > loaded.denied_calls);
    quota_check_live(&quota, loaded);
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);
    wasm_runtime_unload(module);
    quota_destroy(&quota);
}

static void
test_c_api_call_argv_denial_and_trap_lifetime(wasm_engine_t *engine)
{
    QuotaState quota;
    QuotaSnapshot baseline;
    QuotaSnapshot after_call;
    LoadArgs2 args;
    wasm_store_t *store;
    wasm_byte_vec_t binary = { 0 };
    wasm_module_t *module;
    wasm_instance_t *instance;
    wasm_extern_vec_t no_imports = WASM_EMPTY_VEC;
    wasm_extern_vec_t exports = WASM_EMPTY_VEC;
    wasm_func_t *many;
    wasm_func_t *boom;
    wasm_val_t param_values[20];
    wasm_val_vec_t params = WASM_ARRAY_VEC(param_values);
    wasm_val_t result_value = WASM_I32_VAL(INT32_MIN);
    wasm_val_vec_t results = { 1, &result_value, 0, sizeof(result_value),
                               NULL };
    wasm_val_vec_t no_results = WASM_EMPTY_VEC;
    wasm_trap_t *trap = NULL;
    wasm_trap_t *instantiate_trap = NULL;
    wasm_message_t message = { 0 };
    wasm_frame_vec_t trace = WASM_EMPTY_VEC;
    uint32_t i;
#if WASM_ENABLE_DUMP_CALL_STACK != 0
    wasm_frame_t *origin = NULL;
    QuotaSnapshot runtime_frames_live;
    uint32_t origin_func_index;
    size_t origin_module_offset;
    size_t origin_func_offset;
#endif

    CHECK(engine != NULL);
    store = wasm_store_new(engine);
    CHECK(store != NULL);
    wasm_byte_vec_new(&binary, sizeof(c_api_call_module),
                      (const byte_t *)c_api_call_module);
    CHECK(binary.data != NULL);

    quota_init(&quota);
    args = quota_load_args(&quota, "c-api-call");
    args.clone_wasm_binary = true;
    module = wasm_module_new_ex2(store, &binary, &args);
    CHECK(module != NULL);
    instance = wasm_instance_new(store, module, &no_imports, &instantiate_trap);
    CHECK(instance != NULL);
    CHECK(instantiate_trap == NULL);
    wasm_instance_exports(instance, &exports);
    CHECK(exports.num_elems == 2);
    CHECK(wasm_extern_kind(exports.data[0]) == WASM_EXTERN_FUNC);
    CHECK(wasm_extern_kind(exports.data[1]) == WASM_EXTERN_FUNC);
    many = wasm_extern_as_func(exports.data[0]);
    boom = wasm_extern_as_func(exports.data[1]);
    CHECK(many != NULL);
    CHECK(boom != NULL);
    CHECK(wasm_func_param_arity(many) == 20);
    CHECK(wasm_func_result_arity(many) == 1);

    for (i = 0; i < 20; i++) {
        param_values[i].kind = WASM_I32;
        param_values[i].of.i32 = (int32_t)i;
    }
    baseline = quota_snapshot(&quota);

    /* The first allocation after entering the instance owner is the heap
       argv.  Deny exactly that reservation, then allow the returned trap to
       be fully owner-charged. */
    quota_force_next_denials(&quota, 1);
    trap = wasm_func_call(many, &params, &results);
    CHECK(trap != NULL);
    after_call = quota_snapshot(&quota);
    CHECK(after_call.denied_calls == baseline.denied_calls + 1);
    CHECK(after_call.bytes > baseline.bytes);
    CHECK(after_call.allocations > baseline.allocations);
    CHECK(wasm_allocation_quota_allocations_share_owner(instance->inst_comm_rt,
                                                        trap));
    CHECK(results.num_elems == 0);
    CHECK(result_value.of.i32 == INT32_MIN);
    wasm_trap_message(trap, &message);
    CHECK(message.data != NULL);
    CHECK(strstr((const char *)message.data, "argument allocation failed")
          != NULL);
    wasm_byte_vec_delete(&message);
    CHECK(wasm_trap_origin(trap) == NULL);
    wasm_trap_trace(trap, &trace);
    CHECK(trace.num_elems == 0);
    CHECK(trace.data == NULL);
    wasm_frame_vec_delete(&trace);
    wasm_trap_delete(trap);
    trap = NULL;
    quota_check_live(&quota, baseline);

    /* With no headroom, both argv and ordinary trap construction are denied.
       wasm_func_call must still return its allocation-free emergency trap,
       never NULL (which means success in this API). */
    quota_set_limits(&quota, baseline.bytes, baseline.allocations);
    after_call = quota_snapshot(&quota);
    trap = wasm_func_call(many, &params, &results);
    CHECK(trap != NULL);
    CHECK(quota_snapshot(&quota).denied_calls == after_call.denied_calls + 2);
    quota_check_live(&quota, baseline);
    memset(&message, 0, sizeof(message));
    wasm_trap_message(trap, &message);
    CHECK(message.data != NULL);
    CHECK(strstr((const char *)message.data, "failed while allocating its trap")
          != NULL);
    wasm_byte_vec_delete(&message);
    wasm_trap_trace(trap, &trace);
    CHECK(trace.num_elems == 0);
    CHECK(trace.data == NULL);
    wasm_frame_vec_delete(&trace);
    wasm_trap_delete(trap);
    trap = NULL;
    quota_set_limits(&quota, UINT64_MAX, UINT32_MAX);
    quota_check_live(&quota, baseline);

    /* The successful path allocates and frees the same heap argv under the
       owner without changing the steady-state reservation. */
    result_value.of.i32 = INT32_MIN;
    results.num_elems = 0;
    CHECK(wasm_func_call(many, &params, &results) == NULL);
    CHECK(results.num_elems == 1);
    CHECK(result_value.kind == WASM_I32);
    CHECK(result_value.of.i32 == 19);
    quota_check_live(&quota, baseline);

    /* Runtime traps own a deep copy of captured frames.  A later successful
       call may clear runtime exception state but must not invalidate either
       the trap's copy or owned origin/trace copies returned to the embedder. */
    trap = wasm_func_call(boom, &no_results, &no_results);
    CHECK(trap != NULL);
    after_call = quota_snapshot(&quota);
    CHECK(after_call.bytes > baseline.bytes);
    CHECK(after_call.allocations > baseline.allocations);
    CHECK(wasm_allocation_quota_allocations_share_owner(instance->inst_comm_rt,
                                                        trap));
    wasm_trap_message(trap, &message);
    CHECK(message.data != NULL);
    CHECK(message.num_elems > 1);
    wasm_byte_vec_delete(&message);

    result_value.of.i32 = INT32_MIN;
    results.num_elems = 0;
    CHECK(wasm_func_call(many, &params, &results) == NULL);
    CHECK(results.num_elems == 1);
    CHECK(result_value.of.i32 == 19);

#if WASM_ENABLE_DUMP_CALL_STACK != 0
    origin = wasm_trap_origin(trap);
    CHECK(origin != NULL);
    wasm_trap_trace(trap, &trace);
    CHECK(trace.num_elems > 0);
    CHECK(trace.data != NULL);
    CHECK(trace.data[0] != NULL);
    CHECK(wasm_frame_instance(origin) == instance);
    CHECK(wasm_frame_instance(trace.data[0]) == instance);
    CHECK(wasm_frame_func_index(origin)
          == wasm_frame_func_index(trace.data[0]));
    origin_func_index = wasm_frame_func_index(origin);
    origin_module_offset = wasm_frame_module_offset(origin);
    origin_func_offset = wasm_frame_func_offset(origin);
    CHECK(origin_func_index == 1);
#else
    CHECK(wasm_trap_origin(trap) == NULL);
    wasm_trap_trace(trap, &trace);
    CHECK(trace.num_elems == 0);
    CHECK(trace.data == NULL);
#endif

    wasm_trap_delete(trap);
    trap = NULL;
#if WASM_ENABLE_DUMP_CALL_STACK != 0
    CHECK(wasm_frame_func_index(origin) == origin_func_index);
    CHECK(wasm_frame_module_offset(origin) == origin_module_offset);
    CHECK(wasm_frame_func_offset(origin) == origin_func_offset);
    CHECK(wasm_frame_func_index(trace.data[0]) == origin_func_index);
    wasm_frame_delete(origin);
    origin = NULL;
#endif
    wasm_frame_vec_delete(&trace);
#if WASM_ENABLE_DUMP_CALL_STACK != 0
    runtime_frames_live = quota_snapshot(&quota);
    CHECK(runtime_frames_live.bytes > baseline.bytes);
    CHECK(runtime_frames_live.bytes < after_call.bytes);
    CHECK(runtime_frames_live.allocations > baseline.allocations);
    CHECK(runtime_frames_live.allocations < after_call.allocations);

    /* The cluster retains its latest diagnostic frame vector until teardown.
       Replacing it with an equivalent trap must leave the same steady state,
       and deleting the second trap must refund exactly to that state. */
    trap = wasm_func_call(boom, &no_results, &no_results);
    CHECK(trap != NULL);
    quota_check_live(&quota, after_call);
    wasm_trap_delete(trap);
    trap = NULL;
    quota_check_live(&quota, runtime_frames_live);
#else
    quota_check_live(&quota, baseline);
#endif

    wasm_extern_vec_delete(&exports);
    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    quota_destroy(&quota);
}

static void
test_c_api_callback_copy_lifetime(wasm_engine_t *engine)
{
    wasm_store_t *store;
    wasm_valtype_vec_t no_params = WASM_EMPTY_VEC;
    wasm_valtype_vec_t no_results = WASM_EMPTY_VEC;
    wasm_functype_t *func_type;
    wasm_func_t *original;
    wasm_func_t *copy;
    uint32_t callback_calls = 0;
    uint32_t finalizer_calls = 0;
    CApiCallbackEnv *env;

    CHECK(engine != NULL);
    store = wasm_store_new(engine);
    CHECK(store != NULL);
    func_type = wasm_functype_new(&no_params, &no_results);
    CHECK(func_type != NULL);
    env = c_api_callback_env_new(&callback_calls, &finalizer_calls);
    original =
        wasm_func_new_with_env(store, func_type, c_api_import_callback_with_env,
                               env, c_api_callback_env_finalizer);
    CHECK(original != NULL);
    copy = wasm_func_copy(original);
    CHECK(copy != NULL);
    wasm_functype_delete(func_type);

    wasm_func_delete(original);
    CHECK(finalizer_calls == 0);
    wasm_func_delete(copy);
    CHECK(finalizer_calls == 1);
    CHECK(callback_calls == 0);
    wasm_store_delete(store);
    CHECK(finalizer_calls == 1);
}

static void
test_c_api_callback_instance_lifetime(wasm_engine_t *engine,
                                      uint32_t instance_count,
                                      bool exercise_thread_mgr_copy)
{
    QuotaState quota;
    LoadArgs2 args;
    wasm_store_t *store;
    wasm_byte_vec_t binary = { 0 };
    wasm_module_t *module;
    wasm_valtype_vec_t no_params = WASM_EMPTY_VEC;
    wasm_valtype_vec_t no_results = WASM_EMPTY_VEC;
    wasm_functype_t *func_type;
    wasm_func_t *host_func;
    wasm_extern_t *import_data[2];
    wasm_extern_vec_t imports = WASM_ARRAY_VEC(import_data);
    wasm_instance_t *instances[2] = { NULL, NULL };
    wasm_trap_t *trap = NULL;
    wasm_val_vec_t empty_values = WASM_EMPTY_VEC;
    uint32_t callback_calls = 0;
    uint32_t finalizer_calls = 0;
    uint32_t i;
    CApiCallbackEnv *env;

    CHECK(engine != NULL);
    CHECK(instance_count > 0 && instance_count <= 2);
    CHECK(!exercise_thread_mgr_copy || instance_count == 2);
    store = wasm_store_new(engine);
    CHECK(store != NULL);
    wasm_byte_vec_new(&binary, sizeof(c_api_import_binding_module),
                      (const byte_t *)c_api_import_binding_module);
    CHECK(binary.data != NULL);

    quota_init(&quota);
    args = quota_load_args(&quota, "c-api-callback-lifetime");
    module = wasm_module_new_ex2(store, &binary, &args);
    CHECK(module != NULL);
    func_type = wasm_functype_new(&no_params, &no_results);
    CHECK(func_type != NULL);
    env = c_api_callback_env_new(&callback_calls, &finalizer_calls);
    host_func =
        wasm_func_new_with_env(store, func_type, c_api_import_callback_with_env,
                               env, c_api_callback_env_finalizer);
    CHECK(host_func != NULL);
    wasm_functype_delete(func_type);
    import_data[0] = wasm_func_as_extern(host_func);
    CHECK(import_data[0] != NULL);
    import_data[1] = import_data[0];

    for (i = 0; i < instance_count; i++) {
        instances[i] = wasm_instance_new(store, module, &imports, &trap);
        CHECK(instances[i] != NULL);
        CHECK(trap == NULL);
    }

#if WASM_ENABLE_THREAD_MGR != 0
    if (exercise_thread_mgr_copy) {
        WASMAllocationQuotaScope scope;
        WASMModuleInstance *destination =
            (WASMModuleInstance *)instances[1]->inst_comm_rt;

        CHECK(wasm_allocation_quota_scope_enter_for_allocation(
            &scope, instances[1]->inst_comm_rt));
        CHECK(destination->c_api_func_imports != NULL);
        wasm_c_api_func_imports_release(destination->c_api_func_imports, 2);
        wasm_runtime_free(destination->c_api_func_imports);
        destination->c_api_func_imports = NULL;
        CHECK(wasm_cluster_dup_c_api_imports(instances[1]->inst_comm_rt,
                                             instances[0]->inst_comm_rt));
        wasm_allocation_quota_scope_leave(&scope);
    }
#else
    CHECK(!exercise_thread_mgr_copy);
#endif

    /* Each instance owns its own payload reference. The original host wrapper
       can be deleted before either guest invokes the callback. */
    wasm_func_delete(host_func);
    CHECK(finalizer_calls == 0);
    for (i = 0; i < instance_count; i++) {
        wasm_extern_vec_t exports = WASM_EMPTY_VEC;
        wasm_func_t *invoke;

        wasm_instance_exports(instances[i], &exports);
        CHECK(exports.num_elems == 1);
        CHECK(wasm_extern_kind(exports.data[0]) == WASM_EXTERN_FUNC);
        invoke = wasm_extern_as_func(exports.data[0]);
        CHECK(invoke != NULL);
        CHECK(wasm_func_call(invoke, &empty_values, &empty_values) == NULL);
        wasm_extern_vec_delete(&exports);
    }
    CHECK(callback_calls == instance_count * 2);
    CHECK(finalizer_calls == 0);

    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    CHECK(finalizer_calls == 1);
    quota_destroy(&quota);
}

static void
test_c_api_late_instantiate_failure_restores_import_binding(
    wasm_engine_t *engine)
{
    QuotaState quota;
    LoadArgs2 args;
    wasm_store_t *store;
    wasm_byte_vec_t binary = { 0 };
    wasm_module_t *module;
    wasm_valtype_vec_t no_params = WASM_EMPTY_VEC;
    wasm_valtype_vec_t no_results = WASM_EMPTY_VEC;
    wasm_functype_t *func_type;
    wasm_func_t *host_func;
    wasm_extern_t *import_data[2];
    wasm_extern_vec_t imports = WASM_ARRAY_VEC(import_data);
    wasm_val_vec_t empty_values = WASM_EMPTY_VEC;
    wasm_instance_t *calibration_instance;
    wasm_instance_t *failed_instance;
    wasm_trap_t *trap = NULL;
    WASMModuleInstanceCommon *prior_inst_comm_rt;
    WASMFunctionInstanceCommon *prior_func_comm_rt;
    uint16_t prior_func_idx_rt;
    uint64_t calibration_start;
    uint64_t calibration_end;
    uint64_t instantiate_reservations;
    uint32_t denied_before;

    CHECK(engine != NULL);
    store = wasm_store_new(engine);
    CHECK(store != NULL);
    wasm_byte_vec_new(&binary, sizeof(c_api_import_binding_module),
                      (const byte_t *)c_api_import_binding_module);
    CHECK(binary.data != NULL);

    quota_init(&quota);
    args = quota_load_args(&quota, "c-api-import-binding");
    module = wasm_module_new_ex2(store, &binary, &args);
    CHECK(module != NULL);

    func_type = wasm_functype_new(&no_params, &no_results);
    CHECK(func_type != NULL);
    host_func = wasm_func_new(store, func_type, c_api_import_callback);
    CHECK(host_func != NULL);
    wasm_functype_delete(func_type);
    import_data[0] = wasm_func_as_extern(host_func);
    CHECK(import_data[0] != NULL);
    import_data[1] = import_data[0];

    /* A successful instance both establishes the binding that must survive
       and calibrates the deterministic owner-reservation span. The final
       reservation in this span belongs to export-wrapper construction, after
       imports have been rebound to the candidate runtime instance. */
    calibration_start = quota_reserve_call_count(&quota);
    calibration_instance = wasm_instance_new(store, module, &imports, &trap);
    CHECK(calibration_instance != NULL);
    CHECK(trap == NULL);
    calibration_end = quota_reserve_call_count(&quota);
    CHECK(calibration_end > calibration_start);
    instantiate_reservations = calibration_end - calibration_start;
    prior_inst_comm_rt = host_func->inst_comm_rt;
    prior_func_comm_rt = host_func->func_comm_rt;
    prior_func_idx_rt = host_func->func_idx_rt;
    CHECK(prior_inst_comm_rt == calibration_instance->inst_comm_rt);

    denied_before = quota_snapshot(&quota).denied_calls;
    quota_deny_reserve_call(&quota, quota_reserve_call_count(&quota)
                                        + instantiate_reservations);
    failed_instance = wasm_instance_new(store, module, &imports, &trap);
    CHECK(failed_instance == NULL);
    CHECK(trap != NULL);
    CHECK(quota_snapshot(&quota).denied_calls == denied_before + 1);
    wasm_trap_delete(trap);
    trap = NULL;

    /* Before the transactional restore, this call dereferenced the freed
       candidate instance. ASan reports a heap-use-after-free here. */
    c_api_import_callback_calls = 0;
    CHECK(wasm_func_call(host_func, &empty_values, &empty_values) == NULL);
    CHECK(c_api_import_callback_calls == 1);
    CHECK(host_func->inst_comm_rt == prior_inst_comm_rt);
    CHECK(host_func->func_comm_rt == prior_func_comm_rt);
    CHECK(host_func->func_idx_rt == prior_func_idx_rt);

    wasm_func_delete(host_func);
    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    quota_destroy(&quota);
}

#if WASM_ENABLE_THREAD_MGR != 0
typedef struct ThreadCall {
    wasm_exec_env_t exec_env;
    wasm_module_inst_t instance;
    bool initialized;
    bool called;
} ThreadCall;

typedef struct SpawnProbe {
    bool saw_owner;
    bool called;
} SpawnProbe;

static void *
raw_thread_call(void *arg)
{
    ThreadCall *call = arg;

    CHECK(wasm_allocation_quota_get_current() == NULL);
    call->initialized = wasm_runtime_init_thread_env();
    if (call->initialized) {
        call_answer(call->exec_env, call->instance);
        call->called = true;
        wasm_runtime_destroy_thread_env();
    }
    CHECK(wasm_allocation_quota_get_current() == NULL);
    return NULL;
}

static void *
destroy_spawned_exec_env(void *arg)
{
    wasm_exec_env_t exec_env = arg;

    CHECK(wasm_allocation_quota_get_current() == NULL);
    wasm_runtime_destroy_spawned_exec_env(exec_env);
    CHECK(wasm_allocation_quota_get_current() == NULL);
    return NULL;
}

static void *
spawned_runtime_callback(wasm_exec_env_t exec_env, void *arg)
{
    SpawnProbe *probe = arg;
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
    uint8_t *allocation;

    probe->saw_owner = wasm_allocation_quota_get_current() != NULL;
    allocation = wasm_runtime_malloc(37);
    CHECK(allocation != NULL);
    allocation = wasm_runtime_realloc(allocation, 137);
    CHECK(allocation != NULL);
    wasm_runtime_free(allocation);
    call_answer(exec_env, instance);
    probe->called = true;
    return (void *)(uintptr_t)0x51;
}

static void *
spawned_runtime_direct_exit_callback(wasm_exec_env_t exec_env, void *arg)
{
    SpawnProbe *probe = arg;
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
    WASMAllocationQuotaScope outer_scope;
    WASMAllocationQuotaScope inner_scope;

    probe->saw_owner = wasm_allocation_quota_get_current() != NULL;
    call_answer(exec_env, instance);
    CHECK(wasm_allocation_quota_scope_enter_for_allocation(&outer_scope,
                                                           exec_env));
    CHECK(wasm_allocation_quota_scope_enter_for_allocation(&inner_scope,
                                                           instance));
    probe->called = true;

    /* This does not return. It must free the generic runtime-thread wrapper
       once and unwind both retained TLS scopes in strict LIFO order. */
    wasm_cluster_exit_thread(exec_env, (void *)(uintptr_t)0x52);
    CHECK(false);
    return NULL;
}

static void
test_threads_cross_thread_teardown_and_parallel_owners(void)
{
    char error_buf[256] = { 0 };
    QuotaState shared_quota;
    QuotaSnapshot first_ready;
    QuotaSnapshot both_ready;
    wasm_module_t first_module;
    wasm_module_t second_module;
    wasm_module_inst_t first_instance;
    wasm_module_inst_t second_instance;
    wasm_exec_env_t first_exec_env;
    wasm_exec_env_t second_exec_env;
    wasm_exec_env_t spawned_exec_env;
    ThreadCall first_call = { 0 };
    ThreadCall second_call = { 0 };
    SpawnProbe probe = { 0 };
    pthread_t first_thread;
    pthread_t second_thread;
    pthread_t destroy_thread;
    wasm_thread_t runtime_thread;
    void *retval = NULL;

    quota_init(&shared_quota);
    first_module =
        load_owned_module(execution_module, (uint32_t)sizeof(execution_module),
                          &shared_quota, "parallel-first");
    CHECK(first_module != NULL);
    first_instance = wasm_runtime_instantiate(first_module, 8192, 0, error_buf,
                                              (uint32_t)sizeof(error_buf));
    CHECK(first_instance != NULL);
    first_exec_env = wasm_runtime_create_exec_env(first_instance, 16384);
    CHECK(first_exec_env != NULL);
    first_ready = quota_snapshot(&shared_quota);

    memset(error_buf, 0, sizeof(error_buf));
    second_module =
        load_owned_module(execution_module, (uint32_t)sizeof(execution_module),
                          &shared_quota, "parallel-second");
    CHECK(second_module != NULL);
    CHECK(!wasm_allocation_quota_allocations_share_owner(first_module,
                                                         second_module));
    second_instance = wasm_runtime_instantiate(
        second_module, 8192, 0, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(second_instance != NULL);
    second_exec_env = wasm_runtime_create_exec_env(second_instance, 16384);
    CHECK(second_exec_env != NULL);
    both_ready = quota_snapshot(&shared_quota);

    first_call.exec_env = first_exec_env;
    first_call.instance = first_instance;
    second_call.exec_env = second_exec_env;
    second_call.instance = second_instance;
    CHECK(pthread_create(&first_thread, NULL, raw_thread_call, &first_call)
          == 0);
    CHECK(pthread_create(&second_thread, NULL, raw_thread_call, &second_call)
          == 0);
    CHECK(pthread_join(first_thread, NULL) == 0);
    CHECK(pthread_join(second_thread, NULL) == 0);
    CHECK(first_call.initialized && first_call.called);
    CHECK(second_call.initialized && second_call.called);
    quota_check_live(&shared_quota, both_ready);

    spawned_exec_env = wasm_runtime_spawn_exec_env(first_exec_env);
    CHECK(spawned_exec_env != NULL);
    CHECK(pthread_create(&destroy_thread, NULL, destroy_spawned_exec_env,
                         spawned_exec_env)
          == 0);
    CHECK(pthread_join(destroy_thread, NULL) == 0);
    quota_check_live(&shared_quota, both_ready);

    CHECK(wasm_runtime_spawn_thread(first_exec_env, &runtime_thread,
                                    spawned_runtime_callback, &probe)
          == 0);
    CHECK(wasm_runtime_join_thread(runtime_thread, &retval) == 0);
    CHECK(retval == (void *)(uintptr_t)0x51);
    CHECK(probe.saw_owner && probe.called);
    quota_check_live(&shared_quota, both_ready);

    memset(&probe, 0, sizeof(probe));
    retval = NULL;
    CHECK(wasm_runtime_spawn_thread(first_exec_env, &runtime_thread,
                                    spawned_runtime_direct_exit_callback,
                                    &probe)
          == 0);
    CHECK(wasm_runtime_join_thread(runtime_thread, &retval) == 0);
    CHECK(retval == (void *)(uintptr_t)0x52);
    CHECK(probe.saw_owner && probe.called);
    quota_check_live(&shared_quota, both_ready);

    wasm_runtime_destroy_exec_env(second_exec_env);
    wasm_runtime_deinstantiate(second_instance);
    wasm_runtime_unload(second_module);
    quota_check_live(&shared_quota, first_ready);
    wasm_runtime_destroy_exec_env(first_exec_env);
    wasm_runtime_deinstantiate(first_instance);
    wasm_runtime_unload(first_module);
    quota_destroy(&shared_quota);
}
#endif

static wasm_engine_t *
c_api_runtime_mode_init(mem_alloc_type_t mode, RuntimeModeState *state)
{
    MemAllocOption option;
    wasm_config_t config;
    wasm_engine_t *engine;

    memset(state, 0, sizeof(*state));
    memset(&option, 0, sizeof(option));
    memset(&config, 0, sizeof(config));
    if (mode == Alloc_With_Pool) {
        state->pool = malloc(32 * 1024 * 1024);
        CHECK(state->pool != NULL);
        option.pool.heap_buf = state->pool;
        option.pool.heap_size = 32 * 1024 * 1024;
    }
    else if (mode == Alloc_With_Allocator) {
        CHECK(pthread_mutex_init(&state->allocator.lock, NULL) == 0);
        state->allocator_inited = true;
        option.allocator.malloc_func = (void *)custom_malloc;
        option.allocator.realloc_func = (void *)custom_realloc;
        option.allocator.free_func = (void *)custom_free;
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
        option.allocator.user_data = &state->allocator;
#else
        active_mode_state = state;
#endif
    }

    CHECK(wasm_config_set_mem_alloc_opt(&config, mode, &option) == &config);
    engine = wasm_engine_new_with_config(&config);
    CHECK(engine != NULL);
    return engine;
}

static void
c_api_runtime_mode_destroy(mem_alloc_type_t mode, RuntimeModeState *state,
                           wasm_engine_t *engine)
{
    wasm_engine_delete(engine);
    if (mode == Alloc_With_Allocator) {
        CHECK(state->allocator.live_allocations == 0);
        CHECK(pthread_mutex_destroy(&state->allocator.lock) == 0);
#if WASM_MEM_ALLOC_WITH_USER_DATA == 0
        active_mode_state = NULL;
#endif
    }
    free(state->pool);
}

static void
run_c_api_mode(mem_alloc_type_t mode)
{
    RuntimeModeState state;
    wasm_engine_t *engine = c_api_runtime_mode_init(mode, &state);

    test_c_api_callback_copy_lifetime(engine);
    test_c_api_callback_instance_lifetime(engine, 1, false);
    test_c_api_callback_instance_lifetime(engine, 2, true);
    test_c_api_call_argv_denial_and_trap_lifetime(engine);
    test_c_api_late_instantiate_failure_restores_import_binding(engine);
    c_api_runtime_mode_destroy(mode, &state, engine);
}

static void
run_mode(mem_alloc_type_t mode)
{
    RuntimeModeState state;

    runtime_mode_init(mode, &state);
    test_instantiation_exec_env_calls_and_refunds();
    test_externref_reentrant_cleanup_and_refunds(EXTERNREF_CLEANUP_OBJDEL);
    test_externref_reentrant_cleanup_and_refunds(EXTERNREF_CLEANUP_RECLAIM);
    test_externref_reentrant_cleanup_and_refunds(EXTERNREF_CLEANUP_MODULE);
    test_huge_table_instantiation_denial_refunds();
#if WASM_ENABLE_THREAD_MGR != 0
    test_threads_cross_thread_teardown_and_parallel_owners();
#endif
    runtime_mode_destroy(mode, &state);
}

int
main(void)
{
    run_mode(Alloc_With_System_Allocator);
    run_mode(Alloc_With_Allocator);
    run_mode(Alloc_With_Pool);
    run_c_api_mode(Alloc_With_System_Allocator);
    run_c_api_mode(Alloc_With_Allocator);
    run_c_api_mode(Alloc_With_Pool);
    puts("allocation quota execution checks passed");
    return 0;
}
