/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if TEST_HEADER_MODE == 1
#include "wasm_export.h"
#elif TEST_HEADER_MODE == 2
#include "wasm_c_api.h"
#elif TEST_HEADER_MODE == 3
#include "wasm_export.h"
#include "wasm_c_api.h"
#elif TEST_HEADER_MODE == 4
#include "wasm_c_api.h"
#include "wasm_export.h"
#else
#error TEST_HEADER_MODE must select a public header or include order
#endif

#ifdef __cplusplus
#define LOAD_ARGS_STATIC_ASSERT static_assert
#else
#define LOAD_ARGS_STATIC_ASSERT _Static_assert
#endif

LOAD_ARGS_STATIC_ASSERT(sizeof(bool) == 1, "LoadArgs ABI requires byte bools");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, name) == 0,
                        "LoadArgs.name must remain first");

#if UINTPTR_MAX == UINT64_MAX
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, clone_wasm_binary) == 8,
                        "unexpected 64-bit clone_wasm_binary offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, is_component) == 11,
                        "unexpected 64-bit is_component offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment) == 16,
                        "unexpected 64-bit quota attachment offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_reserve) == 24,
                        "unexpected 64-bit reserve callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_release) == 32,
                        "unexpected 64-bit release callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment_retain)
                            == 40,
                        "unexpected 64-bit retain callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment_release)
                            == 48,
                        "unexpected 64-bit attachment release callback offset");
LOAD_ARGS_STATIC_ASSERT(sizeof(LoadArgs) == 56,
                        "unexpected 64-bit LoadArgs size");
#elif UINTPTR_MAX == UINT32_MAX
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, clone_wasm_binary) == 4,
                        "unexpected 32-bit clone_wasm_binary offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, is_component) == 7,
                        "unexpected 32-bit is_component offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment) == 8,
                        "unexpected 32-bit quota attachment offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_reserve) == 12,
                        "unexpected 32-bit reserve callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_release) == 16,
                        "unexpected 32-bit release callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment_retain)
                            == 20,
                        "unexpected 32-bit retain callback offset");
LOAD_ARGS_STATIC_ASSERT(offsetof(LoadArgs, allocation_quota_attachment_release)
                            == 24,
                        "unexpected 32-bit attachment release callback offset");
LOAD_ARGS_STATIC_ASSERT(sizeof(LoadArgs) == 28,
                        "unexpected 32-bit LoadArgs size");
#else
#error unsupported pointer width
#endif

static bool
reserve_quota(void *attachment, uint64_t bytes, uint32_t allocations)
{
    return attachment && bytes && allocations;
}

static void
release_quota(void *attachment, uint64_t bytes, uint32_t allocations)
{
    (void)attachment;
    (void)bytes;
    (void)allocations;
}

static bool
retain_attachment(void *attachment)
{
    return attachment != NULL;
}

static void
release_attachment(void *attachment)
{
    (void)attachment;
}

void
compile_load_args_callback_assignments(void)
{
    typedef void (*setter_t)(
        LoadArgs *, void *, wasm_allocation_quota_reserve_callback_t,
        wasm_allocation_quota_release_callback_t,
        wasm_allocation_quota_attachment_retain_callback_t,
        wasm_allocation_quota_attachment_release_callback_t);
    LoadArgs args;
    setter_t setter = wasm_runtime_load_args_set_allocation_quota;

    memset(&args, 0, sizeof(args));
    args.allocation_quota_reserve = reserve_quota;
    args.allocation_quota_reserve = NULL;
    args.allocation_quota_release = release_quota;
    args.allocation_quota_release = NULL;
    args.allocation_quota_attachment_retain = retain_attachment;
    args.allocation_quota_attachment_retain = NULL;
    args.allocation_quota_attachment_release = release_attachment;
    args.allocation_quota_attachment_release = NULL;
    setter(&args, &args, reserve_quota, release_quota, retain_attachment,
           release_attachment);
}
