/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_ALLOCATION_QUOTA_H
#define _WASM_ALLOCATION_QUOTA_H

#include <stddef.h>

#include "wasm_load_args.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WASMAllocationQuotaToken WASMAllocationQuotaToken;

#define WASM_ALLOCATION_HEADER_MAGIC UINT64_C(0x57414D52514F5441)

/* C99-compatible equivalent of C11 max_align_t for runtime-owned metadata. */
typedef union WASMAllocationMaxAlign {
    long double long_double_value;
    void *pointer_value;
    uint64_t uint64_value;
#if defined(__SIZEOF_INT128__)
    __int128 int128_value;
#endif
} WASMAllocationMaxAlign;

typedef struct WASMAllocationAlignmentProbe {
    char prefix;
    WASMAllocationMaxAlign alignment;
} WASMAllocationAlignmentProbe;

#define WASM_ALLOCATION_MAX_ALIGNMENT \
    ((uint32_t)offsetof(WASMAllocationAlignmentProbe, alignment))

typedef union WASMAllocationHeader {
    struct {
        uint64_t magic;
        void *raw_ptr;
        WASMAllocationQuotaToken *owner;
        uint32_t user_size;
        uint32_t raw_size;
        uint32_t alignment;
        bool explicitly_aligned;
    } data;
    WASMAllocationMaxAlign alignment;
} WASMAllocationHeader;

WASMAllocationQuotaToken *
wasm_allocation_quota_token_create(const LoadArgs *args);

void
wasm_allocation_quota_token_release(WASMAllocationQuotaToken *token);

bool
wasm_allocation_quota_token_retain(WASMAllocationQuotaToken *token);

bool
wasm_allocation_quota_token_reserve(WASMAllocationQuotaToken *token,
                                    uint64_t bytes, uint32_t allocations);

void
wasm_allocation_quota_token_refund(WASMAllocationQuotaToken *token,
                                   uint64_t bytes, uint32_t allocations);

WASMAllocationQuotaToken *
wasm_allocation_quota_get_current(void);

/*
 * Installs a borrowed owner and returns the prior owner for nested restoration.
 * The caller must keep each installed token retained until it is restored.
 */
WASMAllocationQuotaToken *
wasm_allocation_quota_set_current(WASMAllocationQuotaToken *token);

/* Includes and rounds all header/alignment space charged to the allocator. */
bool
wasm_allocation_header_calculate_size(uint32_t user_size, uint32_t alignment,
                                      uint32_t *raw_size);

void *
wasm_allocation_header_initialize(void *raw_ptr, uint32_t raw_size,
                                  uint32_t user_size, uint32_t alignment,
                                  bool explicitly_aligned,
                                  WASMAllocationQuotaToken *owner);

WASMAllocationHeader *
wasm_allocation_header_from_user(void *user_ptr);

#ifdef __cplusplus
}
#endif

#endif /* _WASM_ALLOCATION_QUOTA_H */
