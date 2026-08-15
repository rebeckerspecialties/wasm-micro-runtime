/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_LOAD_ARGS_H
#define _WASM_LOAD_ARGS_H

#include <stdbool.h>
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

/**
 * Loader settings shared by the runtime and WebAssembly C APIs.
 *
 * Keep every field unconditional so that clients and runtime libraries built
 * with different feature defines agree on the ABI. is_component is ignored
 * when component-model support is disabled.
 */
typedef struct LoadArgs {
    char *name;
    /* True by default for wasm_module_new_ex(), false elsewhere. */
    bool clone_wasm_binary;
    /* Allow the AOT/core-Wasm loader to copy fields from the input buffer. */
    bool wasm_binary_freeable;
    /* Defer symbol resolution until wasm_runtime_resolve_symbols(). */
    bool no_resolve;
    bool is_component;

    void *allocation_quota_attachment;
    wasm_allocation_quota_reserve_callback_t allocation_quota_reserve;
    wasm_allocation_quota_release_callback_t allocation_quota_release;
    wasm_allocation_quota_attachment_retain_callback_t
        allocation_quota_attachment_retain;
    wasm_allocation_quota_attachment_release_callback_t
        allocation_quota_attachment_release;
} LoadArgs;

#ifdef __cplusplus
}
#endif

#endif /* _WASM_LOAD_ARGS_H */
