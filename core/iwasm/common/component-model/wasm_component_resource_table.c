/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource_table.h"
#include "wasm_component_resource.h"
#include "wasm_component_task.h"
#include "wasm_runtime_common.h"
#include "wasm_allocation_quota.h"
#include "bh_log.h"
#include <string.h>

WASMComponentResourceTable *
wasm_component_table_init(uint32_t initial_size, uint32_t resize_percent)
{
    if (initial_size == 0 || initial_size > WASM_COMPONENT_TABLE_MAX_LENGTH
        || resize_percent == 0) {
        return NULL;
    }

    WASMComponentResourceTable *table =
        wasm_runtime_malloc(sizeof(WASMComponentResourceTable));
    if (!table) {
        return NULL;
    }

    // Initialize table - start with index 0 reserved
    table->array_size = (initial_size < 1) ? 1 : initial_size;
    table->array = (WASMTableElement **)wasm_runtime_malloc(
        sizeof(WASMTableElement *) * table->array_size);
    table->free_list =
        wasm_runtime_malloc(sizeof(uint32_t) * table->array_size);

    if (!table->array || !table->free_list) {
        if (table->array)
            wasm_runtime_free((void *)table->array);
        if (table->free_list)
            wasm_runtime_free(table->free_list);
        wasm_runtime_free(table);
        return NULL;
    }
    if (!wasm_allocation_quota_allocations_share_owner(table, table->array)
        || !wasm_allocation_quota_allocations_share_owner(table,
                                                          table->free_list)) {
        wasm_runtime_free((void *)table->array);
        wasm_runtime_free(table->free_list);
        wasm_runtime_free(table);
        return NULL;
    }

    // Initialize both arrays to zero
    memset((void *)table->array, 0,
           sizeof(WASMTableElement *) * table->array_size);
    memset(table->free_list, 0, sizeof(uint32_t) * table->array_size);

    // Initialize free list state
    table->free_count = 0;
    table->next_index = 1;
    table->resize_percent = resize_percent;
    table->sealed = false;

    return table;
}

void
wasm_component_table_seal(WASMComponentResourceTable *table)
{
    WASMAllocationQuotaBorrowedScope quota_scope;

    if (!table
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return;
    }

    table->sealed = true;
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
}

bool
wasm_component_table_destroy(WASMComponentResourceTable *table)
{
    WASMAllocationQuotaScope quota_scope;
    bool success = false;

    if (!table)
        return false;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                          table)) {
        LOG_ERROR("component table: allocation quota owner scope could not be "
                  "established for destroy");
        return false;
    }

    /* No destructor may repopulate this table or reuse a cleared slot. */
    table->sealed = true;

    /* Validate the complete table before invoking any destructor so an owner
     * mismatch cannot leave a partially destroyed resource table. */
    for (uint32_t i = 1; i < table->next_index; i++) {
        WASMTableElement *elem = table->array[i];

        if (elem
            && (elem->transaction
                || !wasm_allocation_quota_allocations_share_owner(table, elem)
                || (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr
                    && !wasm_allocation_quota_allocations_share_owner(
                        table, elem->ptr)))) {
            LOG_ERROR("component table: active transaction or resource owner "
                      "mismatch during destroy");
            goto done;
        }
    }

    // Clean up all elements first
    for (uint32_t i = 1; i < table->next_index; i++) {
        if (table->array[i]) {
            WASMTableElement *elem = table->array[i];

            /* Clear the slot before calling user-defined destructor code so a
             * reentrant lookup cannot observe a half-dropped handle. */
            table->array[i] = NULL;

            // Destroy the underlying object based on type
            if (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr) {
                (void)wasm_drop_resource_handle(
                    (WASMResourceHandle *)elem->ptr);
            }
            // TODO: Add destructors for other element types

            wasm_runtime_free(elem);
        }
    }

    if (table->array)
        wasm_runtime_free((void *)table->array);
    if (table->free_list)
        wasm_runtime_free((void *)table->free_list);
    wasm_runtime_free((void *)table);
    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

void *
wasm_component_table_get(WASMComponentResourceTable *table, uint32_t index,
                         WASMTableElementType expected_type)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMTableElement *elem;
    void *result = NULL;

    if (!table
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return NULL;
    }

    if (index >= table->next_index)
        goto done;

    if (table->array[index] == NULL)
        goto done;

    elem = table->array[index];
    if (elem->transaction || elem->type != expected_type)
        goto done;
    if (!wasm_allocation_quota_allocations_share_owner(table, elem)
        || (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE
            && (!elem->ptr
                || !wasm_allocation_quota_allocations_share_owner(
                    table, elem->ptr)))) {
        LOG_ERROR("component table: element owner mismatch during lookup");
        goto done;
    }
    result = elem->ptr;

done:
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return result;
}

bool
wasm_component_table_remove(WASMComponentResourceTable *table, uint32_t index)
{
    WASMAllocationQuotaScope quota_scope;
    WASMTableElement *elem;
    bool success = false;

    if (!table)
        return false;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                          table)) {
        return false;
    }

    if (index >= table->next_index || index == 0
        || table->array[index] == NULL) {
        goto done;
    }

    if (table->free_count >= table->array_size) {
        goto done; // Should not happen
    }

    elem = table->array[index];
    if (elem->transaction
        || !wasm_allocation_quota_allocations_share_owner(table, elem)
        || (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr
            && !wasm_allocation_quota_allocations_share_owner(table,
                                                              elem->ptr))) {
        LOG_ERROR("component table: resource owner mismatch during remove");
        goto done;
    }

    /* Detach first. wasm_component_table_remove() transfers ownership and must
     * only destroy the table's handle wrapper, never the representation. */
    table->array[index] = NULL;
    table->free_list[table->free_count++] = index;

    // Destroy the underlying object based on type
    if (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr) {
        wasm_destroy_resource_handle((WASMResourceHandle *)elem->ptr);
    }
    // TODO: Add destructors for other element types

    wasm_runtime_free(elem);

    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_drop_resource(WASMComponentResourceTable *table,
                                   uint32_t index)
{
    WASMAllocationQuotaScope quota_scope;
    WASMTableElement *elem;
    bool success = false;

    if (!table
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }
    if (index == 0 || index >= table->next_index || table->array[index] == NULL
        || table->array[index]->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
        || table->free_count >= table->array_size) {
        goto done;
    }

    elem = table->array[index];
    if (elem->transaction
        || !wasm_allocation_quota_allocations_share_owner(table, elem)
        || !elem->ptr
        || !wasm_allocation_quota_allocations_share_owner(table, elem->ptr)) {
        LOG_ERROR("component table: resource owner mismatch during drop");
        goto done;
    }

    /* Commit the removal before invoking destructor code. Even when teardown
     * reports an error, this handle must not be dropped a second time. */
    table->array[index] = NULL;
    table->free_list[table->free_count++] = index;

    success = wasm_drop_resource_handle((WASMResourceHandle *)elem->ptr);
    wasm_runtime_free(elem);

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

static bool
wasm_component_table_resize(WASMComponentResourceTable *table)
{
    if (!table || table->sealed)
        return false;

    /* Calculate in 64 bits so a hostile resize percentage cannot wrap. */
    uint64_t growth = (uint64_t)table->array_size * table->resize_percent / 100;
    uint64_t requested_size = (uint64_t)table->array_size + growth;

    // Ensure minimum growth (at least 1 more slot)
    if (requested_size <= table->array_size) {
        requested_size = (uint64_t)table->array_size + 1;
    }

    if (requested_size > WASM_COMPONENT_TABLE_MAX_LENGTH) {
        return false;
    }

    uint32_t new_size = (uint32_t)requested_size;
    uint32_t array_bytes = (uint32_t)(sizeof(WASMTableElement *) * new_size);
    uint32_t free_list_bytes = (uint32_t)(sizeof(uint32_t) * new_size);

    /* Allocate-copy-commit keeps both old buffers valid unless the complete
     * resize succeeds. Two realloc calls cannot provide that guarantee. */
    WASMTableElement **new_array =
        (WASMTableElement **)wasm_runtime_malloc(array_bytes);
    uint32_t *new_free_list = wasm_runtime_malloc(free_list_bytes);

    if (!new_array || !new_free_list) {
        if (new_array) {
            wasm_runtime_free((void *)new_array);
        }
        if (new_free_list) {
            wasm_runtime_free(new_free_list);
        }
        return false;
    }

    memcpy((void *)new_array, (const void *)table->array,
           sizeof(WASMTableElement *) * table->array_size);
    memcpy(new_free_list, table->free_list,
           sizeof(uint32_t) * table->array_size);
    memset((void *)&new_array[table->array_size], 0,
           sizeof(WASMTableElement *) * (new_size - table->array_size));
    memset(&new_free_list[table->array_size], 0,
           sizeof(uint32_t) * (new_size - table->array_size));

    wasm_runtime_free((void *)table->array);
    wasm_runtime_free(table->free_list);
    table->array = new_array;
    table->free_list = new_free_list;
    table->array_size = new_size;

    return true;
}

bool
wasm_component_table_add(WASMComponentResourceTable *table, void *ptr,
                         WASMTableElementType type, uint32_t *out_index)
{
    WASMAllocationQuotaScope quota_scope;
    WASMTableElement *elem = NULL;
    uint32_t index = 0;
    bool success = false;

    if (!table || !ptr || !out_index)
        return false;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                          table)) {
        return false;
    }
    if (table->sealed) {
        goto done;
    }
    if (type == WASM_TABLE_ELEM_RESOURCE_HANDLE
        && !wasm_allocation_quota_allocations_share_owner(table, ptr)) {
        LOG_ERROR("component table: refusing a resource handle with a "
                  "different allocation quota owner");
        goto done;
    }

    elem = wasm_runtime_malloc(sizeof(WASMTableElement));
    if (!elem)
        goto done;

    elem->type = type;
    elem->ptr = ptr;
    elem->transaction = NULL;
    elem->transaction_next = NULL;
    elem->transaction_index = 0;

    // Use free list first
    if (table->free_count > 0) {
        index = table->free_list[table->free_count - 1];
        table->free_count--;

        if (table->array[index] != NULL) {
            table->free_count++;
            goto done; // Should not happen
        }

        table->array[index] = elem;
    }
    else {
        // Grow array
        index = table->next_index;

        if (index > WASM_COMPONENT_TABLE_MAX_LENGTH) {
            goto done;
        }

        // Resize array if needed
        if (index >= table->array_size) {
            if (!wasm_component_table_resize(table)) {
                goto done;
            }
        }

        table->array[index] = elem;
        table->next_index++;
    }

    *out_index = index;
    elem = NULL;
    success = true;

done:
    if (elem) {
        wasm_runtime_free(elem);
    }
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

static bool
table_element_owner_is_valid(WASMComponentResourceTable *table,
                             WASMTableElement *elem)
{
    return table && elem
           && wasm_allocation_quota_allocations_share_owner(table, elem)
           && (elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
               || (elem->ptr
                   && wasm_allocation_quota_allocations_share_owner(
                       table, elem->ptr)));
}

typedef struct WASMComponentTableBorrowSummary {
    Task *task;
    uint32_t count;
} WASMComponentTableBorrowSummary;

static bool
table_transaction_validate(WASMComponentTableTransaction *transaction,
                           bool removing,
                           WASMComponentTableBorrowSummary *borrow_summary)
{
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    WASMComponentTableBorrowSummary summary = { 0 };
    uint32_t visited = 0;

    if (!transaction || !transaction->active || !(table = transaction->table)
        || (transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LIFT
            && transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER)
        || table->free_count > table->array_size
        || transaction->count > table->array_size
        || (transaction->count == 0) != (transaction->head == NULL)
        || (removing
            && transaction->count > table->array_size - table->free_count)) {
        return false;
    }

    for (elem = transaction->head; elem; elem = elem->transaction_next) {
        uint32_t index;

        if (++visited > transaction->count || elem->transaction != transaction
            || (index = elem->transaction_index) == 0
            || index >= table->next_index || table->array[index] != elem
            || !table_element_owner_is_valid(table, elem)) {
            return false;
        }
        if (transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LIFT) {
            WASMResourceHandle *handle;

            if (elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
                || !(handle = (WASMResourceHandle *)elem->ptr) || !handle->own
                || handle->num_lends != 0) {
                return false;
            }
        }
        else if (removing && elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE) {
            WASMResourceHandle *handle = (WASMResourceHandle *)elem->ptr;

            if (!handle->own && handle->borrow_scope) {
                if (summary.task && summary.task != handle->borrow_scope) {
                    return false;
                }
                summary.task = handle->borrow_scope;
                if (summary.count == UINT32_MAX)
                    return false;
                summary.count++;
            }
        }
    }

    if (visited != transaction->count
        || (summary.task && summary.task->num_borrows < summary.count)) {
        return false;
    }
    if (borrow_summary)
        *borrow_summary = summary;
    return true;
}

static void
table_transaction_forget_marks(WASMComponentTableTransaction *transaction)
{
    WASMTableElement *elem;
    uint32_t remaining;

    if (!transaction)
        return;
    elem = transaction->head;
    remaining = transaction->count;
    while (elem && remaining-- > 0) {
        WASMTableElement *next = elem->transaction_next;

        if (elem->transaction == transaction) {
            elem->transaction = NULL;
            elem->transaction_next = NULL;
            elem->transaction_index = 0;
        }
        elem = next;
    }
    memset(transaction, 0, sizeof(*transaction));
}

bool
wasm_component_table_transaction_begin(
    WASMComponentTableTransaction *transaction,
    WASMComponentResourceTable *table, WASMComponentTableTransactionMode mode)
{
    if (!transaction || !table || table->sealed
        || (mode != WASM_COMPONENT_TABLE_TRANSACTION_LIFT
            && mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER)) {
        return false;
    }

    memset(transaction, 0, sizeof(*transaction));
    transaction->table = table;
    transaction->mode = mode;
    transaction->active = true;
    return true;
}

bool
wasm_component_table_transaction_reserve_lift(
    WASMComponentTableTransaction *transaction, uint32_t index)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    WASMResourceHandle *handle;
    bool success = false;

    if (!transaction || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LIFT
        || !(table = transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    if (index == 0 || index >= table->next_index
        || !(elem = table->array[index]) || elem->transaction
        || elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
        || !(handle = (WASMResourceHandle *)elem->ptr) || !handle->own
        || handle->num_lends != 0 || transaction->count == UINT32_MAX
        || !table_element_owner_is_valid(table, elem)) {
        goto done;
    }

    elem->transaction = transaction;
    elem->transaction_next = transaction->head;
    elem->transaction_index = index;
    transaction->head = elem;
    transaction->count++;
    success = true;

done:
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_record_lower(
    WASMComponentTableTransaction *transaction, uint32_t index)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    bool success = false;

    if (!transaction || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER
        || !(table = transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    if (index == 0 || index >= table->next_index
        || !(elem = table->array[index]) || elem->transaction
        || transaction->count == UINT32_MAX
        || !table_element_owner_is_valid(table, elem)) {
        goto done;
    }

    elem->transaction = transaction;
    elem->transaction_next = transaction->head;
    elem->transaction_index = index;
    transaction->head = elem;
    transaction->count++;
    success = true;

done:
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_can_commit(
    WASMComponentTableTransaction *transaction)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    bool success;

    if (!transaction || !transaction->active || !(table = transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    success = table_transaction_validate(
        transaction, transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LIFT,
        NULL);
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

static bool
table_transaction_savepoint_validate(
    const WASMComponentTableTransactionSavepoint *savepoint, bool removing,
    WASMComponentTableBorrowSummary *borrow_summary)
{
    WASMComponentTableTransaction *transaction;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    WASMComponentTableBorrowSummary summary = { 0 };
    uint32_t added_count;
    uint32_t visited = 0;

    if (!savepoint || !savepoint->active
        || !(transaction = savepoint->transaction) || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER
        || !(table = transaction->table)
        || savepoint->count > transaction->count
        || (savepoint->count == 0) != (savepoint->head == NULL)
        || !table_transaction_validate(transaction, false, NULL)) {
        return false;
    }

    added_count = transaction->count - savepoint->count;
    if (removing && added_count > table->array_size - table->free_count) {
        return false;
    }

    elem = transaction->head;
    while (visited < added_count) {
        WASMResourceHandle *handle;

        if (!elem || elem->transaction != transaction) {
            return false;
        }
        if (removing && elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE
            && (handle = (WASMResourceHandle *)elem->ptr) != NULL
            && !handle->own && handle->borrow_scope) {
            if (summary.task && summary.task != handle->borrow_scope) {
                return false;
            }
            summary.task = handle->borrow_scope;
            if (summary.count == UINT32_MAX) {
                return false;
            }
            summary.count++;
        }
        elem = elem->transaction_next;
        visited++;
    }
    if (elem != savepoint->head
        || (summary.task && summary.task->num_borrows < summary.count)) {
        return false;
    }
    if (borrow_summary) {
        *borrow_summary = summary;
    }
    return true;
}

bool
wasm_component_table_transaction_savepoint_begin(
    WASMComponentTableTransaction *transaction,
    WASMComponentTableTransactionSavepoint *savepoint)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    bool success = false;

    if (!transaction || !savepoint || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER
        || !(table = transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    memset(savepoint, 0, sizeof(*savepoint));
    if (table_transaction_validate(transaction, false, NULL)) {
        savepoint->transaction = transaction;
        savepoint->head = transaction->head;
        savepoint->count = transaction->count;
        savepoint->active = true;
        success = true;
    }
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_savepoint_can_commit(
    const WASMComponentTableTransactionSavepoint *savepoint)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    bool success;

    if (!savepoint || !savepoint->active || !savepoint->transaction
        || !(table = savepoint->transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    success = table_transaction_savepoint_validate(savepoint, false, NULL);
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_savepoint_commit(
    WASMComponentTableTransactionSavepoint *savepoint)
{
    if (!wasm_component_table_transaction_savepoint_can_commit(savepoint)) {
        return false;
    }
    memset(savepoint, 0, sizeof(*savepoint));
    return true;
}

bool
wasm_component_table_transaction_savepoint_rollback(
    WASMComponentTableTransactionSavepoint *savepoint)
{
    WASMAllocationQuotaScope quota_scope;
    WASMComponentTableTransaction *transaction;
    WASMComponentResourceTable *table;
    WASMComponentTableBorrowSummary borrow_summary = { 0 };
    WASMTableElement *elem;
    uint32_t added_count;
    bool success = false;

    if (!savepoint || !savepoint->active
        || !(transaction = savepoint->transaction)
        || !(table = transaction->table)
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }
    if (!table_transaction_savepoint_validate(savepoint, true,
                                              &borrow_summary)) {
        goto done;
    }

    added_count = transaction->count - savepoint->count;
    elem = transaction->head;
    while (added_count-- > 0) {
        WASMTableElement *next = elem->transaction_next;
        uint32_t index = elem->transaction_index;

        table->array[index] = NULL;
        table->free_list[table->free_count++] = index;
        elem->transaction = NULL;
        elem->transaction_next = NULL;
        elem->transaction_index = 0;
        if (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr) {
            wasm_destroy_resource_handle((WASMResourceHandle *)elem->ptr);
        }
        wasm_runtime_free(elem);
        elem = next;
    }
    transaction->head = savepoint->head;
    transaction->count = savepoint->count;
    if (borrow_summary.task) {
        borrow_summary.task->num_borrows -= borrow_summary.count;
    }
    memset(savepoint, 0, sizeof(*savepoint));
    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_rebind_single_owned_rep(
    WASMComponentTableTransaction *transaction, uint32_t expected_rep,
    uint32_t replacement_rep)
{
    WASMAllocationQuotaBorrowedScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    WASMResourceHandle *handle;
    bool success = false;

    if (!transaction || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LOWER
        || replacement_rep == 0 || !(table = transaction->table)
        || !wasm_allocation_quota_borrowed_scope_enter_for_allocation(
            &quota_scope, table)) {
        return false;
    }
    if (transaction->count != 1 || !(elem = transaction->head)
        || elem->transaction != transaction
        || elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
        || !(handle = (WASMResourceHandle *)elem->ptr) || !handle->own
        || handle->rep != expected_rep
        || !table_transaction_validate(transaction, false, NULL)) {
        goto done;
    }

    handle->rep = replacement_rep;
    success = true;

done:
    wasm_allocation_quota_borrowed_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_commit(
    WASMComponentTableTransaction *transaction)
{
    WASMAllocationQuotaScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    bool success = false;

    if (!transaction || !transaction->active || !(table = transaction->table)
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }
    if (!table_transaction_validate(
            transaction,
            transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LIFT, NULL)) {
        LOG_ERROR("component table: invalid ownership transaction commit");
        table_transaction_forget_marks(transaction);
        goto done;
    }

    if (transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LOWER) {
        table_transaction_forget_marks(transaction);
        success = true;
        goto done;
    }

    elem = transaction->head;
    while (elem) {
        WASMTableElement *next = elem->transaction_next;
        uint32_t index = elem->transaction_index;

        table->array[index] = NULL;
        table->free_list[table->free_count++] = index;
        elem->transaction = NULL;
        elem->transaction_next = NULL;
        elem->transaction_index = 0;
        wasm_destroy_resource_handle((WASMResourceHandle *)elem->ptr);
        wasm_runtime_free(elem);
        elem = next;
    }
    memset(transaction, 0, sizeof(*transaction));
    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_rollback(
    WASMComponentTableTransaction *transaction)
{
    WASMAllocationQuotaScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *elem;
    WASMComponentTableBorrowSummary borrow_summary = { 0 };
    bool success = false;

    if (!transaction || !transaction->active || !(table = transaction->table)
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }
    if (!table_transaction_validate(
            transaction,
            transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LOWER,
            &borrow_summary)) {
        LOG_ERROR("component table: invalid ownership transaction rollback");
        table_transaction_forget_marks(transaction);
        goto done;
    }

    if (transaction->mode == WASM_COMPONENT_TABLE_TRANSACTION_LIFT) {
        table_transaction_forget_marks(transaction);
        success = true;
        goto done;
    }

    elem = transaction->head;
    while (elem) {
        WASMTableElement *next = elem->transaction_next;
        uint32_t index = elem->transaction_index;

        table->array[index] = NULL;
        table->free_list[table->free_count++] = index;
        elem->transaction = NULL;
        elem->transaction_next = NULL;
        elem->transaction_index = 0;
        if (elem->type == WASM_TABLE_ELEM_RESOURCE_HANDLE && elem->ptr) {
            WASMResourceHandle *handle = (WASMResourceHandle *)elem->ptr;

            wasm_destroy_resource_handle(handle);
        }
        wasm_runtime_free(elem);
        elem = next;
    }
    if (borrow_summary.task)
        borrow_summary.task->num_borrows -= borrow_summary.count;
    memset(transaction, 0, sizeof(*transaction));
    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_transaction_discard_lift(
    WASMComponentTableTransaction *transaction)
{
    WASMAllocationQuotaScope quota_scope;
    WASMComponentResourceTable *table;
    WASMTableElement *head;
    WASMTableElement *elem;
    bool success = true;

    if (!transaction || !transaction->active
        || transaction->mode != WASM_COMPONENT_TABLE_TRANSACTION_LIFT
        || !(table = transaction->table)
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }
    if (!table_transaction_validate(transaction, true, NULL)) {
        LOG_ERROR("component table: invalid ownership transaction discard");
        table_transaction_forget_marks(transaction);
        success = false;
        goto done;
    }

    /* Detach every handle and retire the transaction before invoking any
       representation destructor.  A destructor may synchronously reenter the
       component and must not observe a half-published result. */
    head = transaction->head;
    for (elem = head; elem; elem = elem->transaction_next) {
        uint32_t index = elem->transaction_index;

        table->array[index] = NULL;
        table->free_list[table->free_count++] = index;
        elem->transaction = NULL;
        elem->transaction_index = 0;
    }
    memset(transaction, 0, sizeof(*transaction));

    elem = head;
    while (elem) {
        WASMTableElement *next = elem->transaction_next;

        elem->transaction_next = NULL;
        if (!wasm_drop_resource_handle((WASMResourceHandle *)elem->ptr))
            success = false;
        wasm_runtime_free(elem);
        elem = next;
    }

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}

bool
wasm_component_table_remove_task_borrows(WASMComponentResourceTable *table,
                                         Task *task)
{
    WASMAllocationQuotaScope quota_scope;
    uint32_t found = 0;
    bool success = false;

    if (!table || !task
        || !wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                             table)) {
        return false;
    }

    /* Validate the complete cleanup before changing any slot.  A failed
       cleanup must retain the Task rather than leave a surviving handle with
       a dangling borrow_scope pointer. */
    for (uint32_t i = 1; i < table->next_index; i++) {
        WASMTableElement *elem = table->array[i];
        WASMResourceHandle *handle;

        if (!elem || elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE)
            continue;
        if (!table_element_owner_is_valid(table, elem) || elem->transaction)
            goto done;
        handle = (WASMResourceHandle *)elem->ptr;
        if (!handle->own && handle->borrow_scope == task) {
            if (found == UINT32_MAX)
                goto done;
            found++;
        }
    }
    if (found != task->num_borrows
        || found > table->array_size - table->free_count) {
        goto done;
    }

    for (uint32_t i = 1; i < table->next_index; i++) {
        WASMTableElement *elem = table->array[i];
        WASMResourceHandle *handle;

        if (!elem || elem->type != WASM_TABLE_ELEM_RESOURCE_HANDLE)
            continue;
        handle = (WASMResourceHandle *)elem->ptr;
        if (handle->own || handle->borrow_scope != task)
            continue;
        table->array[i] = NULL;
        table->free_list[table->free_count++] = i;
        wasm_destroy_resource_handle(handle);
        wasm_runtime_free(elem);
    }
    task->num_borrows = 0;
    success = true;

done:
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}
