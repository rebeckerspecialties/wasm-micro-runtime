/*
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 * Instance-local import binding for the opt-in core-linking profile.
 */

static void
c_api_core_imports_destroy(WASMCApiCoreImports *bindings)
{
    wasm_runtime_free(bindings->memories);
    wasm_runtime_free(bindings->tables);
    wasm_runtime_free(bindings->guest_functions);
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
    uint32 i, function_index = 0, memory_index = 0, table_index = 0;
    uint64 count = (uint64)module->import_function_count
                   + module->import_memory_count + module->import_table_count
                   + module->import_global_count;

#define LINK_ERROR(message)                         \
    do {                                            \
        snprintf(error, error_size, "%s", message); \
        goto fail;                                  \
    } while (0)

    if (module->import_global_count)
        LINK_ERROR("core-linking profile: global imports not supported yet");
    /* Public C API function wrappers currently use 16-bit indices. */
    if ((uint64)module->import_function_count + module->function_count
        > UINT16_MAX)
        LINK_ERROR("core-linking C API function index limit");
    if (!store->link_domain
        && !(store->link_domain = malloc_internal(sizeof(*store->link_domain))))
        LINK_ERROR("core reference domain allocation failed");
    bindings->domain = store->link_domain;
    if (count != (imports ? imports->num_elems : 0)
        || (count && !imports->data))
        LINK_ERROR("core import count mismatch");
    if (module->import_memory_count
        && !(bindings->memories = malloc_internal(
                 (uint64)module->import_memory_count * sizeof(void *))))
        LINK_ERROR("core memory binding allocation failed");
    if (module->import_table_count
        && !(bindings->tables = malloc_internal(
                 (uint64)module->import_table_count * sizeof(void *))))
        LINK_ERROR("core table binding allocation failed");
    if (module->import_function_count
        && (!(bindings->functions =
                  malloc_internal((uint64)module->import_function_count
                                  * sizeof(WASMFunctionImport)))
            || !(bindings->guest_functions = malloc_internal(
                     (uint64)module->import_function_count * sizeof(void *)))
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
                : i < module->import_function_count
                            + module->import_memory_count
                    ? WASM_EXTERN_MEMORY
                    : WASM_EXTERN_TABLE))
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
            if (function->inst_comm_rt) {
                WASMModuleInstance *owner =
                    (WASMModuleInstance *)function->inst_comm_rt;
                if (owner->module_type != Wasm_Module_Bytecode
                    || owner->e->c_api_domain != bindings->domain
                    || function->func_idx_rt >= owner->e->function_count)
                    LINK_ERROR("foreign core function domain");
                bindings->guest_functions[function_index - 1] =
                    &owner->e->functions[function->func_idx_rt];
                continue;
            }
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
        else if (external->kind == WASM_EXTERN_TABLE) {
            wasm_table_t *table = wasm_extern_as_table(external);
            WASMModuleInstance *owner =
                (WASMModuleInstance *)table->inst_comm_rt;
            WASMTableType *expected =
                &module->import_tables[table_index].u.table.table_type;
            WASMTableInstance *actual;
            if (!owner || owner->module_type != Wasm_Module_Bytecode
                || owner->e->c_api_domain != bindings->domain
                || table->table_idx_rt >= owner->table_count)
                LINK_ERROR("foreign core table domain");
            actual = owner->tables[table->table_idx_rt];
            if (actual->elem_type != expected->elem_type || actual->is_table64
                || (expected->flags & (TABLE64_FLAG | SHARED_TABLE_FLAG))
                || actual->cur_size < expected->init_size
                || ((expected->flags & MAX_TABLE_SIZE_FLAG)
                    && (!actual->c_api_has_max
                        || actual->max_size > expected->max_size)))
                LINK_ERROR("core table import type mismatch");
            bindings->tables[table_index++] = actual;
        }
    }
    {
        WASMCApiLinkDomain *domain = bindings->domain;
        uint64 needed = (uint64)domain->count + module->import_function_count
                        + module->function_count;
        if (needed > UINT32_MAX / sizeof(void *))
            LINK_ERROR("core function reference limit");
        if (needed > domain->capacity) {
            uint64 capacity = (uint64)domain->capacity * 2;
            WASMFunctionInstance **functions;
            if (capacity < needed)
                capacity = needed;
            if (capacity > UINT32_MAX / sizeof(void *))
                capacity = needed;
            functions = wasm_runtime_realloc(
                domain->functions, (uint32)(capacity * sizeof(void *)));
            if (!functions)
                LINK_ERROR("core function reference allocation failed");
            memset(functions + domain->capacity, 0,
                   (size_t)(capacity - domain->capacity) * sizeof(void *));
            domain->functions = functions;
            domain->capacity = (uint32)capacity;
        }
        bindings->reference_base = domain->count;
    }
    return true;
fail:
    c_api_core_imports_destroy(bindings);
    return false;
#undef LINK_ERROR
}
