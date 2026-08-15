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

#include "wasm_allocation_quota.h"
#include "wasm.h"
#include "wasm_c_api.h"
#include "wasm_export.h"
#include "wasm_runtime_common.h"

#define CHECK(condition)                                            \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr,                                         \
                    "allocation quota load check failed at %s:%d: " \
                    "%s\n",                                         \
                    __FILE__, __LINE__, #condition);                \
            abort();                                                \
        }                                                           \
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
    uint32_t reserve_calls;
    uint32_t release_calls;
    uint32_t denied_calls;
    uint32_t attachment_retain_calls;
    uint32_t attachment_release_calls;
} QuotaState;

static const uint8_t empty_module[] = { 0x00, 0x61, 0x73, 0x6d,
                                        0x01, 0x00, 0x00, 0x00 };

#if WASM_ENABLE_MULTI_MODULE != 0
static const uint8_t function_dependency_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
    0x00, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x05, 0x01, 0x01, 0x66,
    0x00, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x07, 0x0b
};

static const uint8_t function_parent_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
    0x00, 0x01, 0x7f, 0x02, 0x1d, 0x01, 0x17, 0x71, 0x75, 0x6f, 0x74, 0x61,
    0x2d, 0x6c, 0x69, 0x6e, 0x6b, 0x65, 0x64, 0x2d, 0x64, 0x65, 0x70, 0x65,
    0x6e, 0x64, 0x65, 0x6e, 0x63, 0x79, 0x01, 0x66, 0x00, 0x00, 0x03, 0x02,
    0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x01,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b
};
#endif

static void
quota_init(QuotaState *quota)
{
    memset(quota, 0, sizeof(*quota));
    CHECK(pthread_mutex_init(&quota->lock, NULL) == 0);
    quota->byte_limit = UINT64_MAX;
    quota->allocation_limit = UINT32_MAX;
}

static void
quota_destroy(QuotaState *quota)
{
    CHECK(quota->live_bytes == 0);
    CHECK(quota->live_allocations == 0);
    CHECK(pthread_mutex_destroy(&quota->lock) == 0);
}

static bool
quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;
    bool allowed;

    pthread_mutex_lock(&quota->lock);
    quota->reserve_calls++;
    allowed =
        quota->live_bytes <= quota->byte_limit
        && bytes <= quota->byte_limit - quota->live_bytes
        && quota->live_allocations <= quota->allocation_limit
        && allocations <= quota->allocation_limit - quota->live_allocations;
    if (allowed) {
        quota->live_bytes += bytes;
        quota->live_allocations += allocations;
        quota->reserved_bytes += bytes;
        quota->reserved_allocations += allocations;
    }
    else {
        quota->denied_calls++;
    }
    pthread_mutex_unlock(&quota->lock);
    return allowed;
}

static void
quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;

    pthread_mutex_lock(&quota->lock);
    CHECK(bytes <= quota->live_bytes);
    CHECK(allocations <= quota->live_allocations);
    quota->live_bytes -= bytes;
    quota->live_allocations -= allocations;
    quota->released_bytes += bytes;
    quota->released_allocations += allocations;
    quota->release_calls++;
    pthread_mutex_unlock(&quota->lock);
}

static bool
quota_attachment_retain(void *attachment)
{
    QuotaState *quota = attachment;

    pthread_mutex_lock(&quota->lock);
    quota->attachment_retain_calls++;
    pthread_mutex_unlock(&quota->lock);
    return true;
}

static void
quota_attachment_release(void *attachment)
{
    QuotaState *quota = attachment;

    pthread_mutex_lock(&quota->lock);
    quota->attachment_release_calls++;
    pthread_mutex_unlock(&quota->lock);
}

static LoadArgs2
quota_load_args(QuotaState *quota)
{
    LoadArgs2 args;

    wasm_runtime_load_args2_init(&args);
    args.name = "quota-test";
    wasm_runtime_load_args2_set_allocation_quota(
        &args, quota, quota_reserve, quota_release, quota_attachment_retain,
        quota_attachment_release);
    return args;
}

static void
check_balanced(const QuotaState *quota)
{
    CHECK(quota->live_bytes == 0);
    CHECK(quota->live_allocations == 0);
    CHECK(quota->reserved_bytes == quota->released_bytes);
    CHECK(quota->reserved_allocations == quota->released_allocations);
}

static void
test_legacy_load_args_remain_usable(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    LoadArgs args = { 0 };
    wasm_module_t module;

    memcpy(binary, empty_module, sizeof(binary));
    args.name = "legacy";
    module = wasm_runtime_load_ex(binary, (uint32_t)sizeof(binary), &args,
                                  error_buf, (uint32_t)sizeof(error_buf));
    CHECK(module != NULL);
#if WASM_ENABLE_MULTI_MODULE != 0
    CHECK(wasm_runtime_unregister_module(module));
#endif
    wasm_runtime_unload(module);
}

static void
test_version_validation_happens_before_quota_callbacks(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[128];
    QuotaState quota;
    LoadArgs2 args;

    quota_init(&quota);
    args = quota_load_args(&quota);
    args.struct_size = (uint32_t)sizeof(args) - 1;
    memset(error_buf, 0, sizeof(error_buf));
    memcpy(binary, empty_module, sizeof(binary));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "undersized") != NULL);
    CHECK(quota.attachment_retain_calls == 0);
    CHECK(quota.reserve_calls == 0);

    args = quota_load_args(&quota);
    args.abi_version++;
    memset(error_buf, 0, sizeof(error_buf));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "unsupported") != NULL);
    CHECK(quota.attachment_retain_calls == 0);

    wasm_runtime_load_args2_init(&args);
    args.allocation_quota_attachment = &quota;
    args.allocation_quota_reserve = quota_reserve;
    memset(error_buf, 0, sizeof(error_buf));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "incomplete") != NULL);
    CHECK(quota.attachment_retain_calls == 0);
    CHECK(quota.reserve_calls == 0);

    args = quota_load_args(&quota);
    args.reserved[0] = 1;
    memset(error_buf, 0, sizeof(error_buf));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "invalid") != NULL);
    CHECK(quota.attachment_retain_calls == 0);
    check_balanced(&quota);
    quota_destroy(&quota);
}

static void
test_low_cap_fails_before_first_module_allocation(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    QuotaState quota;
    LoadArgs2 args;

    quota_init(&quota);
    quota.allocation_limit = 1;
    args = quota_load_args(&quota);
    memcpy(binary, empty_module, sizeof(binary));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 1);
    CHECK(quota.denied_calls >= 1);
    check_balanced(&quota);
    quota_destroy(&quota);
}

static void
test_successful_load_retains_one_owner_until_unload(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    QuotaState quota;
    LoadArgs2 args;
    wasm_module_t module;

    quota_init(&quota);
    args = quota_load_args(&quota);
    memcpy(binary, empty_module, sizeof(binary));
    module = wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                   error_buf, (uint32_t)sizeof(error_buf));
    CHECK(module != NULL);
    CHECK(wasm_allocation_quota_get_current() == NULL);
    CHECK(quota.live_allocations > 1);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 0);

#if WASM_ENABLE_MULTI_MODULE != 0
    CHECK(wasm_runtime_unregister_module(module));
#endif
    wasm_runtime_unload(module);
    CHECK(wasm_allocation_quota_get_current() == NULL);
    CHECK(quota.attachment_release_calls == 1);
    check_balanced(&quota);
    quota_destroy(&quota);
}

static void
test_nested_owner_mismatch_fails_closed(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    QuotaState first;
    QuotaState second;
    LoadArgs2 first_args;
    LoadArgs2 second_args;
    WASMAllocationQuotaToken *token;

    quota_init(&first);
    quota_init(&second);
    first_args = quota_load_args(&first);
    second_args = quota_load_args(&second);
    token = wasm_allocation_quota_token_create(&first_args);
    CHECK(token != NULL);
    CHECK(wasm_allocation_quota_set_current(token) == NULL);

    memcpy(binary, empty_module, sizeof(binary));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &second_args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "owner") != NULL);
    CHECK(second.attachment_retain_calls == 0);
    CHECK(second.reserve_calls == 0);

    CHECK(wasm_allocation_quota_set_current(NULL) == token);
    wasm_allocation_quota_token_release(token);
    CHECK(first.attachment_release_calls == 1);
    check_balanced(&first);
    check_balanced(&second);
    quota_destroy(&second);
    quota_destroy(&first);
}

#if WASM_ENABLE_MULTI_MODULE != 0
static wasm_module_t
load_legacy(uint8_t *binary, uint32_t binary_size, const char *name,
            bool no_resolve)
{
    char error_buf[128] = { 0 };
    LoadArgs args = { 0 };

    args.name = (char *)name;
    args.no_resolve = no_resolve;
    return wasm_runtime_load_ex(binary, binary_size, &args, error_buf,
                                (uint32_t)sizeof(error_buf));
}

static void
test_multi_module_dependency_owner_mismatch_is_denied(void)
{
    uint8_t dependency_binary[sizeof(empty_module)];
    uint8_t parent_binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    QuotaState dependency_quota;
    QuotaState parent_quota;
    LoadArgs2 dependency_args;
    LoadArgs2 parent_args;
    wasm_module_t dependency;
    wasm_module_t parent;

    quota_init(&dependency_quota);
    quota_init(&parent_quota);
    dependency_args = quota_load_args(&dependency_quota);
    parent_args = quota_load_args(&parent_quota);
    memcpy(dependency_binary, empty_module, sizeof(dependency_binary));
    memcpy(parent_binary, empty_module, sizeof(parent_binary));
    dependency = wasm_runtime_load_ex2(
        dependency_binary, (uint32_t)sizeof(dependency_binary),
        &dependency_args, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(dependency != NULL);
    parent = wasm_runtime_load_ex2(
        parent_binary, (uint32_t)sizeof(parent_binary), &parent_args, error_buf,
        (uint32_t)sizeof(error_buf));
    CHECK(parent != NULL);
    CHECK(!wasm_allocation_quota_allocations_share_owner(parent, dependency));
    CHECK(wasm_runtime_register_module("quota-mismatch-dependency", dependency,
                                       error_buf, (uint32_t)sizeof(error_buf)));

    memset(error_buf, 0, sizeof(error_buf));
    CHECK(wasm_runtime_load_depended_module(parent, "quota-mismatch-dependency",
                                            error_buf,
                                            (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(wasm_allocation_quota_get_current() == NULL);
    CHECK(strstr(error_buf, "owner mismatch") != NULL);
    CHECK(wasm_runtime_search_sub_module(parent, "quota-mismatch-dependency")
          == NULL);

    CHECK(wasm_runtime_unregister_module(parent));
    wasm_runtime_unload(parent);
    CHECK(wasm_runtime_unregister_module(dependency));
    wasm_runtime_unload(dependency);
    CHECK(parent_quota.attachment_release_calls == 1);
    CHECK(dependency_quota.attachment_release_calls == 1);
    check_balanced(&parent_quota);
    check_balanced(&dependency_quota);
    quota_destroy(&parent_quota);
    quota_destroy(&dependency_quota);
}

static void
test_multi_module_dependency_same_owner_reuses_and_refunds(void)
{
    uint8_t dependency_binary[sizeof(empty_module)];
    uint8_t parent_binary[sizeof(empty_module)];
    char error_buf[128] = { 0 };
    QuotaState quota;
    LoadArgs2 args;
    WASMAllocationQuotaToken *token;
    wasm_module_t dependency;
    wasm_module_t parent;
    wasm_module_t reused;

    quota_init(&quota);
    args = quota_load_args(&quota);
    token = wasm_allocation_quota_token_create(&args);
    CHECK(token != NULL);
    CHECK(wasm_allocation_quota_set_current(token) == NULL);
    memcpy(dependency_binary, empty_module, sizeof(dependency_binary));
    memcpy(parent_binary, empty_module, sizeof(parent_binary));
    dependency =
        load_legacy(dependency_binary, (uint32_t)sizeof(dependency_binary),
                    "same-owner-dependency", false);
    parent = load_legacy(parent_binary, (uint32_t)sizeof(parent_binary),
                         "same-owner-parent", false);
    CHECK(dependency != NULL);
    CHECK(parent != NULL);
    CHECK(wasm_runtime_register_module("quota-same-owner-dependency",
                                       dependency, error_buf,
                                       (uint32_t)sizeof(error_buf)));
    CHECK(wasm_allocation_quota_set_current(NULL) == token);
    wasm_allocation_quota_token_release(token);

    CHECK(wasm_allocation_quota_allocations_share_owner(parent, dependency));
    reused = wasm_runtime_load_depended_module(
        parent, "quota-same-owner-dependency", error_buf,
        (uint32_t)sizeof(error_buf));
    CHECK(wasm_allocation_quota_get_current() == NULL);
    CHECK(reused == dependency);
    CHECK(wasm_runtime_search_sub_module(parent, "quota-same-owner-dependency")
          == dependency);
    CHECK(quota.attachment_retain_calls == 1);

    CHECK(wasm_runtime_unregister_module(parent));
    wasm_runtime_unload(parent);
    CHECK(wasm_runtime_unregister_module(dependency));
    wasm_runtime_unload(dependency);
    CHECK(quota.attachment_release_calls == 1);
    check_balanced(&quota);
    quota_destroy(&quota);
}

static void
test_multi_module_resolver_stays_bound_to_checked_dependency(void)
{
    uint8_t first_dependency_binary[sizeof(function_dependency_module)];
    uint8_t second_dependency_binary[sizeof(function_dependency_module)];
    uint8_t parent_binary[sizeof(function_parent_module)];
    char error_buf[128] = { 0 };
    QuotaState graph_quota;
    QuotaState conflicting_quota;
    QuotaState wrong_ambient_quota;
    LoadArgs2 graph_args;
    LoadArgs2 conflicting_args;
    LoadArgs2 wrong_ambient_args;
    WASMAllocationQuotaToken *graph_token;
    WASMAllocationQuotaToken *wrong_ambient_token;
    wasm_module_t first_dependency;
    wasm_module_t second_dependency;
    wasm_module_t parent;
    WASMFunctionImport *resolved_import;

    quota_init(&graph_quota);
    graph_args = quota_load_args(&graph_quota);
    graph_token = wasm_allocation_quota_token_create(&graph_args);
    CHECK(graph_token != NULL);
    CHECK(wasm_allocation_quota_set_current(graph_token) == NULL);
    memcpy(first_dependency_binary, function_dependency_module,
           sizeof(first_dependency_binary));
    memcpy(parent_binary, function_parent_module, sizeof(parent_binary));
    first_dependency = load_legacy(first_dependency_binary,
                                   (uint32_t)sizeof(first_dependency_binary),
                                   "checked-dependency", false);
    parent = load_legacy(parent_binary, (uint32_t)sizeof(parent_binary),
                         "deferred-parent", true);
    CHECK(first_dependency != NULL);
    CHECK(parent != NULL);
    CHECK(wasm_allocation_quota_set_current(NULL) == graph_token);
    wasm_allocation_quota_token_release(graph_token);

    CHECK(wasm_runtime_register_module("quota-linked-dependency",
                                       first_dependency, error_buf,
                                       (uint32_t)sizeof(error_buf)));
    CHECK(wasm_runtime_load_depended_module(parent, "quota-linked-dependency",
                                            error_buf,
                                            (uint32_t)sizeof(error_buf))
          == first_dependency);
    CHECK(wasm_allocation_quota_get_current() == NULL);
    CHECK(wasm_runtime_unregister_module(first_dependency));

    quota_init(&conflicting_quota);
    conflicting_args = quota_load_args(&conflicting_quota);
    memcpy(second_dependency_binary, function_dependency_module,
           sizeof(second_dependency_binary));
    second_dependency = wasm_runtime_load_ex2(
        second_dependency_binary, (uint32_t)sizeof(second_dependency_binary),
        &conflicting_args, error_buf, (uint32_t)sizeof(error_buf));
    CHECK(second_dependency != NULL);
    CHECK(wasm_runtime_register_module("quota-linked-dependency",
                                       second_dependency, error_buf,
                                       (uint32_t)sizeof(error_buf)));

    quota_init(&wrong_ambient_quota);
    wrong_ambient_args = quota_load_args(&wrong_ambient_quota);
    wrong_ambient_token =
        wasm_allocation_quota_token_create(&wrong_ambient_args);
    CHECK(wrong_ambient_token != NULL);
    CHECK(wasm_allocation_quota_set_current(wrong_ambient_token) == NULL);
    memset(error_buf, 0, sizeof(error_buf));
    CHECK(wasm_runtime_load_depended_module(parent, "quota-linked-dependency",
                                            error_buf,
                                            (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "scope") != NULL);
    CHECK(wasm_allocation_quota_get_current() == wrong_ambient_token);
    CHECK(wasm_allocation_quota_set_current(NULL) == wrong_ambient_token);
    wasm_allocation_quota_token_release(wrong_ambient_token);
    CHECK(wrong_ambient_quota.attachment_retain_calls == 1);
    CHECK(wrong_ambient_quota.attachment_release_calls == 1);
    check_balanced(&wrong_ambient_quota);
    quota_destroy(&wrong_ambient_quota);

    CHECK(wasm_runtime_resolve_symbols(parent));
    CHECK(wasm_allocation_quota_get_current() == NULL);
    resolved_import = &((WASMModule *)parent)->import_functions[0].u.function;
    CHECK(resolved_import->import_module == (WASMModule *)first_dependency);
    CHECK(resolved_import->import_func_linked
          == ((WASMModule *)first_dependency)->functions[0]);
    CHECK(resolved_import->import_func_linked
          != ((WASMModule *)second_dependency)->functions[0]);

    CHECK(wasm_runtime_unregister_module(second_dependency));
    wasm_runtime_unload(second_dependency);
    CHECK(wasm_runtime_unregister_module(parent));
    wasm_runtime_unload(parent);
    wasm_runtime_unload(first_dependency);
    CHECK(graph_quota.attachment_retain_calls == 1);
    CHECK(graph_quota.attachment_release_calls == 1);
    CHECK(conflicting_quota.attachment_retain_calls == 1);
    CHECK(conflicting_quota.attachment_release_calls == 1);
    check_balanced(&graph_quota);
    check_balanced(&conflicting_quota);
    quota_destroy(&conflicting_quota);
    quota_destroy(&graph_quota);
}
#endif

static void
test_c_api_versioned_load_owns_wrapper_graph(void)
{
    wasm_engine_t *engine;
    wasm_store_t *store;
    wasm_byte_vec_t binary = { 0 };
    wasm_module_t *module;
    QuotaState denied;
    QuotaState quota;
    LoadArgs2 args;

    engine = wasm_engine_new();
    CHECK(engine != NULL);
    store = wasm_store_new(engine);
    CHECK(store != NULL);
    wasm_byte_vec_new(&binary, sizeof(empty_module),
                      (const byte_t *)empty_module);
    CHECK(binary.data != NULL);

    quota_init(&denied);
    denied.allocation_limit = 1;
    args = quota_load_args(&denied);
    args.clone_wasm_binary = 1;
    CHECK(wasm_module_new_ex2(store, &binary, &args) == NULL);
    CHECK(denied.denied_calls >= 1);
    CHECK(denied.attachment_retain_calls == 1);
    CHECK(denied.attachment_release_calls == 1);
    check_balanced(&denied);
    quota_destroy(&denied);

    quota_init(&quota);
    args = quota_load_args(&quota);
    args.clone_wasm_binary = 1;
    module = wasm_module_new_ex2(store, &binary, &args);
    CHECK(module != NULL);
    CHECK(quota.live_allocations > 1);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 0);

    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    CHECK(quota.attachment_release_calls == 0);
    wasm_engine_delete(engine);
    CHECK(quota.attachment_release_calls == 1);
    check_balanced(&quota);
    quota_destroy(&quota);
}

#if WASM_ENABLE_WASM_CACHE != 0
static void
test_c_api_cache_reuses_only_the_current_owner(void)
{
    wasm_engine_t *engine;
    wasm_store_t *first_store;
    wasm_store_t *second_store;
    wasm_store_t *third_store;
    wasm_store_t *fourth_store;
    wasm_byte_vec_t binary = { 0 };
    wasm_module_t *first_module;
    wasm_module_t *second_module;
    wasm_module_t *third_module;
    wasm_module_t *fourth_module;
    QuotaState first_quota;
    QuotaState second_quota;
    LoadArgs2 first_args;
    LoadArgs2 second_args;
    WASMAllocationQuotaToken *first_token;
    WASMAllocationQuotaToken *second_token;

    engine = wasm_engine_new();
    CHECK(engine != NULL);
    first_store = wasm_store_new(engine);
    second_store = wasm_store_new(engine);
    third_store = wasm_store_new(engine);
    fourth_store = wasm_store_new(engine);
    CHECK(first_store != NULL);
    CHECK(second_store != NULL);
    CHECK(third_store != NULL);
    CHECK(fourth_store != NULL);
    wasm_byte_vec_new(&binary, sizeof(empty_module),
                      (const byte_t *)empty_module);
    CHECK(binary.data != NULL);

    quota_init(&first_quota);
    first_args = quota_load_args(&first_quota);
    first_args.clone_wasm_binary = 1;
    first_token = wasm_allocation_quota_token_create(&first_args);
    CHECK(first_token != NULL);
    CHECK(wasm_allocation_quota_set_current(first_token) == NULL);
    first_module = wasm_module_new_ex2(first_store, &binary, &first_args);
    second_module = wasm_module_new_ex2(second_store, &binary, &first_args);
    CHECK(first_module != NULL);
    CHECK(second_module == first_module);
    CHECK(wasm_allocation_quota_set_current(NULL) == first_token);
    wasm_allocation_quota_token_release(first_token);
    CHECK(first_quota.attachment_retain_calls == 1);

    quota_init(&second_quota);
    second_args = quota_load_args(&second_quota);
    second_args.clone_wasm_binary = 1;
    second_token = wasm_allocation_quota_token_create(&second_args);
    CHECK(second_token != NULL);
    CHECK(wasm_allocation_quota_set_current(second_token) == NULL);
    third_module = wasm_module_new_ex2(third_store, &binary, &second_args);
    fourth_module = wasm_module_new_ex2(fourth_store, &binary, &second_args);
    CHECK(third_module != NULL);
    CHECK(third_module != first_module);
    CHECK(fourth_module == third_module);
    CHECK(wasm_allocation_quota_set_current(NULL) == second_token);
    wasm_allocation_quota_token_release(second_token);
    CHECK(second_quota.attachment_retain_calls == 1);

    wasm_byte_vec_delete(&binary);
    wasm_store_delete(fourth_store);
    wasm_store_delete(third_store);
    wasm_store_delete(second_store);
    wasm_store_delete(first_store);
    CHECK(first_quota.attachment_release_calls == 0);
    CHECK(second_quota.attachment_release_calls == 0);
    wasm_engine_delete(engine);
    CHECK(first_quota.attachment_release_calls == 1);
    CHECK(second_quota.attachment_release_calls == 1);
    check_balanced(&first_quota);
    check_balanced(&second_quota);
    quota_destroy(&second_quota);
    quota_destroy(&first_quota);
}
#endif

int
main(void)
{
    CHECK(wasm_runtime_init());
    test_legacy_load_args_remain_usable();
    test_version_validation_happens_before_quota_callbacks();
    test_low_cap_fails_before_first_module_allocation();
    test_successful_load_retains_one_owner_until_unload();
    test_nested_owner_mismatch_fails_closed();
#if WASM_ENABLE_MULTI_MODULE != 0
    test_multi_module_dependency_owner_mismatch_is_denied();
    test_multi_module_dependency_same_owner_reuses_and_refunds();
    test_multi_module_resolver_stays_bound_to_checked_dependency();
#endif
    wasm_runtime_destroy();
    test_c_api_versioned_load_owns_wrapper_graph();
#if WASM_ENABLE_WASM_CACHE != 0
    test_c_api_cache_reuses_only_the_current_owner();
#endif
    puts("allocation quota load checks passed");
    return 0;
}
