/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <string.h>

#include "wasm_export.h"

extern bool
wasm_runtime_load_args_has_valid_allocation_quota(const LoadArgs *args);

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            fprintf(stderr, "load-args validation failed at line %d\n", \
                    __LINE__);                                          \
            return 1;                                                   \
        }                                                               \
    } while (0)

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

int
main(void)
{
    LoadArgs args;
    int attachment = 0;

    memset(&args, 0, sizeof(args));
    CHECK(wasm_runtime_load_args_has_valid_allocation_quota(&args));

    args.allocation_quota_attachment = &attachment;
    CHECK(!wasm_runtime_load_args_has_valid_allocation_quota(&args));

    memset(&args, 0, sizeof(args));
    args.allocation_quota_reserve = reserve_quota;
    CHECK(!wasm_runtime_load_args_has_valid_allocation_quota(&args));

    memset(&args, 0, sizeof(args));
    args.allocation_quota_release = release_quota;
    CHECK(!wasm_runtime_load_args_has_valid_allocation_quota(&args));

    memset(&args, 0, sizeof(args));
    args.allocation_quota_attachment_retain = retain_attachment;
    CHECK(!wasm_runtime_load_args_has_valid_allocation_quota(&args));

    memset(&args, 0, sizeof(args));
    args.allocation_quota_attachment_release = release_attachment;
    CHECK(!wasm_runtime_load_args_has_valid_allocation_quota(&args));

    wasm_runtime_load_args_set_allocation_quota(
        &args, &attachment, reserve_quota, release_quota, retain_attachment,
        release_attachment);
    CHECK(args.allocation_quota_attachment == &attachment);
    CHECK(args.allocation_quota_reserve == reserve_quota);
    CHECK(args.allocation_quota_release == release_quota);
    CHECK(args.allocation_quota_attachment_retain == retain_attachment);
    CHECK(args.allocation_quota_attachment_release == release_attachment);
    CHECK(wasm_runtime_load_args_has_valid_allocation_quota(&args));

    wasm_runtime_load_args_set_allocation_quota(&args, NULL, NULL, NULL, NULL,
                                                NULL);
    CHECK(wasm_runtime_load_args_has_valid_allocation_quota(&args));
    wasm_runtime_load_args_set_allocation_quota(NULL, NULL, NULL, NULL, NULL,
                                                NULL);

    return 0;
}
