/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_runtime.h"
#include "wasm_runtime.h"
#include "wasm_runtime_common.h"
#include "wasm_allocation_quota.h"
#include "bh_log.h"
#include <string.h>

static bool
resource_handle_owner_graph_is_valid(const WASMResourceHandle *handle)
{
    WASMComponentResourceInstance *rt;

    if (!handle || !(rt = handle->rt)) {
        return false;
    }
    if (rt->impl
        && !wasm_allocation_quota_allocations_share_owner(handle, rt->impl)) {
        LOG_ERROR("resource handle: component owner mismatch");
        return false;
    }
    if (rt->dtor_method && rt->dtor_method->module_instance
        && !wasm_allocation_quota_allocations_share_owner(
            handle, rt->dtor_method->module_instance)) {
        LOG_ERROR("resource handle: destructor module owner mismatch");
        return false;
    }
    return true;
}

WASMResourceHandle *
wasm_create_resource_handle(WASMComponentResourceInstance *rt, uint32_t rep,
                            bool own)
{
    WASMAllocationQuotaScope quota_scope;
    WASMResourceHandle *handle = NULL;
    bool scoped = false;

    if (!rt) {
        return NULL;
    }

    if (rep < 1) {
        return NULL;
    }
    if (rt->impl) {
        if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                              rt->impl)) {
            return NULL;
        }
        scoped = true;
    }

    handle = wasm_runtime_malloc(sizeof(WASMResourceHandle));
    if (!handle) {
        goto done;
    }

    memset(handle, 0, sizeof(WASMResourceHandle));

    handle->rt = rt;
    handle->own = own;
    handle->borrow_scope = NULL;
    handle->num_lends = 0;
    handle->rep = rep;
    if (!resource_handle_owner_graph_is_valid(handle)) {
        wasm_runtime_free(handle);
        handle = NULL;
    }

done:
    if (scoped) {
        wasm_allocation_quota_scope_leave(&quota_scope);
    }
    return handle;
}

void
wasm_destroy_resource_handle(WASMResourceHandle *handle)
{
    WASMAllocationQuotaScope quota_scope;

    if (!handle)
        return;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                          handle)) {
        LOG_ERROR("resource handle: allocation quota owner scope could not be "
                  "established for destroy");
        return;
    }
    if (!resource_handle_owner_graph_is_valid(handle)) {
        wasm_allocation_quota_scope_leave(&quota_scope);
        return;
    }

    wasm_runtime_free(handle);
    wasm_allocation_quota_scope_leave(&quota_scope);
}

bool
wasm_drop_resource_handle(WASMResourceHandle *handle)
{
    WASMAllocationQuotaScope quota_scope;
    bool success = true;

    if (!handle) {
        return false;
    }
    if (!wasm_allocation_quota_scope_enter_for_allocation(&quota_scope,
                                                          handle)) {
        LOG_ERROR("resource drop: allocation quota owner scope could not be "
                  "established");
        return false;
    }
    if (!resource_handle_owner_graph_is_valid(handle)) {
        wasm_allocation_quota_scope_leave(&quota_scope);
        return false;
    }

    if (handle->own) {
        WASMComponentResourceInstance *rt = handle->rt;
        uint32_t rep = handle->rep;

        if (!rt) {
            LOG_ERROR("resource drop: missing resource type");
            success = false;
        }
        else {
            if (rt->host_drop_callback) {
                if (!rt->host_drop_callback(rt->host_drop_attachment, rep)) {
                    LOG_ERROR("resource drop: host callback failed for %s/%s "
                              "representation %u",
                              rt->interface_name ? rt->interface_name : "?",
                              rt->name ? rt->name : "?", rep);
                    success = false;
                }
            }
            else if (rt->is_builtin_wasi) {
                HostResourceTable *hr_table = get_global_host_resource_table();

                if (!hr_table) {
                    LOG_ERROR("resource drop: host resource table is not "
                              "initialized");
                    success = false;
                }
                else if (!host_resource_table_delete(hr_table, rep)) {
                    LOG_ERROR("resource drop: host resource %u was not found",
                              rep);
                    success = false;
                }
            }
            else if (rt->is_host) {
                LOG_ERROR("resource drop: imported host resource %s/%s has no "
                          "owner-drop callback",
                          rt->interface_name ? rt->interface_name : "?",
                          rt->name ? rt->name : "?");
                success = false;
            }

            if (rt->dtor_method) {
                WASMModuleInstanceCommon *dtor_module_inst =
                    (WASMModuleInstanceCommon *)
                        rt->dtor_method->module_instance;
                WASMExecEnv *dtor_exec_env =
                    wasm_runtime_get_exec_env_singleton(dtor_module_inst);

                if (!dtor_exec_env) {
                    LOG_ERROR("resource drop: no exec_env for dtor");
                    success = false;
                }
                else if (!wasm_allocation_quota_allocations_share_owner(
                             handle, dtor_exec_env)) {
                    LOG_ERROR("resource drop: destructor exec env owner "
                              "mismatch");
                    success = false;
                }
                else {
                    WASMModuleInstanceCommon *saved_inst =
                        wasm_runtime_get_module_inst(dtor_exec_env);
                    wasm_val_t arg = {
                        .kind = WASM_I32,
                        .of.i32 = (int32_t)rep,
                    };
#ifdef OS_ENABLE_HW_BOUND_CHECK
                    WASMExecEnv *saved_tls = wasm_runtime_get_exec_env_tls();
                    wasm_runtime_set_exec_env_tls(NULL);
#endif

                    wasm_exec_env_set_module_inst(dtor_exec_env,
                                                  dtor_module_inst);
                    if (!wasm_runtime_call_wasm_a(
                            dtor_exec_env,
                            (WASMFunctionInstanceCommon *)rt->dtor_method, 0,
                            NULL, 1, &arg)) {
                        const char *ex =
                            wasm_runtime_get_exception(dtor_module_inst);
                        LOG_ERROR("resource drop: dtor call failed: %s",
                                  ex ? ex : "(unknown)");
                        success = false;
                    }
                    wasm_exec_env_restore_module_inst(dtor_exec_env,
                                                      saved_inst);
#ifdef OS_ENABLE_HW_BOUND_CHECK
                    wasm_runtime_set_exec_env_tls(saved_tls);
#endif
                }
            }
        }
    }

    wasm_runtime_free(handle);
    wasm_allocation_quota_scope_leave(&quota_scope);
    return success;
}
