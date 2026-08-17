/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_canon.h"
#include "wasm_component_runtime.h"
#include "wasm_component_task.h"
#include "bh_log.h"

bool
canon_resource_new(WASMComponentResourceInstance *rt,
                   WASMComponentInstance *inst, uint32_t rep,
                   uint32_t *out_index)
{
    if (!rt || !inst || !out_index) {
        LOG_ERROR("canon resource.new: invalid arguments");
        return false;
    }

    if (!inst->may_leave) {
        LOG_ERROR("canon resource.new: component instance may not leave");
        return false;
    }

    WASMResourceHandle *handle = wasm_create_resource_handle(rt, rep, true);
    if (!handle) {
        LOG_ERROR("canon resource.new: failed to create resource handle");
        return false;
    }

    if (!wasm_component_table_add(inst->table, handle,
                                  WASM_TABLE_ELEM_RESOURCE_HANDLE, out_index)) {
        wasm_destroy_resource_handle(handle);
        LOG_ERROR("canon resource.new: failed to add handle to table");
        return false;
    }

    return true;
}

bool
canon_resource_rep(const WASMComponentResourceInstance *rt,
                   WASMComponentInstance *inst, uint32_t handle_index,
                   uint32_t *out_rep)
{
    if (!rt || !inst || !out_rep) {
        LOG_ERROR("canon resource.rep: invalid arguments");
        return false;
    }

    const WASMResourceHandle *handle =
        (WASMResourceHandle *)wasm_component_table_get(
            inst->table, handle_index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    if (!handle) {
        LOG_ERROR("canon resource.rep: invalid handle index %u", handle_index);
        return false;
    }

    if (handle->rt != rt) {
        LOG_ERROR("canon resource.rep: resource type mismatch");
        return false;
    }

    *out_rep = handle->rep;
    return true;
}

bool
canon_resource_drop(const WASMComponentResourceInstance *rt,
                    WASMComponentInstance *inst, uint32_t handle_index)
{
    if (!rt || !inst) {
        LOG_ERROR("canon resource.drop: invalid arguments");
        return false;
    }

    if (!inst->may_leave) {
        LOG_ERROR("canon resource.drop: component instance may not leave");
        return false;
    }

    WASMResourceHandle *handle = (WASMResourceHandle *)wasm_component_table_get(
        inst->table, handle_index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    if (!handle) {
        LOG_ERROR("canon resource.drop: invalid handle index %u", handle_index);
        return false;
    }

    if (handle->rt != rt) {
        LOG_ERROR("canon resource.drop: resource type mismatch");
        return false;
    }

    if (handle->num_lends != 0) {
        LOG_ERROR("canon resource.drop: handle still has %u active borrows",
                  handle->num_lends);
        return false;
    }

    Task *borrow_scope = handle->borrow_scope;
    if (borrow_scope && borrow_scope->num_borrows == 0) {
        LOG_ERROR("canon resource.drop: borrow count underflow");
        return false;
    }

    if (!wasm_component_table_drop_resource(inst->table, handle_index)) {
        LOG_ERROR("canon resource.drop: failed to drop handle from table");
        return false;
    }

    if (borrow_scope) {
        borrow_scope->num_borrows--;
    }

    return true;
}
