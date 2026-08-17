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

#include "wasm_export.h"
#include "wasm_allocation_quota.h"
#include "component-model/wasm_canonical_abi.h"

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "component quota check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition);                       \
            abort();                                                       \
        }                                                                  \
    } while (0)

typedef struct Binary {
    uint8_t *data;
    uint32_t size;
} Binary;

typedef struct LoadedComponent {
    WASMComponent *component;
    uint8_t *binary;
} LoadedComponent;

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
    uint32_t denied_calls;
    uint32_t attachment_retain_calls;
    uint32_t attachment_release_calls;
    uint32_t deny_reserve_call;
} QuotaState;

static Binary
read_binary(const char *path)
{
    Binary binary = { 0 };
    FILE *file = fopen(path, "rb");
    long size;

    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    CHECK(size > 0 && (unsigned long)size <= UINT32_MAX);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    binary.data = malloc((size_t)size);
    CHECK(binary.data != NULL);
    CHECK(fread(binary.data, 1, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0);
    binary.size = (uint32_t)size;
    return binary;
}

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
    CHECK(quota->reserved_bytes == quota->released_bytes);
    CHECK(quota->reserved_allocations == quota->released_allocations);
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
        quota->reserve_calls != quota->deny_reserve_call
        && quota->live_bytes <= quota->byte_limit
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
    args.name = "component-quota-test";
    args.is_component = 1;
    wasm_runtime_load_args2_set_allocation_quota(
        &args, quota, quota_reserve, quota_release, quota_attachment_retain,
        quota_attachment_release);
    return args;
}

static LoadedComponent
load_component_copy(const Binary *source, const LoadArgs2 *args,
                    char *error_buf, uint32_t error_buf_size)
{
    LoadedComponent loaded = { 0 };

    loaded.binary = malloc(source->size);
    CHECK(loaded.binary != NULL);
    memcpy(loaded.binary, source->data, source->size);
    loaded.component = wasm_component_load_ex2(loaded.binary, source->size,
                                               args, error_buf, error_buf_size);
    return loaded;
}

static void
release_loaded_component(LoadedComponent *loaded)
{
    if (loaded->component)
        wasm_component_unload(loaded->component);
    free(loaded->binary);
    memset(loaded, 0, sizeof(*loaded));
}

static void
test_legacy_component_load(const Binary *source)
{
    uint8_t *copy = malloc(source->size);
    char error_buf[256] = { 0 };
    LoadArgs args = { 0 };
    WASMComponent *component;

    CHECK(copy != NULL);
    memcpy(copy, source->data, source->size);
    args.name = "legacy-component";
    component = wasm_component_load(copy, source->size, &args, error_buf,
                                    (uint32_t)sizeof(error_buf));
    CHECK(component != NULL);
    wasm_component_unload(component);
    free(copy);
}

static void
test_component_root_allocation_is_bounded(const Binary *source)
{
    char error_buf[256] = { 0 };
    QuotaState quota;
    LoadArgs2 args;
    LoadedComponent loaded;

    quota_init(&quota);
    quota.allocation_limit = 1;
    args = quota_load_args(&quota);
    loaded = load_component_copy(source, &args, error_buf,
                                 (uint32_t)sizeof(error_buf));
    CHECK(loaded.component == NULL);
    CHECK(strstr(error_buf, "allocate component") != NULL);
    CHECK(quota.denied_calls >= 1);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 1);
    release_loaded_component(&loaded);
    quota_destroy(&quota);
}

static uint32_t
test_component_graph_uses_one_owner(const Binary *source)
{
    char error_buf[256] = { 0 };
    QuotaState quota;
    LoadArgs2 args;
    LoadedComponent loaded;
    uint32_t reserve_calls;

    quota_init(&quota);
    args = quota_load_args(&quota);
    loaded = load_component_copy(source, &args, error_buf,
                                 (uint32_t)sizeof(error_buf));
    if (!loaded.component)
        fprintf(stderr, "component load failed: %s\n", error_buf);
    CHECK(loaded.component != NULL);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 0);
    CHECK(quota.live_allocations > 1);
    CHECK(quota.reserved_allocations > quota.live_allocations);
    reserve_calls = quota.reserve_calls;

    release_loaded_component(&loaded);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);
    return reserve_calls;
}

static void
test_validation_maps_fail_closed(const Binary *source,
                                 uint32_t successful_reserve_calls)
{
    uint32_t deny_call;
    bool found_map_denial = false;
    bool found_entry_denial = false;

    for (deny_call = 1; deny_call <= successful_reserve_calls; deny_call++) {
        char error_buf[256] = { 0 };
        QuotaState quota;
        LoadArgs2 args;
        LoadedComponent loaded;

        quota_init(&quota);
        quota.deny_reserve_call = deny_call;
        args = quota_load_args(&quota);
        loaded = load_component_copy(source, &args, error_buf,
                                     (uint32_t)sizeof(error_buf));
        if (!loaded.component && strstr(error_buf, "hashmap"))
            found_map_denial = true;
        if (!loaded.component
            && (strstr(error_buf, "record component")
                || strstr(error_buf, "record resource"))) {
            found_entry_denial = true;
        }
        release_loaded_component(&loaded);
        CHECK(quota.denied_calls == 1);
        CHECK(quota.attachment_retain_calls == 1);
        CHECK(quota.attachment_release_calls == 1);
        quota_destroy(&quota);

        if (found_map_denial && found_entry_denial)
            break;
    }

    CHECK(found_map_denial);
    CHECK(found_entry_denial);
}

static void
test_payloadless_variant_refunds(void)
{
    QuotaState quota;
    LoadArgs2 args;
    WASMAllocationQuotaToken *token;
    wit_value_t variant;
    uint64_t baseline_bytes;
    uint32_t baseline_allocations;

    quota_init(&quota);
    args = quota_load_args(&quota);
    token = wasm_allocation_quota_token_create(&args);
    CHECK(token != NULL);
    baseline_bytes = quota.live_bytes;
    baseline_allocations = quota.live_allocations;

    CHECK(wasm_allocation_quota_set_current(token) == NULL);
    variant = wit_variant_ctor("closed", 6, NULL);
    CHECK(variant != NULL);
    CHECK(quota.live_allocations == baseline_allocations + 2);
    CHECK(wasm_allocation_quota_set_current(NULL) == token);

    CHECK(wasm_allocation_quota_set_current(token) == NULL);
    CHECK(free_wit_value(variant));
    CHECK(wasm_allocation_quota_set_current(NULL) == token);
    CHECK(quota.live_bytes == baseline_bytes);
    CHECK(quota.live_allocations == baseline_allocations);

    wasm_allocation_quota_token_release(token);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);
}

int
main(int argc, char **argv)
{
    Binary simple;
    Binary validation_heavy;
    Binary nested;
    uint32_t validation_reserve_calls;

    CHECK(argc == 4);
    simple = read_binary(argv[1]);
    validation_heavy = read_binary(argv[2]);
    nested = read_binary(argv[3]);
    CHECK(wasm_runtime_init());

    test_legacy_component_load(&simple);
    test_component_root_allocation_is_bounded(&simple);
    validation_reserve_calls =
        test_component_graph_uses_one_owner(&validation_heavy);
    test_validation_maps_fail_closed(&validation_heavy,
                                     validation_reserve_calls);
    (void)test_component_graph_uses_one_owner(&nested);
    test_payloadless_variant_refunds();

    wasm_runtime_destroy();
    free(nested.data);
    free(validation_heavy.data);
    free(simple.data);
    puts("allocation quota component load checks passed");
    return 0;
}
