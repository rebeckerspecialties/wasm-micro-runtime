/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component.h"
#include "wasm_component_runtime.h"
#include "wasm_component_flat.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "wasm_loader_common.h"
#include "wasm_runtime_common.h"
#include "wasm_export.h"
#include <stdio.h>
#if WASM_ENABLE_LIBC_WASI != 0
#include "../../libraries/libc-wasi-p2/libc_wasi_p2_wrapper.h"
#endif

// Section 10: imports section
bool
wasm_component_parse_imports_section(const uint8_t **payload,
                                     uint32_t payload_len,
                                     WASMComponentImportSection *out,
                                     char *error_buf, uint32_t error_buf_size,
                                     uint32_t *consumed_len)
{
    if (!payload || !*payload || payload_len == 0 || !out) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Invalid payload or output pointer");
        if (consumed_len)
            *consumed_len = 0;
        return false;
    }

    const uint8_t *p = *payload;
    const uint8_t *end = *payload + payload_len;

    // import ::= in:<importname'> ed:<externdesc> => (import in ed)
    // Read the count of imports (LEB128-encoded)
    uint64_t import_count_leb = 0;
    if (!read_leb((uint8_t **)&p, end, 32, false, &import_count_leb, error_buf,
                  error_buf_size)) {
        if (consumed_len)
            *consumed_len = (uint32_t)(p - *payload);
        return false;
    }

    uint32_t import_count = (uint32_t)import_count_leb;
    out->count = import_count;

    if (import_count > 0) {
        out->imports = wasm_component_checked_calloc(
            import_count, sizeof(WASMComponentImport), p, end, 1,
            "component import", error_buf, error_buf_size);
        if (!out->imports) {
            if (consumed_len)
                *consumed_len = (uint32_t)(p - *payload);
            return false;
        }

        for (uint32_t i = 0; i < import_count; ++i) {
            // importname' ::= 0x00 len:<u32> in:<importname> => in (if len =
            // |in|)
            //             | 0x01 len:<u32> in:<importname> vs:<versionsuffix'>
            //             => in vs (if len = |in|)
            // Parse import name (simple or versioned)
            WASMComponentImportName *import_name =
                wasm_runtime_malloc(sizeof(WASMComponentImportName));
            if (!import_name) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to allocate memory for import_name");
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            // Initialize the struct to zero to avoid garbage data
            memset(import_name, 0, sizeof(WASMComponentImportName));
            out->imports[i].import_name = import_name;

            bool status = parse_component_import_name(
                &p, end, import_name, error_buf, error_buf_size);
            if (!status) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to parse component name for import %u",
                                 i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }

            // externdesc ::= 0x00 0x11 i:<core:typeidx> => (core module (type
            // i))
            //            | 0x01 i:<typeidx> => (func (type i))
            //            | 0x02 b:<valuebound> => (value b)
            //            | 0x03 b:<typebound> => (type b)
            //            | 0x04 i:<typeidx> => (component (type i))
            //            | 0x05 i:<typeidx> => (instance (type i))
            // Parse externdesc (core module, func, value, type, component,
            // instance)
            WASMComponentExternDesc *extern_desc =
                wasm_runtime_malloc(sizeof(WASMComponentExternDesc));
            if (!extern_desc) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to allocate memory for extern_desc");
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            // Initialize the struct to zero to avoid garbage data
            memset(extern_desc, 0, sizeof(WASMComponentExternDesc));
            out->imports[i].extern_desc = extern_desc;

            status = parse_extern_desc(&p, end, extern_desc, error_buf,
                                       error_buf_size);
            if (!status) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to parse extern_desc for import %u",
                                 i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }

            // Store the parsed import (importname' + externdesc)
        }
    }

    if (consumed_len)
        *consumed_len = (uint32_t)(p - *payload);
    return true;
}

// Individual section free functions
void
wasm_component_free_imports_section(WASMComponentSection *section)
{
    if (!section || !section->parsed.import_section)
        return;

    WASMComponentImportSection *import_sec = section->parsed.import_section;
    if (import_sec->imports) {
        for (uint32_t j = 0; j < import_sec->count; ++j) {
            WASMComponentImport *import = &import_sec->imports[j];

            // Free import name
            if (import->import_name) {
                free_component_import_name(import->import_name);
                wasm_runtime_free(import->import_name);
                import->import_name = NULL;
            }

            // Free extern desc
            if (import->extern_desc) {
                free_extern_desc(import->extern_desc);
                wasm_runtime_free(import->extern_desc);
                import->extern_desc = NULL;
            }
        }
        wasm_runtime_free(import_sec->imports);
        import_sec->imports = NULL;
    }
    wasm_runtime_free(import_sec);
    section->parsed.import_section = NULL;
}

WASMCoreExport *
wasm_import_find_in_args(WASMImport *import, WASMInstExpr *expression,
                         WASMComponentInstance *comp_instance, char *error_buf,
                         uint32 error_buf_size)
{

    if (!import || !expression || !comp_instance) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: invalid function call\n");
        return NULL;
    }
    uint32 arg_idx = 0, export_idx = 0;
    const char *module_name = NULL, *field_name = NULL;
    WASMComponentInstArg *arg = NULL;
    WASMModuleInstance *arg_instance = NULL;
    WASMCoreExport resolved_export = { 0 };
    WASMCoreExport *found_export = NULL;
    bool found = false;

    module_name = import->u.names.module_name;
    field_name = import->u.names.field_name;
    if (!module_name || !field_name) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: invalid core import name\n");
        return NULL;
    }

    resolved_export.kind = import->kind;

    for (arg_idx = 0; arg_idx < expression->with_args.arg_len; arg_idx++) {
        arg = &expression->with_args.args[arg_idx];
        if (!arg->name || !arg->name->name) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: invalid instantiation argument name\n");
            return NULL;
        }
        // Find Module instance by name
        if (!strcmp(module_name, arg->name->name)) {
            if (arg->idx.instance_idx
                >= comp_instance->core_module_instances_count) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Instantiation argument Core instance "
                                 "%d not yet defined\n",
                                 arg->idx.instance_idx);
                return NULL;
            }
            arg_instance =
                comp_instance->core_module_instances[arg->idx.instance_idx];
            if (!arg_instance) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Instantiation argument Core instance "
                                 "%d is null\n",
                                 arg->idx.instance_idx);
                return NULL;
            }
            // Search thought the respective export list of the provided core
            // instance
            switch (import->kind) {
                case IMPORT_KIND_FUNC:
                    for (export_idx = 0;
                         export_idx < arg_instance->export_func_count;
                         export_idx++) {
                        if (!strcmp(field_name,
                                    arg_instance->export_functions[export_idx]
                                        .name)) {
                            // TODO: Will need to implement a check for function
                            // signature as well
                            LOG_DEBUG("Func import %s found", field_name);
                            resolved_export.exp.func_instance =
                                arg_instance->export_functions[export_idx]
                                    .function;
                            found = true;
                            break;
                        }
                    }
                    break;
                case IMPORT_KIND_TABLE:
                    for (export_idx = 0;
                         export_idx < arg_instance->export_table_count;
                         export_idx++) {
                        if (!strcmp(
                                field_name,
                                arg_instance->export_tables[export_idx].name)) {
                            LOG_DEBUG("table import %s found", field_name);
                            resolved_export.exp.table_instance =
                                arg_instance->export_tables[export_idx].table;
                            found = true;
                            break;
                        }
                    }
                    break;
                case IMPORT_KIND_MEMORY:
                    for (export_idx = 0;
                         export_idx < arg_instance->export_memory_count;
                         export_idx++) {
                        if (!strcmp(field_name,
                                    arg_instance->export_memories[export_idx]
                                        .name)) {
                            LOG_DEBUG("mem import %s found", field_name);
                            resolved_export.exp.mem_instance =
                                arg_instance->export_memories[export_idx]
                                    .memory;
                            found = true;
                            break;
                        }
                    }
                    break;
                case IMPORT_KIND_GLOBAL:
                    for (export_idx = 0;
                         export_idx < arg_instance->export_global_count;
                         export_idx++) {
                        if (!strcmp(field_name,
                                    arg_instance->export_globals[export_idx]
                                        .name)) {
                            LOG_DEBUG("global import %s found", field_name);
                            resolved_export.exp.global_instance =
                                arg_instance->export_globals[export_idx].global;
                            found = true;
                            break;
                        }
                    }
                    break;
                default:
                    LOG_WARNING("Import kind not supported\n");
                    break;
            }
            if (found) {
                found_export = wasm_runtime_malloc(sizeof(WASMCoreExport));
                if (!found_export) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: failed to allocate resolved core "
                                     "export\n");
                    return NULL;
                }
                *found_export = resolved_export;
                return found_export;
            }
        }
    }
    return NULL;
}

bool
wasm_resolve_core_imports(WASMInstExpr *expression, WASMModule *target,
                          WASMComponentInstance *comp_instance,
                          WASMCoreImports *found_imports, char *error_buf,
                          uint32 error_buf_size)
{
    // Check if the arguments provided in the instantion expression resolve all
    // the imports required by the target core module
    uint32 import_idx = 0;
    WASMImport *import = NULL;
    WASMCoreExport *imported_instance = NULL;

    if (!target->imports) {
        return false;
    }
    for (import_idx = 0; import_idx < target->import_count; import_idx++) {
        import = &target->imports[import_idx];
        // For each import required by the target core module, check if it is
        // satisfied by one of the exports of the provided core instance
        // arguments
        imported_instance = wasm_import_find_in_args(
            import, expression, comp_instance, error_buf, error_buf_size);

        if (!imported_instance) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "Import %d of core instance not found in arguments\n",
                import_idx);
            return false;
        }
        else {
            switch (imported_instance->kind) {
                case IMPORT_KIND_FUNC:
                    found_imports->func_instance[found_imports->func_count] =
                        imported_instance->exp.func_instance;
                    found_imports->func_count++;
                    break;
                case IMPORT_KIND_TABLE:
                    found_imports->table_instance[found_imports->tables_count] =
                        imported_instance->exp.table_instance;
                    found_imports->tables_count++;
                    break;
                case IMPORT_KIND_MEMORY:
                    found_imports->mem_instance[found_imports->mem_count] =
                        imported_instance->exp.mem_instance;
                    found_imports->mem_count++;
                    break;
                case IMPORT_KIND_GLOBAL:
                    found_imports
                        ->global_instance[found_imports->globals_count] =
                        imported_instance->exp.global_instance;
                    found_imports->globals_count++;
                    break;
                default:
                    break;
            }
        }

        wasm_runtime_free(imported_instance);
    }
    return true;
}

#define HOST_IMPORT_CLONE_MAP_TYPE 0x1
#define HOST_IMPORT_CLONE_MAP_LOCAL 0x2
#define HOST_IMPORT_CORE_TYPE_MAX_VALUES (MAX_FLAT_PARAMS + MAX_FLAT_RESULTS)

typedef struct WASMHostImportCloneMapEntry {
    const void *source;
    void *clone;
    uint8 flags;
} WASMHostImportCloneMapEntry;

typedef struct WASMHostImportCloneContext {
    const WASMComponentInstTypeInstance *type_template;
    WASMComponentInstance *instance;
    uint8 *next;
    uint64 remaining;
    WASMHostImportCloneMapEntry *map;
    uint32 map_capacity;
} WASMHostImportCloneContext;

static uint64
host_import_clone_align_size(uint64 size)
{
    uint64 alignment = sizeof(void *);

    return (size + alignment - 1) & ~(alignment - 1);
}

static bool
host_import_clone_add_size(uint64 *total, uint64 size)
{
    uint64 aligned_size;

    if (!total || size > UINT64_MAX - (sizeof(void *) - 1)) {
        return false;
    }

    aligned_size = host_import_clone_align_size(size);
    if (*total > UINT64_MAX - aligned_size) {
        return false;
    }

    *total += aligned_size;
    return true;
}

static bool
host_import_type_is_local(const WASMComponentInstTypeInstance *type_template,
                          const WASMComponentTypeInstance *type)
{
    uintptr_t begin;
    uintptr_t end;
    uintptr_t address;

    if (!type_template || !type || !type_template->owned_types_begin
        || type_template->owned_types_size == 0) {
        return false;
    }

    begin = (uintptr_t)type_template->owned_types_begin;
    if (begin > UINTPTR_MAX - type_template->owned_types_size) {
        return false;
    }
    end = begin + type_template->owned_types_size;
    address = (uintptr_t)type;
    return address >= begin && address < end;
}

static bool
host_import_clone_map_capacity(uint32 type_count, uint32 *capacity)
{
    uint64 required;
    uint32 result = 8;

    if (!capacity) {
        return false;
    }
    if (type_count == 0) {
        *capacity = 0;
        return true;
    }

    /* Each unique type contributes its wrapper plus at most one resource or
     * function implementation key. Keep the open-addressed map <= 50% full. */
    required = (uint64)type_count * 4;
    while (result < required) {
        if (result > UINT32_MAX / 2) {
            return false;
        }
        result *= 2;
    }

    *capacity = result;
    return true;
}

static uint32
host_import_clone_map_hash(const void *source, uint32 capacity)
{
    uintptr_t value = (uintptr_t)source;

    value >>= 3;
    value ^= value >> 16;
    value *= (uintptr_t)2654435761U;
    return (uint32)value & (capacity - 1);
}

static WASMHostImportCloneMapEntry *
host_import_clone_map_find_in(WASMHostImportCloneMapEntry *map, uint32 capacity,
                              const void *source)
{
    uint32 index;

    if (!map || capacity == 0 || !source) {
        return NULL;
    }

    index = host_import_clone_map_hash(source, capacity);
    for (uint32 probe = 0; probe < capacity; probe++) {
        WASMHostImportCloneMapEntry *entry =
            &map[(index + probe) & (capacity - 1)];

        if (!entry->source) {
            return NULL;
        }
        if (entry->source == source) {
            return entry;
        }
    }

    return NULL;
}

static WASMHostImportCloneMapEntry *
host_import_clone_map_find(const WASMHostImportCloneContext *clone_ctx,
                           const void *source)
{
    if (!clone_ctx) {
        return NULL;
    }
    return host_import_clone_map_find_in(clone_ctx->map,
                                         clone_ctx->map_capacity, source);
}

static bool
host_import_clone_map_insert(WASMHostImportCloneMapEntry *map, uint32 capacity,
                             const void *source, void *clone, uint8 flags)
{
    uint32 index;

    if (!map || capacity == 0 || !source || !clone) {
        return false;
    }

    index = host_import_clone_map_hash(source, capacity);
    for (uint32 probe = 0; probe < capacity; probe++) {
        WASMHostImportCloneMapEntry *entry =
            &map[(index + probe) & (capacity - 1)];

        if (!entry->source) {
            entry->source = source;
            entry->clone = clone;
            entry->flags = flags;
            return true;
        }
        if (entry->source == source) {
            if (entry->clone != clone) {
                return false;
            }
            entry->flags |= flags;
            return true;
        }
    }

    return false;
}

static bool
host_import_clone_specific_size(const WASMComponentTypeInstance *type,
                                uint64 *size)
{
    if (!type || !size) {
        return false;
    }

    *size = 0;
    switch (type->type) {
        case COMPONENT_VAL_TYPE_RECORD:
            if (!type->type_specific.record) {
                return false;
            }
            *size = sizeof(WASMComponentRecordInstance)
                    + (uint64)type->type_specific.record->count
                          * sizeof(WASMComponentLabelValTypeInstance);
            break;
        case COMPONENT_VAL_TYPE_VARIANT:
            if (!type->type_specific.variant) {
                return false;
            }
            *size = sizeof(WASMComponentVariantInstance)
                    + (uint64)type->type_specific.variant->count
                          * sizeof(WASMComponentCaseValInstance);
            break;
        case COMPONENT_VAL_TYPE_LIST:
            if (!type->type_specific.list) {
                return false;
            }
            *size = sizeof(WASMComponentListInstance);
            break;
        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
            if (!type->type_specific.list_len) {
                return false;
            }
            *size = sizeof(WASMComponentListLenInstance);
            break;
        case COMPONENT_VAL_TYPE_TUPLE:
            if (!type->type_specific.tuple) {
                return false;
            }
            *size = sizeof(WASMComponentTupleInstance)
                    + (uint64)type->type_specific.tuple->count
                          * sizeof(WASMComponentTypeInstance *);
            break;
        case COMPONENT_VAL_TYPE_OPTION:
            if (!type->type_specific.option) {
                return false;
            }
            *size = sizeof(WASMComponentOptionInstance);
            break;
        case COMPONENT_VAL_TYPE_RESULT:
            if (!type->type_specific.result) {
                return false;
            }
            *size = sizeof(WASMComponentResultInstance);
            break;
        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
            if (!type->type_specific.resource_handle) {
                return false;
            }
            *size = sizeof(WASMComponentResourceHandleInstance);
            break;
        case COMPONENT_VAL_TYPE_FUNCTION:
            if (!type->type_specific.function
                || !type->type_specific.function->params
                || !type->type_specific.function->results) {
                return false;
            }
            *size = sizeof(WASMComponentFuncTypeInstance)
                    + sizeof(WASMComponentParamListInstance)
                    + (uint64)type->type_specific.function->params->count
                          * sizeof(WASMComponentLabelValTypeInstance)
                    + sizeof(WASMComponentResultListInstance);
            break;
        case COMPONENT_VAL_TYPE_RESOURCE_SYNC:
        case COMPONENT_VAL_TYPE_RESOURCE_ASYNC:
            if (!type->type_specific.resource) {
                return false;
            }
            *size = sizeof(WASMComponentResourceInstance)
                    + 3 * sizeof(WASMFunctionInstance);
            break;
        default:
            break;
    }

    return true;
}

static bool
host_import_clone_storage_size(
    const WASMComponentInstTypeInstance *type_template, uint64 *size,
    uint32 *map_capacity, char *error_buf, uint32 error_buf_size)
{
    uint64 total = 0;
    uint64 map_size = 0;
    uint64 core_type_size =
        offsetof(WASMFuncType, types) + HOST_IMPORT_CORE_TYPE_MAX_VALUES;
    WASMHostImportCloneMapEntry *seen = NULL;
    bool success = false;

    if (!type_template || !size || !map_capacity
        || !host_import_clone_map_capacity(type_template->types_count,
                                           map_capacity)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Invalid host import type template\n");
        return false;
    }

    if (*map_capacity > 0) {
        map_size = (uint64)*map_capacity * sizeof(WASMHostImportCloneMapEntry);
        if (map_size > UINT32_MAX
            || !host_import_clone_add_size(&total, map_size)
            || !(seen = wasm_runtime_malloc((uint32)map_size))) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "ERROR: Failed to allocate host import type map\n");
            return false;
        }
        memset(seen, 0, (size_t)map_size);
    }

    for (uint32 type_idx = 0; type_idx < type_template->types_count;
         type_idx++) {
        WASMComponentTypeInstance *source = type_template->types[type_idx];
        uint64 specific_size;

        if (!source) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Invalid host import type graph\n");
            goto done;
        }
        if (!host_import_type_is_local(type_template, source)
            || host_import_clone_map_find_in(seen, *map_capacity, source)) {
            continue;
        }
        if (!host_import_clone_map_insert(seen, *map_capacity, source, source,
                                          HOST_IMPORT_CLONE_MAP_TYPE
                                              | HOST_IMPORT_CLONE_MAP_LOCAL)) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Host import type map is full\n");
            goto done;
        }

        if (!host_import_clone_specific_size(source, &specific_size)
            || !host_import_clone_add_size(&total,
                                           sizeof(WASMComponentTypeInstance))
            || (specific_size
                && !host_import_clone_add_size(&total, specific_size))) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Invalid host import type graph\n");
            goto done;
        }
    }

    for (uint32 func_idx = 0; func_idx < type_template->func_count;
         func_idx++) {
        if (!host_import_clone_add_size(&total, sizeof(WASMFunctionImport))
            || !host_import_clone_add_size(&total, core_type_size)) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Host import clone size overflow\n");
            goto done;
        }
    }

    if (total > UINT32_MAX) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Host import clone is too large\n");
        goto done;
    }

    *size = total;
    success = true;

done:
    if (seen) {
        wasm_runtime_free(seen);
    }
    return success;
}

static void *
host_import_clone_alloc(WASMHostImportCloneContext *clone_ctx, uint64 size)
{
    uint64 aligned_size;
    void *result;

    if (!clone_ctx || size > UINT64_MAX - (sizeof(void *) - 1)) {
        return NULL;
    }

    aligned_size = host_import_clone_align_size(size);
    if (aligned_size > clone_ctx->remaining) {
        return NULL;
    }

    result = clone_ctx->next;
    memset(result, 0, (size_t)aligned_size);
    clone_ctx->next += aligned_size;
    clone_ctx->remaining -= aligned_size;
    return result;
}

static bool
host_import_core_valtype_to_byte(const WASMComponentCoreValType *type,
                                 uint8 *byte)
{
    if (!type || !byte) {
        return false;
    }

    switch (type->tag) {
        case WASM_CORE_VALTYPE_NUM:
            *byte = (uint8)type->type.num_type;
            return true;
        case WASM_CORE_VALTYPE_VECTOR:
            *byte = (uint8)type->type.vector_type;
            return true;
        case WASM_CORE_VALTYPE_REF:
            *byte = (uint8)type->type.ref_type;
            return true;
        default:
            return false;
    }
}

static bool
host_import_build_core_func_type(
    WASMHostImportCloneContext *clone_ctx,
    WASMComponentFuncTypeInstance *component_func_type, WASMFuncType **result,
    char *error_buf, uint32 error_buf_size)
{
    CanonicalOptions canonical_options = { 0 };
    LiftLowerContext flatten_context = { 0 };
    WASMComponentCoreFuncType flattened = { 0 };
    WASMFuncType *core_func_type = NULL;
    uint32 total_value_count;
    uint32 param_cell_num;
    uint32 ret_cell_num;
    bool success = false;

    if (!clone_ctx || !component_func_type || !result) {
        return false;
    }

    flatten_context.canonical_opts = &canonical_options;
    flatten_context.inst = clone_ctx->instance;
    flatten_context.borrow_scope_type = BORROW_SCOPE_NONE;
    if (!flatten_functype(&flatten_context, component_func_type,
                          FLATTEN_CONTEXT_LOWER, &flattened)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Failed to flatten host function type\n");
        return false;
    }

    total_value_count = flattened.params.count + flattened.results.count;
    if (total_value_count > HOST_IMPORT_CORE_TYPE_MAX_VALUES
        || !(core_func_type = host_import_clone_alloc(
                 clone_ctx, offsetof(WASMFuncType, types)
                                + HOST_IMPORT_CORE_TYPE_MAX_VALUES))) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Failed to allocate host core function type\n");
        goto done;
    }

#if WASM_ENABLE_GC != 0
    core_func_type->base_type.type_flag = WASM_TYPE_FUNC;
#endif
    core_func_type->param_count = (uint16)flattened.params.count;
    core_func_type->result_count = (uint16)flattened.results.count;
    for (uint32 idx = 0; idx < flattened.params.count; idx++) {
        if (!host_import_core_valtype_to_byte(&flattened.params.val_types[idx],
                                              &core_func_type->types[idx])) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Invalid flattened host parameter type\n");
            goto done;
        }
    }
    for (uint32 idx = 0; idx < flattened.results.count; idx++) {
        if (!host_import_core_valtype_to_byte(
                &flattened.results.val_types[idx],
                &core_func_type->types[flattened.params.count + idx])) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Invalid flattened host result type\n");
            goto done;
        }
    }

    param_cell_num =
        wasm_get_cell_num(core_func_type->types, core_func_type->param_count);
    ret_cell_num =
        wasm_get_cell_num(core_func_type->types + core_func_type->param_count,
                          core_func_type->result_count);
    if (param_cell_num > UINT16_MAX || ret_cell_num > UINT16_MAX) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Flattened host function type is too large\n");
        goto done;
    }
    core_func_type->param_cell_num = (uint16)param_cell_num;
    core_func_type->ret_cell_num = (uint16)ret_cell_num;
    *result = core_func_type;
    success = true;

done:
    free_core_functype(&flattened);
    return success;
}

static WASMComponentTypeInstance *
host_import_find_type_clone(const WASMHostImportCloneContext *clone_ctx,
                            WASMComponentTypeInstance *source)
{
    WASMHostImportCloneMapEntry *entry;

    if (!clone_ctx || !source) {
        return NULL;
    }

    entry = host_import_clone_map_find(clone_ctx, source);
    return entry ? entry->clone : source;
}

static WASMComponentResourceInstance *
host_import_find_resource_clone(const WASMHostImportCloneContext *clone_ctx,
                                WASMComponentResourceInstance *source)
{
    WASMHostImportCloneMapEntry *entry;

    if (!clone_ctx || !source) {
        return NULL;
    }

    entry = host_import_clone_map_find(clone_ctx, source);
    return entry ? entry->clone : source;
}

static WASMComponentFuncTypeInstance *
host_import_find_func_type_clone(const WASMHostImportCloneContext *clone_ctx,
                                 WASMComponentFuncTypeInstance *source)
{
    WASMHostImportCloneMapEntry *entry;

    if (!clone_ctx || !source) {
        return NULL;
    }

    entry = host_import_clone_map_find(clone_ctx, source);
    return entry ? entry->clone : source;
}

static bool
host_import_clone_type(WASMHostImportCloneContext *clone_ctx,
                       const WASMComponentTypeInstance *source,
                       WASMComponentTypeInstance **result)
{
    WASMComponentTypeInstance *clone;
    uint64 specific_size;
    void *specific;

    if (!clone_ctx || !source || !result
        || !host_import_clone_specific_size(source, &specific_size)
        || !(clone = host_import_clone_alloc(
                 clone_ctx, sizeof(WASMComponentTypeInstance)))) {
        return false;
    }

    *clone = *source;
    if (!specific_size) {
        *result = clone;
        return true;
    }

    specific = host_import_clone_alloc(clone_ctx, specific_size);
    if (!specific) {
        return false;
    }

    switch (source->type) {
        case COMPONENT_VAL_TYPE_RECORD:
            memcpy(specific, source->type_specific.record,
                   (size_t)specific_size);
            clone->type_specific.record = specific;
            break;
        case COMPONENT_VAL_TYPE_VARIANT:
            memcpy(specific, source->type_specific.variant,
                   (size_t)specific_size);
            clone->type_specific.variant = specific;
            break;
        case COMPONENT_VAL_TYPE_LIST:
            memcpy(specific, source->type_specific.list, (size_t)specific_size);
            clone->type_specific.list = specific;
            break;
        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
            memcpy(specific, source->type_specific.list_len,
                   (size_t)specific_size);
            clone->type_specific.list_len = specific;
            break;
        case COMPONENT_VAL_TYPE_TUPLE:
        {
            WASMComponentTupleInstance *tuple = specific;

            memcpy(tuple, source->type_specific.tuple,
                   sizeof(WASMComponentTupleInstance));
            tuple->element_types =
                (WASMComponentTypeInstance **)((uint8 *)tuple + sizeof(*tuple));
            memcpy(tuple->element_types,
                   source->type_specific.tuple->element_types,
                   (size_t)source->type_specific.tuple->count
                       * sizeof(WASMComponentTypeInstance *));
            clone->type_specific.tuple = tuple;
            break;
        }
        case COMPONENT_VAL_TYPE_OPTION:
            memcpy(specific, source->type_specific.option,
                   (size_t)specific_size);
            clone->type_specific.option = specific;
            break;
        case COMPONENT_VAL_TYPE_RESULT:
            memcpy(specific, source->type_specific.result,
                   (size_t)specific_size);
            clone->type_specific.result = specific;
            break;
        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
            memcpy(specific, source->type_specific.resource_handle,
                   (size_t)specific_size);
            clone->type_specific.resource_handle = specific;
            break;
        case COMPONENT_VAL_TYPE_FUNCTION:
        {
            WASMComponentFuncTypeInstance *func_type = specific;
            WASMComponentParamListInstance *params =
                (WASMComponentParamListInstance *)((uint8 *)func_type
                                                   + sizeof(*func_type));
            uint64 params_size =
                sizeof(*params)
                + (uint64)source->type_specific.function->params->count
                      * sizeof(WASMComponentLabelValTypeInstance);
            WASMComponentResultListInstance *results =
                (WASMComponentResultListInstance *)((uint8 *)params
                                                    + params_size);

            memcpy(params, source->type_specific.function->params,
                   (size_t)params_size);
            memcpy(results, source->type_specific.function->results,
                   sizeof(*results));
            func_type->params = params;
            func_type->results = results;
            clone->type_specific.function = func_type;
            break;
        }
        case COMPONENT_VAL_TYPE_RESOURCE_SYNC:
        case COMPONENT_VAL_TYPE_RESOURCE_ASYNC:
        {
            WASMComponentResourceInstance *resource = specific;
            WASMFunctionInstance *drop_method =
                (WASMFunctionInstance *)((uint8 *)resource + sizeof(*resource));
            WASMFunctionInstance *new_method = drop_method + 1;
            WASMFunctionInstance *rep_method = new_method + 1;

            *resource = *source->type_specific.resource;
            if (resource->drop_method) {
                *drop_method = *resource->drop_method;
                drop_method->resource = resource;
            }
            if (resource->new_method) {
                *new_method = *resource->new_method;
                new_method->resource = resource;
            }
            if (resource->rep_method) {
                *rep_method = *resource->rep_method;
                rep_method->resource = resource;
            }
            resource->drop_method = resource->drop_method ? drop_method : NULL;
            resource->new_method = resource->new_method ? new_method : NULL;
            resource->rep_method = resource->rep_method ? rep_method : NULL;
            clone->type_specific.resource = resource;
            break;
        }
        default:
            return false;
    }

    *result = clone;
    return true;
}

static void
host_import_remap_type(WASMHostImportCloneContext *clone_ctx,
                       WASMComponentTypeInstance *type)
{
    if (!clone_ctx || !type) {
        return;
    }

    switch (type->type) {
        case COMPONENT_VAL_TYPE_RECORD:
            for (uint32 idx = 0; idx < type->type_specific.record->count;
                 idx++) {
                type->type_specific.record->fields[idx].type =
                    host_import_find_type_clone(
                        clone_ctx,
                        type->type_specific.record->fields[idx].type);
            }
            break;
        case COMPONENT_VAL_TYPE_VARIANT:
            for (uint32 idx = 0; idx < type->type_specific.variant->count;
                 idx++) {
                type->type_specific.variant->cases[idx].value_type =
                    host_import_find_type_clone(
                        clone_ctx,
                        type->type_specific.variant->cases[idx].value_type);
            }
            break;
        case COMPONENT_VAL_TYPE_LIST:
            type->type_specific.list->element_type =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.list->element_type);
            break;
        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
            type->type_specific.list_len->element_type =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.list_len->element_type);
            break;
        case COMPONENT_VAL_TYPE_TUPLE:
            for (uint32 idx = 0; idx < type->type_specific.tuple->count;
                 idx++) {
                type->type_specific.tuple->element_types[idx] =
                    host_import_find_type_clone(
                        clone_ctx,
                        type->type_specific.tuple->element_types[idx]);
            }
            break;
        case COMPONENT_VAL_TYPE_OPTION:
            type->type_specific.option->element_type =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.option->element_type);
            break;
        case COMPONENT_VAL_TYPE_RESULT:
            type->type_specific.result->result_type =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.result->result_type);
            type->type_specific.result->error_type =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.result->error_type);
            break;
        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
            type->type_specific.resource_handle->resource =
                host_import_find_resource_clone(
                    clone_ctx, type->type_specific.resource_handle->resource);
            break;
        case COMPONENT_VAL_TYPE_FUNCTION:
            for (uint32 idx = 0;
                 idx < type->type_specific.function->params->count; idx++) {
                type->type_specific.function->params->params[idx].type =
                    host_import_find_type_clone(
                        clone_ctx,
                        type->type_specific.function->params->params[idx].type);
            }
            type->type_specific.function->results->result =
                host_import_find_type_clone(
                    clone_ctx, type->type_specific.function->results->result);
            break;
        default:
            break;
    }
}

static bool
host_import_clone_types(WASMHostImportCloneContext *clone_ctx, char *error_buf,
                        uint32 error_buf_size)
{
    if (!clone_ctx || !clone_ctx->type_template || !clone_ctx->instance) {
        return false;
    }

    if (clone_ctx->map_capacity > 0) {
        clone_ctx->map = host_import_clone_alloc(
            clone_ctx, (uint64)clone_ctx->map_capacity
                           * sizeof(WASMHostImportCloneMapEntry));
        if (!clone_ctx->map) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "ERROR: Failed to allocate host import type map\n");
            return false;
        }
    }

    for (uint32 type_idx = 0; type_idx < clone_ctx->type_template->types_count;
         type_idx++) {
        WASMComponentTypeInstance *source =
            clone_ctx->type_template->types[type_idx];
        WASMHostImportCloneMapEntry *entry =
            host_import_clone_map_find(clone_ctx, source);
        WASMComponentTypeInstance *clone = NULL;
        bool is_local;

        if (entry) {
            clone = entry->clone;
        }
        else {
            is_local =
                host_import_type_is_local(clone_ctx->type_template, source);
            if (is_local
                && !host_import_clone_type(clone_ctx, source, &clone)) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Failed to clone host import types\n");
                return false;
            }
            if (!is_local) {
                clone = source;
            }
            if (!host_import_clone_map_insert(
                    clone_ctx->map, clone_ctx->map_capacity, source, clone,
                    HOST_IMPORT_CLONE_MAP_TYPE
                        | (is_local ? HOST_IMPORT_CLONE_MAP_LOCAL : 0))) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Host import type map is full\n");
                return false;
            }
            if ((source->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                 || source->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)
                && !host_import_clone_map_insert(
                    clone_ctx->map, clone_ctx->map_capacity,
                    source->type_specific.resource,
                    clone->type_specific.resource,
                    is_local ? HOST_IMPORT_CLONE_MAP_LOCAL : 0)) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Host resource type map is full\n");
                return false;
            }
            if (source->type == COMPONENT_VAL_TYPE_FUNCTION
                && !host_import_clone_map_insert(
                    clone_ctx->map, clone_ctx->map_capacity,
                    source->type_specific.function,
                    clone->type_specific.function,
                    is_local ? HOST_IMPORT_CLONE_MAP_LOCAL : 0)) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Host function type map is full\n");
                return false;
            }
        }
        clone_ctx->instance->types[type_idx] = clone;
    }

    clone_ctx->instance->types_count = clone_ctx->type_template->types_count;
    for (uint32 map_idx = 0; map_idx < clone_ctx->map_capacity; map_idx++) {
        WASMHostImportCloneMapEntry *entry = &clone_ctx->map[map_idx];

        if (entry->source
            && (entry->flags
                & (HOST_IMPORT_CLONE_MAP_TYPE | HOST_IMPORT_CLONE_MAP_LOCAL))
                   == (HOST_IMPORT_CLONE_MAP_TYPE
                       | HOST_IMPORT_CLONE_MAP_LOCAL)) {
            host_import_remap_type(clone_ctx, entry->clone);
        }
    }

    return true;
}

static uint32
host_import_bind_resources(WASMHostImportCloneContext *clone_ctx,
                           WASMComponentInstance *root_instance,
                           char *interface_name)
{
    uint32 resource_count = 0;

    for (uint32 map_idx = 0; map_idx < clone_ctx->map_capacity; map_idx++) {
        WASMHostImportCloneMapEntry *entry = &clone_ctx->map[map_idx];
        WASMComponentTypeInstance *type;
        WASMComponentResourceInstance *resource;
        wasm_component_host_resource_drop_callback_t drop_callback = NULL;

        if (!entry->source
            || (entry->flags
                & (HOST_IMPORT_CLONE_MAP_TYPE | HOST_IMPORT_CLONE_MAP_LOCAL))
                   != (HOST_IMPORT_CLONE_MAP_TYPE
                       | HOST_IMPORT_CLONE_MAP_LOCAL)) {
            continue;
        }
        type = entry->clone;
        if (!type
            || (type->type != COMPONENT_VAL_TYPE_RESOURCE_SYNC
                && type->type != COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
            continue;
        }

        resource = type->type_specific.resource;
        if (!resource->interface_name) {
            resource->interface_name = interface_name;
            resource_count++;
        }
        resource->is_builtin_wasi = false;
        resource->is_host = true;
        resource->host_drop_callback = NULL;
        resource->host_drop_attachment = NULL;
        resource->host_drop_attachment_is_custom_data = false;
        if (resource->name
            && wasm_component_find_host_resource_drop_callback(
                root_instance->component, resource->interface_name,
                resource->name, &drop_callback)) {
            resource->host_drop_callback = drop_callback;
            resource->host_drop_attachment = root_instance->custom_data;
            resource->host_drop_attachment_is_custom_data = true;
        }
    }

    return resource_count;
}

static void
host_import_set_builtin_wasi_resource_provenance(
    WASMHostImportCloneContext *clone_ctx, bool allow_builtin_wasi)
{
    for (uint32 map_idx = 0; map_idx < clone_ctx->map_capacity; map_idx++) {
        WASMHostImportCloneMapEntry *entry = &clone_ctx->map[map_idx];
        WASMComponentTypeInstance *type;

        if (!entry->source
            || (entry->flags
                & (HOST_IMPORT_CLONE_MAP_TYPE | HOST_IMPORT_CLONE_MAP_LOCAL))
                   != (HOST_IMPORT_CLONE_MAP_TYPE
                       | HOST_IMPORT_CLONE_MAP_LOCAL)) {
            continue;
        }
        type = entry->clone;
        if (type
            && (type->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                || type->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
            WASMComponentResourceInstance *resource =
                type->type_specific.resource;
#if WASM_ENABLE_LIBC_WASI != 0
            resource->is_builtin_wasi =
                allow_builtin_wasi
                && wasm_native_has_builtin_wasi_p2_resource(
                    resource->interface_name, resource->name);
#else
            resource->is_builtin_wasi = false;
#endif
        }
    }
}

bool
wasm_resolve_imports_host(WASMComponentImportSection *import_section,
                          WASMComponentInstance *comp_instance, char *error_buf,
                          uint32 error_buf_size)
{
    char *interface_name = NULL;
    uint32 import_idx = 0;
    WASMComponentImport *import = NULL;
    WASMComponentInstTypeInstance *instance_type = NULL;
    WASMComponentInstance *new_inst = NULL;
    void *func_ptr = NULL;
    char *field_name = NULL;

    if (!import_section || !comp_instance) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Invalid root component imports\n");
        return false;
    }

    for (import_idx = 0; import_idx < import_section->count; import_idx++) {
        uint32 func_export_count = 0;
        uint32 resource_count;
        uint32 clone_map_capacity;
        uint64 clone_storage_size;
        bool is_wasi = false;
#if WASM_ENABLE_LIBC_WASI != 0
        bool resolved_static_host_function = false;
#endif
        bool resources_use_builtin_wasi = false;
        WASMHostImportCloneContext clone_ctx = { 0 };

        import = &import_section->imports[import_idx];
        if (!import->extern_desc
            || import->extern_desc->type != WASM_COMP_EXTERN_INSTANCE) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "ERROR: Root component import not a component instance\n");
            return false;
        }
        if (import->import_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
            interface_name = import->import_name->imported.simple.name->name;
        }
        else if (import->import_name->tag == WASM_COMP_IMPORTNAME_VERSIONED) {
            interface_name = import->import_name->imported.versioned.name->name;
        }
        else {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Component import tag %d not supported\n",
                             import->import_name->tag);
            return false;
        }
        is_wasi = strncmp(interface_name, "wasi:", strlen("wasi:")) == 0;

        LOG_DEBUG("  Host Import : %s", interface_name);

        if (import->extern_desc->extern_desc.instance.type_idx
                >= comp_instance->types_count
            || comp_instance
                       ->types[import->extern_desc->extern_desc.instance
                                   .type_idx]
                       ->type
                   != COMPONENT_VAL_TYPE_INSTANCE) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "ERROR: Invalid instance type index %d\n",
                import->extern_desc->extern_desc.instance.type_idx);
            return false;
        }
        instance_type =
            comp_instance
                ->types[import->extern_desc->extern_desc.instance.type_idx]
                ->type_specific.instance;
        WASMComponentIndexCount index_count = { 0 };
        index_count.types = instance_type->types_count;
        index_count.functions = instance_type->func_count;
        index_count.exports = instance_type->exports_count;
        index_count.defined_core_functions = instance_type->func_count;
        index_count.defined_functions = instance_type->func_count;
        if (!host_import_clone_storage_size(instance_type, &clone_storage_size,
                                            &clone_map_capacity, error_buf,
                                            error_buf_size)) {
            return false;
        }
        index_count.types_total_size = clone_storage_size;
        new_inst = wasm_component_instance_allocate(&index_count, error_buf,
                                                    error_buf_size);
        if (!new_inst) {
            return false;
        }

        clone_ctx.type_template = instance_type;
        clone_ctx.instance = new_inst;
        clone_ctx.next = new_inst->defined_types;
        clone_ctx.remaining = clone_storage_size;
        clone_ctx.map_capacity = clone_map_capacity;
        if (!host_import_clone_types(&clone_ctx, error_buf, error_buf_size)) {
            wasm_component_deinstantiate(new_inst);
            return false;
        }
        resource_count = host_import_bind_resources(&clone_ctx, comp_instance,
                                                    interface_name);

        for (uint32 func_idx = 0; func_idx < instance_type->func_count;
             func_idx++) {
            new_inst->defined_functions[new_inst->defined_functions_count]
                .func_type = host_import_find_func_type_clone(
                &clone_ctx, instance_type->funcs[func_idx]);
            new_inst->defined_functions[new_inst->defined_functions_count]
                .core_func = NULL;
            new_inst->functions[new_inst->functions_count] =
                &new_inst->defined_functions[new_inst->defined_functions_count];
            new_inst->defined_functions_count++;
            new_inst->functions_count++;
        }
        for (uint32 export_idx = 0; export_idx < instance_type->exports_count;
             export_idx++) {
            new_inst->exports[export_idx].export_name =
                instance_type->exports[export_idx].export_name;
            new_inst->exports[export_idx].type =
                instance_type->exports[export_idx].type;
            if (instance_type->exports[export_idx].type
                == WASM_COMP_EXTERN_FUNC) {
#if WASM_ENABLE_LIBC_WASI != 0
                const NativeSymbol *wasi_symbol = NULL;
#endif
                if (func_export_count >= new_inst->functions_count) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Import function index exceeded\n");
                    wasm_component_deinstantiate(new_inst);
                    return false;
                }
                WASMFunctionInstance *template_core_func =
                    &instance_type->defined_core_funcs[func_export_count];
                WASMFunctionInstance *core_func =
                    &new_inst->defined_core_functions[func_export_count];
                WASMFunctionImport *func_import = host_import_clone_alloc(
                    &clone_ctx, sizeof(WASMFunctionImport));
                if (!template_core_func->u.func_import || !func_import) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Failed to clone host function import\n");
                    wasm_component_deinstantiate(new_inst);
                    return false;
                }
                *func_import = *template_core_func->u.func_import;
                func_import->func_ptr_linked = NULL;
                func_import->signature = NULL;
                func_import->attachment = NULL;
                func_import->call_conv_raw = false;
                func_import->call_conv_wasm_c_api = false;
#if WASM_ENABLE_MULTI_MODULE != 0
                func_import->import_module = NULL;
                func_import->import_func_linked = NULL;
#endif
                *core_func = *template_core_func;
                core_func->is_import_func = true;
                core_func->u.func_import = func_import;
#if WASM_ENABLE_MULTI_MODULE != 0 || WASM_ENABLE_COMPONENT_MODEL != 0
                core_func->import_module_inst = NULL;
                core_func->import_func_inst = NULL;
#endif
                core_func->canon_options = NULL;
                core_func->module_instance = NULL;
                core_func->component_function =
                    &new_inst->defined_functions[func_export_count];
                if (!host_import_build_core_func_type(
                        &clone_ctx, core_func->component_function->func_type,
                        &func_import->func_type, error_buf, error_buf_size)) {
                    wasm_component_deinstantiate(new_inst);
                    return false;
                }
                core_func->param_count = func_import->func_type->param_count;
                core_func->param_cell_num =
                    func_import->func_type->param_cell_num;
                core_func->ret_cell_num = func_import->func_type->ret_cell_num;
                core_func->param_types = func_import->func_type->types;
                new_inst->defined_core_functions_count++;
                field_name = instance_type->exports[export_idx]
                                 .export_name->exported.simple.name->name;
#if WASM_ENABLE_LIBC_WASI != 0
                wasi_symbol = is_wasi ? wasm_native_get_wasi_p2_module_func(
                                  interface_name, field_name)
                                      : NULL;
#endif
                func_ptr = wasm_native_resolve_symbol_exact(
                    interface_name, field_name, func_import->func_type,
                    &func_import->signature, &func_import->attachment,
                    &func_import->call_conv_raw);
#if WASM_ENABLE_LIBC_WASI != 0
                if (func_ptr
                    && (!wasi_symbol || func_ptr != wasi_symbol->func_ptr)) {
                    resolved_static_host_function = true;
                }
#endif

                if (!func_ptr && is_wasi) {
#if WASM_ENABLE_LIBC_WASI != 0
                    if (!wasm_check_wasi_p2_version(interface_name)) {
                        set_error_buf_ex(
                            error_buf, error_buf_size,
                            "ERROR: Incompatible WASI version for %s",
                            interface_name);
                        wasm_component_deinstantiate(new_inst);
                        return false;
                    }
                    if (wasi_symbol
                        && wasm_native_validate_symbol_signature(
                            func_import->func_type, wasi_symbol->signature)) {
                        func_ptr = wasi_symbol->func_ptr;
                        func_import->signature = wasi_symbol->signature;
                        func_import->attachment = wasi_symbol->attachment;
                        func_import->call_conv_raw = false;
                    }
#endif
                }

                if (!func_ptr) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Function %s not found in native symbols\n",
                        field_name);
                    wasm_component_deinstantiate(new_inst);
                    return false;
                }
                LOG_DEBUG(
                    "  Host function : %s %s found successfully, %d params",
                    interface_name, field_name,
                    instance_type->defined_core_funcs[func_export_count]
                        .param_count);
                new_inst->defined_functions[func_export_count].core_func =
                    core_func;
                func_import->func_ptr_linked = func_ptr;
                new_inst->exports[export_idx].exp.function =
                    &new_inst->defined_functions[func_export_count];
                func_export_count++;
                new_inst->exports_count++;
            }
            else if (instance_type->exports[export_idx].type
                     == WASM_COMP_EXTERN_TYPE) {
                new_inst->exports[export_idx].exp.type =
                    host_import_find_type_clone(
                        &clone_ctx,
                        instance_type->exports[export_idx].exp.type);
                new_inst->exports_count++;
            }
            else {
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "ERROR: Import type %d not supported for host imports\n",
                    instance_type->exports[export_idx].type);
                wasm_component_deinstantiate(new_inst);
                return false;
            }
        }

        /* A wasi: namespace does not imply ownership by WAMR's global host
         * resource table. Only an interface implemented exclusively by the
         * built-in WASI fallback may use that table during resource.drop.
         * Resource-only and mixed static/built-in interfaces fail closed and
         * require an exact owner-drop callback. */
#if WASM_ENABLE_LIBC_WASI != 0
        resources_use_builtin_wasi = is_wasi && !resolved_static_host_function;
#endif
        host_import_set_builtin_wasi_resource_provenance(
            &clone_ctx, resources_use_builtin_wasi);

        comp_instance->resources_count += resource_count;
        comp_instance
            ->component_instances[comp_instance->component_instances_count] =
            new_inst;
        comp_instance->component_instances_count++;
        comp_instance
            ->defined_instances[comp_instance->defined_instances_count] =
            new_inst;
        comp_instance->defined_instances_count++;
        new_inst = NULL;
    }
    return true;
}

bool
wasm_resolve_imports_WASI(WASMComponentImportSection *import_section,
                          WASMComponentInstance *comp_instance, char *error_buf,
                          uint32 error_buf_size)
{
    return wasm_resolve_imports_host(import_section, comp_instance, error_buf,
                                     error_buf_size);
}

bool
wasm_resolve_imports(WASMComponentImportSection *import_section,
                     WASMComponentInstance *comp_instance,
                     WASMComponentInstArgInstances *instance_expression,
                     char *error_buf, uint32 error_buf_size)
{
    const char *import_name = NULL;
    const char *arg_name = NULL;
    uint32 arg_idx = 0;
    uint32 idx = 0;
    WASMComponentImport *import = NULL;

    for (idx = 0; idx < import_section->count; idx++) {
        import = &import_section->imports[idx];

        if (import->import_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
            import_name = import->import_name->imported.simple.name->name;
        }
        else if (import->import_name->tag == WASM_COMP_IMPORTNAME_VERSIONED) {
            import_name = import->import_name->imported.versioned.name->name;
        }
        else {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Component import tag %d not supported\n",
                             import->import_name->tag);
            return false;
        }
        LOG_DEBUG("  Import : %s", import_name);
        WASMComponentInstArgInstance *inst_arg = NULL, *curr_inst_arg = NULL;
        for (arg_idx = 0; arg_idx < instance_expression->arg_len; arg_idx++) {
            curr_inst_arg = &instance_expression->args[arg_idx];
            if (!curr_inst_arg) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Invaid argument list\n");
                return false;
            }
            arg_name = curr_inst_arg->name->name;
            if (!strcmp(import_name, arg_name)) {
                if (import->extern_desc->type
                    != curr_inst_arg->sort_idx->sort->sort) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "Import <%s> sort doesn't match",
                                     import_name);
                    return false;
                }
                inst_arg = curr_inst_arg;
                break;
            }
        }
        if (!inst_arg) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "Import not found in arguments list\n");
            return false;
        }
        switch (inst_arg->sort_idx->sort->sort) {
            case WASM_COMP_SORT_CORE_SORT:
                if (inst_arg->sort_idx->sort->core_sort != 0x11) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: The only allowed core sort is "
                                     "core module, sort %d not allowed",
                                     inst_arg->sort_idx->sort->core_sort);
                    return false;
                }
                LOG_DEBUG("Core module import added");
                comp_instance->core_modules[comp_instance->core_modules_count] =
                    inst_arg->arg.core_module;
                comp_instance->core_modules_count++;
                break;
            case WASM_COMP_SORT_FUNC:
                comp_instance->functions[comp_instance->functions_count] =
                    inst_arg->arg.function;
                comp_instance->functions_count++;
                LOG_DEBUG("Function import added");
                break;
            case WASM_COMP_SORT_VALUE:
                LOG_DEBUG("Value import added");
                comp_instance->values[comp_instance->values_count] =
                    inst_arg->arg.value;
                comp_instance->values_count++;
                break;
            case WASM_COMP_SORT_TYPE:
                LOG_DEBUG("Type import added");
                // Validate type bound: if the import declares (type (sub
                // resource)), verify the provided type is actually a resource
                // type
                if (import->extern_desc->type == WASM_COMP_EXTERN_TYPE) {
                    const WASMComponentTypeBound *tb =
                        import->extern_desc->extern_desc.type.type_bound;
                    if (tb && tb->tag == WASM_COMP_TYPEBOUND_TYPE) {
                        if (!inst_arg->arg.type
                            || (inst_arg->arg.type->type
                                    != COMPONENT_VAL_TYPE_RESOURCE_SYNC
                                && inst_arg->arg.type->type
                                       != COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
                            set_error_buf_ex(
                                error_buf, error_buf_size,
                                "Type import <%s> has (sub resource) bound but "
                                "argument is not a resource type",
                                import_name);
                            return false;
                        }
                    }
                }
                comp_instance->types[comp_instance->types_count] =
                    inst_arg->arg.type;
                comp_instance->types_count++;
                break;
            case WASM_COMP_SORT_COMPONENT:
                LOG_DEBUG("Component import added");
                comp_instance->components[comp_instance->components_count] =
                    inst_arg->arg.component;
                comp_instance->components_count++;
                break;
            case WASM_COMP_SORT_INSTANCE:
                LOG_DEBUG("Instance import added");
                comp_instance->component_instances
                    [comp_instance->component_instances_count] =
                    inst_arg->arg.instance;
                comp_instance->component_instances_count++;
                break;
            default:
                break;
        }
    }
    return true;
}
