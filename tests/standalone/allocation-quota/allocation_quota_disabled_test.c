/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* A build without WAMR_BUILD_ALLOCATION_QUOTA keeps the stock allocator and
 * refuses loader settings that ask for a quota, before any callback runs. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            fprintf(stderr,                                             \
                    "allocation quota disabled check failed at %s:%d: " \
                    "%s\n",                                             \
                    __FILE__, __LINE__, #condition);                    \
            abort();                                                    \
        }                                                               \
    } while (0)

static const uint8_t empty_module[] = { 0x00, 0x61, 0x73, 0x6d,
                                        0x01, 0x00, 0x00, 0x00 };

static uint32_t callback_calls;

static bool
quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    (void)attachment;
    (void)bytes;
    (void)allocations;
    callback_calls++;
    return true;
}

static void
quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    (void)attachment;
    (void)bytes;
    (void)allocations;
    callback_calls++;
}

static bool
quota_attachment_retain(void *attachment)
{
    (void)attachment;
    callback_calls++;
    return true;
}

static void
quota_attachment_release(void *attachment)
{
    (void)attachment;
    callback_calls++;
}

int
main(void)
{
    uint8_t binary[sizeof(empty_module)];
    char error_buf[160] = { 0 };
    LoadArgs2 args;
    wasm_module_t module;
    void *block;
    int attachment;

    CHECK(wasm_runtime_init());

    block = wasm_runtime_malloc(64);
    CHECK(block != NULL);
    memset(block, 0xa5, 64);
    block = wasm_runtime_realloc(block, 4096);
    CHECK(block != NULL);
    CHECK(((uint8_t *)block)[63] == 0xa5);
    wasm_runtime_free(block);

    memcpy(binary, empty_module, sizeof(binary));
    wasm_runtime_load_args2_init(&args);
    args.name = "quota-disabled";
    module = wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                   error_buf, (uint32_t)sizeof(error_buf));
    CHECK(module != NULL);
    wasm_runtime_unload(module);

    wasm_runtime_load_args2_set_allocation_quota(
        &args, &attachment, quota_reserve, quota_release,
        quota_attachment_retain, quota_attachment_release);
    memcpy(binary, empty_module, sizeof(binary));
    CHECK(wasm_runtime_load_ex2(binary, (uint32_t)sizeof(binary), &args,
                                error_buf, (uint32_t)sizeof(error_buf))
          == NULL);
    CHECK(strstr(error_buf, "not enabled in this build") != NULL);
    CHECK(callback_calls == 0);

    CHECK(wasm_runtime_allocation_quota_token_create(&args) == NULL);
    CHECK(wasm_runtime_allocation_quota_token_set_current(NULL) == NULL);
    CHECK(callback_calls == 0);

    wasm_runtime_destroy();
    puts("allocation quota disabled-build checks passed");
    return 0;
}
