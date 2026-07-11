/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_runtime.h"
#include "wasm_runtime.h"
#include "wasm_runtime_common.h"
#include "bh_log.h"
#include <string.h>

WASMResourceHandle *
wasm_create_resource_handle(WASMComponentResourceInstance *rt, uint32_t rep,
                            bool own)
{
    if (!rt) {
        return NULL;
    }

    if (rep < 1) {
        return NULL;
    }

    WASMResourceHandle *handle =
        wasm_runtime_malloc(sizeof(WASMResourceHandle));
    if (!handle) {
        return NULL;
    }

    memset(handle, 0, sizeof(WASMResourceHandle));

    handle->rt = rt;
    handle->own = own;
    handle->borrow_scope = NULL;
    handle->num_lends = 0;
    handle->rep = rep;

    return handle;
}

void
wasm_destroy_resource_handle(WASMResourceHandle *handle)
{
    if (!handle)
        return;

    wasm_runtime_free(handle);
}

bool
wasm_drop_resource_handle(WASMResourceHandle *handle)
{
    bool success = true;

    if (!handle) {
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
    return success;
}
