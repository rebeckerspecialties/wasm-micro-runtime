/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource_table.h"
#include "wasm_component_resource.h"
#include "wasm_runtime_common.h"
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

    // Initialize both arrays to zero
    memset((void *)table->array, 0,
           sizeof(WASMTableElement *) * table->array_size);
    memset(table->free_list, 0, sizeof(uint32_t) * table->array_size);

    // Initialize free list state
    table->free_count = 0;
    table->next_index = 1;
    table->resize_percent = resize_percent;

    return table;
}

void
wasm_component_table_destroy(WASMComponentResourceTable *table)
{
    if (!table)
        return;

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

    if (table->array) wasm_runtime_free((void *)table->array);
    if (table->free_list) wasm_runtime_free((void *)table->free_list);
    wasm_runtime_free((void *)table);
}

void *
wasm_component_table_get(WASMComponentResourceTable *table, uint32_t index,
                         WASMTableElementType expected_type)
{
    if (!table)
        return NULL;

    if (index >= table->next_index)
        return NULL;

    if (table->array[index] == NULL)
        return NULL;

    const WASMTableElement *elem = table->array[index];
    if (elem->type != expected_type) {
        return NULL;
    }

    return elem->ptr;
}

bool
wasm_component_table_remove(WASMComponentResourceTable *table, uint32_t index)
{
    if (!table)
        return false;

    if (index >= table->next_index || index == 0
        || table->array[index] == NULL) {
        return false;
    }

    if (table->free_count >= table->array_size) {
        return false; // Should not happen
    }

    WASMTableElement *elem = table->array[index];

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

    return true;
}

bool
wasm_component_table_drop_resource(WASMComponentResourceTable *table,
                                   uint32_t index)
{
    WASMTableElement *elem;
    bool success;

    if (!table || index == 0 || index >= table->next_index
        || table->array[index] == NULL
        || table->array[index]->type != WASM_TABLE_ELEM_RESOURCE_HANDLE
        || table->free_count >= table->array_size) {
        return false;
    }

    elem = table->array[index];

    /* Commit the removal before invoking destructor code. Even when teardown
     * reports an error, this handle must not be dropped a second time. */
    table->array[index] = NULL;
    table->free_list[table->free_count++] = index;

    success = wasm_drop_resource_handle((WASMResourceHandle *)elem->ptr);
    wasm_runtime_free(elem);
    return success;
}

static bool
wasm_component_table_resize(WASMComponentResourceTable *table)
{
    if (!table)
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
    if (!table || !ptr || !out_index)
        return false;

    WASMTableElement *elem = wasm_runtime_malloc(sizeof(WASMTableElement));
    if (!elem)
        return false;

    elem->type = type;
    elem->ptr = ptr;

    uint32_t index = 0;

    // Use free list first
    if (table->free_count > 0) {
        index = table->free_list[table->free_count - 1];
        table->free_count--;

        if (table->array[index] != NULL) {
            wasm_runtime_free(elem);
            return false; // Should not happen
        }

        table->array[index] = elem;
    }
    else {
        // Grow array
        index = table->next_index;

        if (index > WASM_COMPONENT_TABLE_MAX_LENGTH) {
            wasm_runtime_free(elem);
            return false;
        }

        // Resize array if needed
        if (index >= table->array_size) {
            if (!wasm_component_table_resize(table)) {
                wasm_runtime_free(elem);
                return false;
            }
        }

        table->array[index] = elem;
        table->next_index++;
    }

    *out_index = index;
    return true;
}
