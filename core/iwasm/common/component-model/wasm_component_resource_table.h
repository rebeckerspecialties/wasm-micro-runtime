/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RESOURCE_TABLE_H
#define WASM_COMPONENT_RESOURCE_TABLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_COMPONENT_TABLE_MAX_LENGTH ((1u << 28) - 1)

typedef struct WASMResourceHandle WASMResourceHandle;
typedef struct Task Task;
typedef struct WASMComponentTableTransaction WASMComponentTableTransaction;

typedef enum WASMTableElementType {
    WASM_TABLE_ELEM_RESOURCE_HANDLE = 1,
    WASM_TABLE_ELEM_ERROR_CONTEXT = 2,
} WASMTableElementType;

typedef struct WASMTableElement {
    WASMTableElementType type;
    void *ptr;
    WASMComponentTableTransaction *transaction;
    struct WASMTableElement *transaction_next;
    uint32_t transaction_index;
} WASMTableElement;

typedef struct WASMComponentResourceTable {
    WASMTableElement **array;
    uint32_t *free_list;
    uint32_t array_size;
    uint32_t free_count;
    uint32_t next_index; // Should start at 1
    uint32_t resize_percent;
    bool sealed;
} WASMComponentResourceTable;

typedef enum WASMComponentTableTransactionMode {
    WASM_COMPONENT_TABLE_TRANSACTION_LIFT = 1,
    WASM_COMPONENT_TABLE_TRANSACTION_LOWER = 2,
} WASMComponentTableTransactionMode;

struct WASMComponentTableTransaction {
    WASMComponentResourceTable *table;
    WASMTableElement *head;
    uint32_t count;
    WASMComponentTableTransactionMode mode;
    bool active;
};

typedef struct WASMComponentTableTransactionSavepoint {
    WASMComponentTableTransaction *transaction;
    WASMTableElement *head;
    uint32_t count;
    bool active;
} WASMComponentTableTransactionSavepoint;

bool
wasm_component_table_add(WASMComponentResourceTable *table, void *ptr,
                         WASMTableElementType type, uint32_t *out);

/** Remove an element without dropping its underlying representation. */
bool
wasm_component_table_remove(WASMComponentResourceTable *table, uint32_t index);

/**
 * Remove and drop a resource handle. Unlike wasm_component_table_remove(),
 * this destroys an owned representation and is therefore not suitable for an
 * ownership transfer.
 */
bool
wasm_component_table_drop_resource(WASMComponentResourceTable *table,
                                   uint32_t index);

void *
wasm_component_table_get(WASMComponentResourceTable *table, uint32_t index,
                         WASMTableElementType expected_type);

WASMComponentResourceTable *
wasm_component_table_init(uint32_t initial_size, uint32_t resize_percent);

/** Prevent any later add, free-list reuse, or growth. Idempotent. */
void
wasm_component_table_seal(WASMComponentResourceTable *table);

bool
wasm_component_table_destroy(WASMComponentResourceTable *table);

/**
 * Begin an allocation-free canonical ownership transaction. Lift
 * transactions reserve existing owned handles until the complete lifted
 * value succeeds. Lower transactions tentatively register newly-added table
 * elements until the complete lowered value succeeds.
 */
bool
wasm_component_table_transaction_begin(
    WASMComponentTableTransaction *transaction,
    WASMComponentResourceTable *table, WASMComponentTableTransactionMode mode);

/** Reserve an existing owned resource handle for a lift transaction. */
bool
wasm_component_table_transaction_reserve_lift(
    WASMComponentTableTransaction *transaction, uint32_t index);

/** Record an element just added by the active lower transaction. */
bool
wasm_component_table_transaction_record_lower(
    WASMComponentTableTransaction *transaction, uint32_t index);

/** Preflight a transaction; no table state is changed. */
bool
wasm_component_table_transaction_can_commit(
    WASMComponentTableTransaction *transaction);

/**
 * Capture the current end of an active lower transaction.  Committing a
 * savepoint keeps subsequently recorded handles in the outer transaction;
 * rolling it back removes only those handles.  Both operations are
 * allocation-free and leave the outer transaction active.
 */
bool
wasm_component_table_transaction_savepoint_begin(
    WASMComponentTableTransaction *transaction,
    WASMComponentTableTransactionSavepoint *savepoint);

bool
wasm_component_table_transaction_savepoint_can_commit(
    const WASMComponentTableTransactionSavepoint *savepoint);

bool
wasm_component_table_transaction_savepoint_commit(
    WASMComponentTableTransactionSavepoint *savepoint);

bool
wasm_component_table_transaction_savepoint_rollback(
    WASMComponentTableTransactionSavepoint *savepoint);

/* Replace the placeholder representation in the sole tentative owned handle.
 * This lets a host pre-lower an unpublished handle before a native side effect
 * creates the real representation. */
bool
wasm_component_table_transaction_rebind_single_owned_rep(
    WASMComponentTableTransaction *transaction, uint32_t expected_rep,
    uint32_t replacement_rep);

/** Commit all ownership changes atomically after a complete canonical value. */
bool
wasm_component_table_transaction_commit(
    WASMComponentTableTransaction *transaction);

/** Undo all ownership changes in reverse order without dropping reps. */
bool
wasm_component_table_transaction_rollback(
    WASMComponentTableTransaction *transaction);

/** Drop representations produced by a callee but not published to its caller.
 */
bool
wasm_component_table_transaction_discard_lift(
    WASMComponentTableTransaction *transaction);

/** Remove every temporary borrow owned by a failed or completed task. */
bool
wasm_component_table_remove_task_borrows(WASMComponentResourceTable *table,
                                         Task *task);

#ifdef __cplusplus
}
#endif

#endif
