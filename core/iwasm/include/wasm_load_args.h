/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_LOAD_ARGS_H
#define _WASM_LOAD_ARGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-owned quota for native allocations retained by a loaded module and by
 * objects derived from it. Bytes passed to these callbacks are the exact byte
 * counts requested from WAMR's configured native allocator, including any
 * ownership header and alignment padding. The allocation count is the number
 * of live logical allocations; a realloc changes bytes but not the count.
 *
 * All callbacks must be non-allocating, reentrancy-safe, and thread-safe. A
 * successful attachment retain is paired with exactly one attachment release
 * after the final charged allocation is actually freed. Retained module
 * graphs therefore retain both their charge and their attachment.
 */
typedef bool (*wasm_allocation_quota_reserve_callback_t)(void *attachment,
                                                         uint64_t bytes,
                                                         uint32_t allocations);
typedef void (*wasm_allocation_quota_release_callback_t)(void *attachment,
                                                         uint64_t bytes,
                                                         uint32_t allocations);
typedef bool (*wasm_allocation_quota_attachment_retain_callback_t)(
    void *attachment);
typedef void (*wasm_allocation_quota_attachment_release_callback_t)(
    void *attachment);

/*
 * Keep the legacy loader settings ABI unchanged.  LoadArgs has no size or
 * version field, so new runtimes must never read fields beyond this layout.
 */
#ifndef LOAD_ARGS_OPTION_DEFINED
#define LOAD_ARGS_OPTION_DEFINED
typedef struct LoadArgs {
    char *name;
    /* This option is only used by the Wasm C API (see wasm_c_api.h). */
    bool clone_wasm_binary;
    /* This option is only used by the AOT/wasm loader. */
    bool wasm_binary_freeable;
    /* Defer symbol resolution until wasm_runtime_resolve_symbols(). */
    bool no_resolve;
#if WASM_ENABLE_COMPONENT_MODEL != 0
    bool is_component;
#endif
} LoadArgs;
#endif /* LOAD_ARGS_OPTION_DEFINED */

/**
 * Versioned loader settings.  Unlike LoadArgs, this layout is independent of
 * WAMR feature defines.  Call wasm_runtime_load_args2_init() before assigning
 * fields.  V1 callers must set struct_size to sizeof(LoadArgs2) and
 * abi_version to WASM_LOAD_ARGS2_ABI_VERSION_1.
 */
#define WASM_LOAD_ARGS2_ABI_VERSION_1 UINT32_C(1)
typedef struct LoadArgs2 {
    uint32_t struct_size;
    uint32_t abi_version;
    char *name;
    uint8_t clone_wasm_binary;
    uint8_t wasm_binary_freeable;
    uint8_t no_resolve;
    uint8_t is_component;
    uint8_t reserved[4];
    void *allocation_quota_attachment;
    wasm_allocation_quota_reserve_callback_t allocation_quota_reserve;
    wasm_allocation_quota_release_callback_t allocation_quota_release;
    wasm_allocation_quota_attachment_retain_callback_t
        allocation_quota_attachment_retain;
    wasm_allocation_quota_attachment_release_callback_t
        allocation_quota_attachment_release;
} LoadArgs2;

#define WASM_LOAD_ARGS2_SIZE_V1                                          \
    ((uint32_t)(offsetof(LoadArgs2, allocation_quota_attachment_release) \
                + sizeof(                                                \
                    wasm_allocation_quota_attachment_release_callback_t)))

#ifdef __cplusplus
}
#endif

#endif /* _WASM_LOAD_ARGS_H */
