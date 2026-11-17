/*
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 * Instance-local import binding for the opt-in core-linking profile.
 */

static void
c_api_core_imports_destroy(WASMCApiCoreImports *bindings)
{
    wasm_runtime_free(bindings->memories);
    wasm_runtime_free(bindings->functions);
    wasm_runtime_free(bindings->callbacks);
    memset(bindings, 0, sizeof(*bindings));
}

static bool
c_api_core_imports_prepare(wasm_store_t *store, const WASMModule *module,
                           const wasm_extern_vec_t *imports,
                           WASMCApiCoreImports *bindings, char *error,
                           uint32 error_size)
{
    uint32 i, function_index = 0, memory_index = 0;
    uint64 count = (uint64)module->import_function_count
                   + module->import_memory_count + module->import_table_count
                   + module->import_global_count;

#define LINK_ERROR(message)                         \
    do {                                            \
        snprintf(error, error_size, "%s", message); \
        goto fail;                                  \
    } while (0)

    if (module->import_table_count || module->import_global_count)
        LINK_ERROR(
            "core-linking profile: table/global imports not supported yet");
    if (count != (imports ? imports->num_elems : 0)
        || (count && !imports->data))
        LINK_ERROR("core import count mismatch");
    if (module->import_memory_count
        && !(bindings->memories = malloc_internal(
                 (uint64)module->import_memory_count * sizeof(void *))))
        LINK_ERROR("core memory binding allocation failed");
    if (module->import_function_count
        && (!(bindings->functions =
                  malloc_internal((uint64)module->import_function_count
                                  * sizeof(WASMFunctionImport)))
            || !(bindings->callbacks =
                     malloc_internal((uint64)module->import_function_count
                                     * sizeof(CApiFuncImport)))))
        LINK_ERROR("core function binding allocation failed");

    for (i = 0; i < count; i++) {
        wasm_extern_t *external = imports->data[i];
        if (!external || external->store != store)
            LINK_ERROR("null or foreign-store core import");
        /* Match wasm_module_imports' established C API ordering. */
        if (external->kind
            != (i < module->import_function_count ? WASM_EXTERN_FUNC
                                                  : WASM_EXTERN_MEMORY))
            LINK_ERROR("core import kind mismatch");
        if (external->kind == WASM_EXTERN_FUNC) {
            wasm_func_t *function = wasm_extern_as_func(external);
            WASMFunctionImport *target = &bindings->functions[function_index];
            CApiFuncImport *callback = &bindings->callbacks[function_index];
            *target = module->import_functions[function_index++].u.function;
            if (!function->type
                || !wasm_functype_same_internal(function->type,
                                                target->func_type))
                LINK_ERROR("core function import type mismatch");
            if (function->inst_comm_rt)
                LINK_ERROR("core-linking profile: guest function import not "
                           "supported yet");
            target->call_conv_wasm_c_api = true;
            target->func_ptr_linked = function->with_env
                                          ? (void *)function->u.cb_env.cb
                                          : (void *)function->u.cb;
            if (!target->func_ptr_linked)
                LINK_ERROR("null core host callback");
            callback->func_ptr_linked = target->func_ptr_linked;
            callback->with_env_arg = function->with_env;
            callback->env_arg =
                function->with_env ? function->u.cb_env.env : NULL;
        }
        else if (external->kind == WASM_EXTERN_MEMORY) {
            wasm_memory_t *memory = wasm_extern_as_memory(external);
            WASMModuleInstance *owner =
                (WASMModuleInstance *)memory->inst_comm_rt;
            WASMMemoryType *expected =
                &module->import_memories[memory_index].u.memory.mem_type;
            WASMMemoryInstance *actual;
            if (!owner || owner->module_type != Wasm_Module_Bytecode
                || memory->memory_idx_rt >= owner->memory_count)
                LINK_ERROR("core memory import must refer to a live "
                           "interpreter memory");
            actual = owner->memories[memory->memory_idx_rt];
            if (!memory->type || actual->is_shared_memory || actual->is_memory64
                || (expected->flags & (SHARED_MEMORY_FLAG | MEMORY64_FLAG))
                || actual->cur_page_count < expected->init_page_count
                || actual->num_bytes_per_page != expected->num_bytes_per_page
                || ((expected->flags & MAX_PAGE_COUNT_FLAG)
                    && (!actual->c_api_has_max
                        || actual->max_page_count > expected->max_page_count)))
                LINK_ERROR("core memory import type mismatch");
            bindings->memories[memory_index++] = actual;
        }
    }
    return true;
fail:
    c_api_core_imports_destroy(bindings);
    return false;
#undef LINK_ERROR
}
