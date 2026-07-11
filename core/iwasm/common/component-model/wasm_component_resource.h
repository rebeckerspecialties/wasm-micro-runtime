/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RESOURCE_H
#define WASM_COMPONENT_RESOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WASMComponentResourceInstance WASMComponentResourceInstance;
typedef struct Task Task;
typedef struct Subtask Subtask;

typedef struct WASMResourceHandle {
    WASMComponentResourceInstance *rt;
    uint32_t rep;
    bool own;
    Task *borrow_scope;
    uint32_t num_lends;
} WASMResourceHandle;

WASMResourceHandle *
wasm_create_resource_handle(WASMComponentResourceInstance *rt, uint32_t rep,
                            bool own);

/** Release only the handle wrapper without dropping its representation. */
void
wasm_destroy_resource_handle(WASMResourceHandle *handle);

/**
 * Drop a handle from a component resource table. Owned handles destroy their
 * underlying representation before the handle wrapper is released. Borrowed
 * handles only release the wrapper; their task bookkeeping remains the
 * caller's responsibility.
 *
 * The handle wrapper is released even when representation teardown fails.
 */
bool
wasm_drop_resource_handle(WASMResourceHandle *handle);

static inline uint32_t
wasm_resource_handle_get_rep_i32(const WASMResourceHandle *handle)
{
    if (!handle || handle->rep < 1) {
        return 0;
    }
    return handle->rep;
}

#ifdef __cplusplus
}
#endif

#endif
