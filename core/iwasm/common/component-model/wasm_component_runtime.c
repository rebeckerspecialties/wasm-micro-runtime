/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#if WASM_ENABLE_COMPONENT_MODEL != 0
#include "wasm.h"
#include "wasm_runtime.h"
#include "wasm_component.h"
#include "wasm_loader.h"
#include "wasm_component_runtime.h"
#include "wasm_component_canon.h"
#include "bh_log.h"
#include "stdio.h"
#include "wasm_component_canonical.h"
#include "wasm_component_task.h"
#include "bh_assert.h"
#include "../../libraries/libc-wasi-p2/wasi_p2_cli.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"

/// @brief Compute discriminant alignment based on number of cases
uint32_t
compute_discriminant_alignment(uint32_t num_cases)
{
    if (num_cases <= 256)
        return 1; // u8
    else if (num_cases <= 65536)
        return 2; // u16
    else
        return 4; // u32
}

uint32_t
compute_max_case_alignment(WASMComponentVariantInstance *type)
{
    uint32_t max_case_align = 1;
    for (uint32_t i = 0; i < type->count; i++) {
        if (type->cases[i].value_type) {
            uint32_t case_align = compute_alignment(type->cases[i].value_type);
            if (case_align > max_case_align) {
                max_case_align = case_align;
            }
        }
    }

    return max_case_align;
}

/// @brief Compute alignment for primitive values
uint32_t
compute_alignment_primitive_value(WASMComponentPrimValType primval)
{
    switch (primval) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            return 1;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
            return 2;
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_F32:
        case WASM_COMP_PRIMVAL_CHAR:
        case WASM_COMP_PRIMVAL_STRING:
        case WASM_COMP_PRIMVAL_ERROR_CONTEXT:
            return 4;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
        case WASM_COMP_PRIMVAL_F64:
            return 8;
        default:
            return 1;
    }
}

/// @brief Calculate alignment size type
/// @param type
/// @return uint32_t alignment computed
uint32_t
compute_alignment(WASMComponentTypeInstance *type)
{
    switch (type->type) {
        case COMPONENT_VAL_TYPE_PRIMVAL:
            // Look at which primval it is
            return compute_alignment_primitive_value(
                type->type_specific.primval);

        case COMPONENT_VAL_TYPE_LIST:
        {
            return 4;
        }

        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
        {
            WASMComponentListLenInstance *list_len =
                type->type_specific.list_len;
            return compute_alignment(list_len->element_type);
        }

        case COMPONENT_VAL_TYPE_RECORD:
        {
            // For records, alignment = max alignment of all fields
            uint32_t max_align = 1;
            WASMComponentTypeInstance *val_type = NULL;
            for (uint32_t index = 0; index < type->type_specific.record->count;
                 index++) {
                val_type = type->type_specific.record->fields[index].type;
                uint32_t field_align = compute_alignment(val_type);
                if (field_align > max_align) {
                    max_align = field_align;
                }
            }

            return max_align;
        }

        case COMPONENT_VAL_TYPE_TUPLE:
        {
            // For tuples, alignment = max alignment of all elements
            uint32_t max_align = 1;
            WASMComponentTupleInstance *tuple = type->type_specific.tuple;
            for (uint32_t i = 0; i < tuple->count; i++) {
                uint32_t elem_align =
                    compute_alignment(tuple->element_types[i]);
                if (elem_align > max_align) {
                    max_align = elem_align;
                }
            }
            return max_align;
        }

        case COMPONENT_VAL_TYPE_FLAGS:
        {
            uint32_t n = type->type_specific.flag->count;

            bh_assert((0 < n) && (n <= 32));

            if (n <= 8)
                return 1;
            if (n <= 16)
                return 2;

            return 4;
        }

        case COMPONENT_VAL_TYPE_VARIANT:
        {
            uint32_t n = type->type_specific.variant->count;

            // Compute discriminant alignment
            uint32_t disc_align = compute_discriminant_alignment(n);

            // Compute max case alignment
            uint32_t max_case_align =
                compute_max_case_alignment(type->type_specific.variant);

            return (disc_align > max_case_align) ? disc_align : max_case_align;
        }

        case COMPONENT_VAL_TYPE_OPTION:
        {
            // Option = variant with 2 cases: none (empty), some (element_type)
            WASMComponentOptionInstance *option = type->type_specific.option;

            // Discriminant for 2 cases = u8 = alignment 1
            uint32_t disc_align = 1;

            // Max case alignment = max(none, some)
            // none is empty (align 1), some has element_type
            uint32_t some_align = compute_alignment(option->element_type);

            return (disc_align > some_align) ? disc_align : some_align;
        }

        case COMPONENT_VAL_TYPE_RESULT:
        {
            // Result = variant with 2 cases: ok (result_type), error
            // (error_type)
            WASMComponentResultInstance *result = type->type_specific.result;

            // Discriminant for 2 cases = u8 = alignment 1
            uint32_t disc_align = 1;

            // Max case alignment = max(ok, error)
            uint32_t ok_align = 1;
            uint32_t err_align = 1;

            if (result->result_type) {
                ok_align = compute_alignment(result->result_type);
            }
            if (result->error_type) {
                err_align = compute_alignment(result->error_type);
            }

            uint32_t max_case_align =
                (ok_align > err_align) ? ok_align : err_align;

            return (disc_align > max_case_align) ? disc_align : max_case_align;
        }

        case COMPONENT_VAL_TYPE_ENUM:
        {
            // Enum = variant where all cases are empty
            // So alignment = discriminant alignment only
            WASMComponentEnumType *enum_type = type->type_specific.enum_type;
            return compute_discriminant_alignment(enum_type->count);
        }

        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
        case COMPONENT_VAL_TYPE_STREAM:
        case COMPONENT_VAL_TYPE_FUTURE:
        {
            return 4; // All are i32 handles
        }

        default:
            return 0;
    }
}

/// @brief Compute elem size for primitive values
uint32_t
compute_elem_size_primitive_value(WASMComponentPrimValType primval)
{
    switch (primval) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            return 1;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
            return 2;
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_F32:
        case WASM_COMP_PRIMVAL_CHAR:
        case WASM_COMP_PRIMVAL_ERROR_CONTEXT:
            return 4;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
        case WASM_COMP_PRIMVAL_F64:
        case WASM_COMP_PRIMVAL_STRING:
            return 8;
        default:
            return 1;
    }
}

/// @brief Calculate element size
/// @param type
/// @return uint32_t elem size computed
uint32_t
compute_elem_size(WASMComponentTypeInstance *type)
{
    switch (type->type) {
        case COMPONENT_VAL_TYPE_PRIMVAL:
            // Look at which primval it is
            return compute_elem_size_primitive_value(
                type->type_specific.primval);

        case COMPONENT_VAL_TYPE_LIST:
        {
            return 8;
        }

        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
        {
            WASMComponentListLenInstance *list_len =
                type->type_specific.list_len;
            return list_len->len * compute_elem_size(list_len->element_type);
        }

        case COMPONENT_VAL_TYPE_RECORD:
        {
            // For records, alignment = max alignment of all fields
            uint32_t s = 0;
            WASMComponentTypeInstance *val_type = NULL;
            for (uint32_t index = 0; index < type->type_specific.record->count;
                 index++) {
                val_type = type->type_specific.record->fields[index].type;
                s = align_to(s, compute_alignment(val_type));
                s += compute_elem_size(val_type);
            }

            assert(s > 0);
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_TUPLE:
        {
            // Tuple despecializes to record, same elem_size algorithm
            uint32_t s = 0;
            WASMComponentTupleInstance *tuple = type->type_specific.tuple;

            for (uint32_t i = 0; i < tuple->count; i++) {
                WASMComponentTypeInstance *elem = tuple->element_types[i];
                s = align_to(
                    s, compute_alignment(elem)); // Align to element's alignment
                s += compute_elem_size(elem);    // Add element's size
            }

            bh_assert(s > 0); // Tuples can't be empty
            return align_to(
                s,
                type->alignment); // Final padding to tuple's overall alignment
        }

        case COMPONENT_VAL_TYPE_FLAGS:
        {
            uint32_t n = type->type_specific.flag->count;

            bh_assert((0 < n) && (n <= 32));

            if (n <= 8)
                return 1;
            if (n <= 16)
                return 2;

            return 4;
        }

        case COMPONENT_VAL_TYPE_VARIANT:
        {
            WASMComponentVariantInstance *variant = type->type_specific.variant;

            // 1. Discriminant size (depends on number of cases)
            uint32_t s = compute_discriminant_alignment(
                variant->count); // Returns size (1, 2, or 4)

            // 2. Find max case alignment and max case size
            uint32_t max_case_align = 1;
            uint32_t max_case_size = 0;

            for (uint32_t i = 0; i < variant->count; i++) {
                if (variant->cases[i].value_type) {
                    uint32_t case_align =
                        compute_alignment(variant->cases[i].value_type);
                    uint32_t case_size =
                        compute_elem_size(variant->cases[i].value_type);

                    if (case_align > max_case_align) {
                        max_case_align = case_align;
                    }
                    if (case_size > max_case_size) {
                        max_case_size = case_size;
                    }
                }
            }

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size (largest payload among all cases)
            s += max_case_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_OPTION:
        {
            // Option = variant with 2 cases: none (empty), some (element_type)
            WASMComponentOptionInstance *option = type->type_specific.option;

            // 1. Discriminant size (2 cases = u8 = 1 byte)
            uint32_t s = 1;

            // 2. Max case alignment: max(none=1, some=element_align)
            uint32_t some_align = compute_alignment(option->element_type);
            uint32_t max_case_align = (some_align > 1) ? some_align : 1;

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size (only "some" has payload)
            uint32_t some_size = compute_elem_size(option->element_type);
            s += some_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_RESULT:
        {
            // Result = variant with 2 cases: ok (result_type), error
            // (error_type)
            WASMComponentResultInstance *result = type->type_specific.result;

            // 1. Discriminant size (2 cases = u8 = 1 byte)
            uint32_t s = 1;

            // 2. Max case alignment: max(ok_align, error_align)
            uint32_t ok_align = 1, err_align = 1;
            uint32_t ok_size = 0, err_size = 0;

            if (result->result_type) {
                ok_align = compute_alignment(result->result_type);
                ok_size = compute_elem_size(result->result_type);
            }
            if (result->error_type) {
                err_align = compute_alignment(result->error_type);
                err_size = compute_elem_size(result->error_type);
            }

            uint32_t max_case_align =
                (ok_align > err_align) ? ok_align : err_align;
            uint32_t max_case_size = (ok_size > err_size) ? ok_size : err_size;

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size
            s += max_case_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_ENUM:
        {
            // Enum = variant with all cases empty
            // elem_size = discriminant size only (no payloads)
            WASMComponentEnumType *enum_type = type->type_specific.enum_type;
            return compute_discriminant_alignment(enum_type->count);
        }

        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
        case COMPONENT_VAL_TYPE_STREAM:
        case COMPONENT_VAL_TYPE_FUTURE:
        {
            return 4; // All are i32 handles
        }

        default:
            return 0;
    }
}

static bool
component_layout_add(uint64 *total, uint64 count, uint64 element_size)
{
    if (!total
        || (element_size != 0
            && count > (UINT64_MAX - *total) / element_size)) {
        return false;
    }
    *total += count * element_size;
    return true;
}

bool
wasm_get_def_val_type_size(WASMComponentDefValType *def_val_type, uint64 *size)
{
    uint64 total = 0;

    if (!def_val_type || !size
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentTypeInstance))) {
        return false;
    }

    switch (def_val_type->tag) {
        case WASM_COMP_DEF_VAL_PRIMVAL: // pvt:<primvaltype>
        // Flags type
        case WASM_COMP_DEF_VAL_FLAGS: // 0x6e l*:vec(<label'>)
        // Enum type
        case WASM_COMP_DEF_VAL_ENUM: // 0x6d l*:vec(<label'>)
            // Flags and Enums are fully defined in Component binary, require no
            // additional linking in Instance
            break;
        // Record type (labeled fields)
        case WASM_COMP_DEF_VAL_RECORD: // 0x72 lt*:vec(<labelvaltype>)
            if (!def_val_type->def_val.record
                || (def_val_type->def_val.record->count > 0
                    && !def_val_type->def_val.record->fields)
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentRecordInstance))
                || !component_layout_add(
                    &total, def_val_type->def_val.record->count,
                    sizeof(WASMComponentLabelValTypeInstance))) {
                return false;
            }
            break;
        // Variant type (labeled cases)
        case WASM_COMP_DEF_VAL_VARIANT: // 0x71 case*:vec(<case>)
            if (!def_val_type->def_val.variant
                || (def_val_type->def_val.variant->count > 0
                    && !def_val_type->def_val.variant->cases)
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentVariantInstance))
                || !component_layout_add(
                    &total, def_val_type->def_val.variant->count,
                    sizeof(WASMComponentCaseValInstance))) {
                return false;
            }
            break;
        // List types
        case WASM_COMP_DEF_VAL_LIST: // 0x70 t:<valtype>
            if (!def_val_type->def_val.list
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentListInstance))) {
                return false;
            }
            break;
        case WASM_COMP_DEF_VAL_LIST_LEN: // 0x67 t:<valtype> len:<u32>
            if (!def_val_type->def_val.list_len
                || !component_layout_add(
                    &total, 1, sizeof(WASMComponentListLenInstance))) {
                return false;
            }
            break;
        // Tuple type
        case WASM_COMP_DEF_VAL_TUPLE: // 0x6f t*:vec(<valtype>)
            if (!def_val_type->def_val.tuple
                || (def_val_type->def_val.tuple->count > 0
                    && !def_val_type->def_val.tuple->element_types)
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentTupleInstance))
                || !component_layout_add(&total,
                                         def_val_type->def_val.tuple->count,
                                         sizeof(WASMComponentTypeInstance *))) {
                return false;
            }
            break;
        // Option type
        case WASM_COMP_DEF_VAL_OPTION: // 0x6b t:<valtype>
            if (!def_val_type->def_val.option
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentOptionInstance))) {
                return false;
            }
            break;
        // Result type
        case WASM_COMP_DEF_VAL_RESULT: // 0x6a t?:<valtype>? u?:<valtype>?
            if (!def_val_type->def_val.result
                || !component_layout_add(&total, 1,
                                         sizeof(WASMComponentResultInstance))) {
                return false;
            }
            break;
        // Handle types
        case WASM_COMP_DEF_VAL_OWN:    // 0x69 i:<typeidx>
        case WASM_COMP_DEF_VAL_BORROW: // 0x68 i:<typeidx>
            if ((def_val_type->tag == WASM_COMP_DEF_VAL_OWN
                 && !def_val_type->def_val.owned)
                || (def_val_type->tag == WASM_COMP_DEF_VAL_BORROW
                    && !def_val_type->def_val.borrow)
                || !component_layout_add(
                    &total, 1, sizeof(WASMComponentResourceHandleInstance))) {
                return false;
            }
            break;
        // Async types
        case WASM_COMP_DEF_VAL_STREAM: // 0x66 t?:<valtype>?
        case WASM_COMP_DEF_VAL_FUTURE: // 0x65 t?:<valtype>?
            LOG_WARNING("Not yet supported\n");
            break;
        default:
            return false;
    }
    *size = total;
    return true;
}

/// @brief Calculate size needed for the function types defined in this
/// component
/// @param func_type
/// @return
bool
wasm_get_func_type_size(WASMComponentFuncType *func_type, uint64 *size)
{
    uint64 total = 0;

    if (!func_type || !func_type->params || !func_type->results || !size
        || (func_type->params->count > 0 && !func_type->params->params)
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentFuncTypeInstance))
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentParamListInstance))
        || !component_layout_add(&total, func_type->params->count,
                                 sizeof(WASMComponentLabelValTypeInstance))
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentResultListInstance))
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentTypeInstance))) {
        return false;
    }
    *size = total;
    return true;
}

/// @brief Calculates total size required by component instance type (including
/// index spaces and defined types)
/// @param instance_type instance type declaration from binary section
/// @param instance_type_size OUT: structure that will hold the index counts +
/// total defined types memory size
/// @return
bool
wasm_get_inst_decl_size(WASMComponentInstType *instance_type,
                        WASMComponentInstanceDeclTypeSize *instance_type_size,
                        uint64 *size)
{
    uint64 total = 0, types_size = 0, funcs_count = 0, types_count = 0,
           exports_count = 0, resource_count = 0, method_count = 0;
    uint32 idx = 0;

    if (!instance_type || !size
        || (instance_type->count > 0 && !instance_type->instance_decls)) {
        return false;
    }

    for (idx = 0; idx < instance_type->count; idx++) {
        WASMComponentInstDecl *instance_decl =
            &instance_type->instance_decls[idx];
        switch (instance_decl->tag) {
            case WASM_COMP_COMPONENT_DECL_INSTANCE_CORE_TYPE:
                // TODO: not present in current test binaries
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_TYPE:
                // The new types defined in this instance type
                if (!instance_decl->decl.type) {
                    return false;
                }
                switch (instance_decl->decl.type->tag) {
                    case WASM_COMP_DEF_TYPE:
                    {
                        uint64 nested_size = 0;
                        if (!wasm_get_def_val_type_size(
                                instance_decl->decl.type->type.def_val_type,
                                &nested_size)
                            || !component_layout_add(&types_size, 1,
                                                     nested_size)) {
                            return false;
                        }
                        break;
                    }
                    case WASM_COMP_FUNC_TYPE:
                    {
                        uint64 nested_size = 0;
                        if (!wasm_get_func_type_size(
                                instance_decl->decl.type->type.func_type,
                                &nested_size)
                            || !component_layout_add(&types_size, 1,
                                                     nested_size)) {
                            return false;
                        }
                        break;
                    }
                    default:
                        LOG_WARNING("Instance declaration type only supports "
                                    "types and functions for now \n");
                        break;
                }
                types_count++;
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_ALIAS:
                // Increase index count for the aliased sort
                if (!instance_decl->decl.alias
                    || !instance_decl->decl.alias->sort) {
                    return false;
                }
                switch (instance_decl->decl.alias->sort->sort) {
                    case WASM_COMP_SORT_FUNC:
                        funcs_count++;
                        break;
                    case WASM_COMP_SORT_TYPE:
                        types_count++;
                        break;
                    default:
                        LOG_WARNING("Instance declaration Aliases should only "
                                    "support types and functions for now\n");
                        break;
                }
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL:
                // Increase index count + export count for export definition
                exports_count++;
                if (!instance_decl->decl.export_decl
                    || !instance_decl->decl.export_decl->extern_desc) {
                    return false;
                }
                switch (instance_decl->decl.export_decl->extern_desc->type) {
                    case WASM_COMP_EXTERN_FUNC:
                        funcs_count++;
                        break;
                    case WASM_COMP_EXTERN_TYPE:
                        types_count++;
                        if (!instance_decl->decl.export_decl->extern_desc
                                 ->extern_desc.type.type_bound) {
                            return false;
                        }
                        if (instance_decl->decl.export_decl->extern_desc
                                ->extern_desc.type.type_bound->tag
                            == WASM_COMP_TYPEBOUND_TYPE) {
                            if (!component_layout_add(
                                    &types_size, 1,
                                    sizeof(WASMComponentTypeInstance))
                                || !component_layout_add(
                                    &types_size, 1,
                                    sizeof(WASMComponentResourceInstance))
                                || !component_layout_add(
                                    &types_size, 3,
                                    sizeof(WASMFunctionInstance))) {
                                return false;
                            }
                            resource_count++;
                        }
                        break;
                    default:
                        LOG_WARNING("Instance declaration Exports should only "
                                    "support types and functions for now\n");
                        break;
                }
                break;
            default:
                LOG_WARNING("Unsupported Component instance declaration type");
                break;
        }
    }

    if (types_count > UINT32_MAX || funcs_count > UINT32_MAX
        || exports_count > UINT32_MAX || resource_count > UINT32_MAX) {
        return false;
    }

    method_count = funcs_count;
    if (!component_layout_add(&method_count, resource_count, 3)
        || !component_layout_add(&total, 1, sizeof(WASMComponentTypeInstance))
        || !component_layout_add(&total, 1,
                                 sizeof(WASMComponentInstTypeInstance))
        || !component_layout_add(&total, 1, types_size)
        || !component_layout_add(&total, types_count,
                                 sizeof(WASMComponentTypeInstance))
        || !component_layout_add(&total, funcs_count,
                                 sizeof(WASMComponentFunctionInstance))
        || !component_layout_add(&total, exports_count,
                                 sizeof(WASMComponentExportInstance))
        || !component_layout_add(&total, method_count,
                                 sizeof(WASMFunctionInstance))
        || !component_layout_add(&total, method_count,
                                 sizeof(WASMFunctionImport))) {
        return false;
    }

    if (instance_type_size) {
        // Needed for instantiating the WASMComponentInstTypeInstance structure
        instance_type_size->exports_count = (uint32)exports_count;
        instance_type_size->func_count = (uint32)funcs_count;
        instance_type_size->types_count = (uint32)types_count;
        instance_type_size->types_size = types_size;
        instance_type_size->resource_count = (uint32)resource_count;
    }
    *size = total;
    return true;
}

/// @brief Calculate total size that needs to be allocated for the instance ==
/// size of index spaces + total size necessary for defined types
/// @param component
/// @return index count structure
WASMComponentIndexCount *
wasm_component_get_index_count(WASMComponent *component, char *error_buf,
                               uint32 error_buf_size)
{
    if (!component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: component ptr is NULL\n");
        return NULL;
    }
    // From the component binary section extract the count of each index space
    // of the component, for memory allocation and indexing
    WASMComponentIndexCount *index_count =
        (WASMComponentIndexCount *)wasm_runtime_malloc(
            sizeof(WASMComponentIndexCount));
    if (!index_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: alloc index_count failed\n");
        return NULL;
    }
    memset(index_count, 0, sizeof(WASMComponentIndexCount));
    uint32 section_idx = 0, idx = 0;
    WASMComponentSection *section = NULL;

    LOG_DEBUG(
        "Calculate size of component instance index spaces to be allocated");
    for (section_idx = 0; section_idx < component->section_count;
         section_idx++) {
        if (!component->sections) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: component section NULL value\n");
            return NULL;
        }
        section = &component->sections[section_idx];
        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
                // TODO: Core custom section not in scope for now
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                index_count->core_modules++;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                index_count->core_instances +=
                    (uint32)section->parsed.core_instance_section->count;
                index_count->defined_core_instances +=
                    (uint32)section->parsed.core_instance_section->count;
                break;
            case WASM_COMP_SECTION_CORE_TYPE:
                index_count->core_types +=
                    (uint32)section->parsed.core_type_section->count;
                break;
            case WASM_COMP_SECTION_COMPONENT:
                index_count->components++;
                break;
            case WASM_COMP_SECTION_INSTANCES:
                index_count->instances +=
                    (uint32)section->parsed.instance_section->count;
                index_count->defined_instances +=
                    (uint32)section->parsed.instance_section->count;
                break;
            case WASM_COMP_SECTION_ALIASES:
            {
                uint32 aliases_count =
                    (uint32)section->parsed.alias_section->count;
                for (idx = 0; idx < aliases_count; idx++) {
                    WASMComponentAliasDefinition *alias =
                        &section->parsed.alias_section->aliases[idx];
                    if (!alias || !alias->sort) {
                        set_error_buf_ex(error_buf, error_buf_size,
                                         "ERROR: Invalid alias section");
                        return NULL;
                    }
                    switch (alias->sort->sort) {
                        case WASM_COMP_SORT_CORE_SORT:
                            switch (alias->sort->core_sort) {
                                case WASM_COMP_CORE_SORT_FUNC:
                                    index_count->core_functions++;
                                    index_count->defined_core_functions++;
                                    break;
                                case WASM_COMP_CORE_SORT_TABLE:
                                    index_count->core_tables++;
                                    break;
                                case WASM_COMP_CORE_SORT_MEMORY:
                                    index_count->core_memories++;
                                    break;
                                case WASM_COMP_CORE_SORT_GLOBAL:
                                    index_count->core_globals++;
                                    break;
                                case WASM_COMP_CORE_SORT_TYPE:
                                    index_count->core_types++;
                                    break;
                                case WASM_COMP_CORE_SORT_MODULE:
                                    index_count->core_modules++;
                                    break;
                                case WASM_COMP_CORE_SORT_INSTANCE:
                                    index_count->core_instances++;
                                default:
                                    break;
                            }
                            break;
                        case WASM_COMP_SORT_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_SORT_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_SORT_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_SORT_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_SORT_INSTANCE:
                            index_count->instances++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_TYPE:
                index_count->types +=
                    (uint32)section->parsed.type_section->count;
                for (idx = 0; idx < section->parsed.type_section->count;
                     idx++) {
                    WASMComponentTypes *type =
                        &section->parsed.type_section->types[idx];
                    uint64 type_size = 0;
                    bool has_layout = true;
                    if (!type) {
                        set_error_buf_ex(error_buf, error_buf_size,
                                         "ERROR: Invalid type section");
                        break;
                    }
                    switch (type->tag) {
                        case WASM_COMP_DEF_TYPE:
                            if (!wasm_get_def_val_type_size(
                                    type->type.def_val_type, &type_size)) {
                                has_layout = false;
                            }
                            break;
                        case WASM_COMP_FUNC_TYPE:
                            if (!wasm_get_func_type_size(type->type.func_type,
                                                         &type_size)) {
                                has_layout = false;
                            }
                            break;
                        case WASM_COMP_INSTANCE_TYPE:
                            if (!wasm_get_inst_decl_size(
                                    type->type.instance_type, NULL,
                                    &type_size)) {
                                has_layout = false;
                            }
                            break;
                        case WASM_COMP_RESOURCE_TYPE_SYNC:
                            if (!component_layout_add(
                                    &type_size, 1,
                                    sizeof(WASMComponentTypeInstance))
                                || !component_layout_add(
                                    &type_size, 1,
                                    sizeof(WASMComponentResourceInstance))
                                || !component_layout_add(
                                    &type_size, 3,
                                    sizeof(WASMFunctionInstance))) {
                                has_layout = false;
                            }
                            break;
                        default:
                            LOG_WARNING("Other types not supported for now\n");
                            continue;
                    }
                    if (!has_layout
                        || !component_layout_add(&index_count->types_total_size,
                                                 1, type_size)) {
                        set_error_buf_ex(
                            error_buf, error_buf_size,
                            "ERROR: Component type layout is too large or "
                            "invalid\n");
                        wasm_runtime_free(index_count);
                        return NULL;
                    }
                }
                break;
            case WASM_COMP_SECTION_IMPORTS:
                index_count->imports +=
                    (uint32)section->parsed.import_section->count;
                for (idx = 0; idx < section->parsed.import_section->count;
                     idx++) {
                    WASMComponentImport *import_section =
                        &section->parsed.import_section->imports[idx];
                    if (!import_section || !import_section->extern_desc)
                        continue;
                    switch (import_section->extern_desc->type) {
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            index_count->core_modules++;
                            break;
                        case WASM_COMP_EXTERN_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_EXTERN_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            index_count->instances++;
                            index_count->defined_instances++;
                            break;
                        default:
                            set_error_buf_ex(
                                error_buf, error_buf_size,
                                "ERROR: Unrecognised import type\n");
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_EXPORTS:
                index_count->exports +=
                    (uint32)section->parsed.export_section->count;
                for (idx = 0; idx < section->parsed.export_section->count;
                     idx++) {
                    WASMComponentExport *export_section =
                        &section->parsed.export_section->exports[idx];
                    if (!export_section)
                        continue;
                    switch (export_section->sort_idx->sort->sort) {
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            if (export_section->sort_idx->sort->core_sort
                                == WASM_COMP_CORE_SORT_MODULE)
                                index_count->core_modules++;
                            break;
                        case WASM_COMP_EXTERN_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_EXTERN_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            index_count->instances++;
                            break;
                        default:
                            set_error_buf_ex(
                                error_buf, error_buf_size,
                                "ERROR: Unrecognised export type\n");
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_VALUES:
                // TODO: not in scope for now
                break;
            case WASM_COMP_SECTION_CANONS:
            {
                WASMComponentCanon *canon = NULL;
                for (idx = 0; idx < section->parsed.canon_section->count;
                     idx++) {
                    canon = &section->parsed.canon_section->canons[idx];

                    switch (canon->tag) {
                        case WASM_COMP_CANON_LIFT:
                            index_count->functions++;
                            index_count->defined_functions++;
                            if (canon->canon_data.lift.canon_opts
                                    ->canon_opts_count) {
                                index_count->canon_options_funcs++;
                                index_count->canon_options +=
                                    (uint32)canon->canon_data.lift.canon_opts
                                        ->canon_opts_count;
                            }
                            break;
                        case WASM_COMP_CANON_LOWER:
                            index_count->core_functions++;
                            index_count->defined_core_functions++;
                            if (canon->canon_data.lower.canon_opts
                                    ->canon_opts_count) {
                                index_count->canon_options_funcs++;
                                index_count->canon_options +=
                                    (uint32)canon->canon_data.lift.canon_opts
                                        ->canon_opts_count;
                            }
                            break;
                        case WASM_COMP_CANON_RESOURCE_DROP:
                        case WASM_COMP_CANON_RESOURCE_NEW:
                        case WASM_COMP_CANON_RESOURCE_REP:
                            index_count->core_functions++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            default:
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "FATAL ERROR: Unknown/unsupported section (id=%u)\n",
                    section->id);
                break;
        }
    }
    return index_count;
}

static bool
component_instance_add_storage(uint64 *total_size, uint64 count,
                               uint64 element_size, const char *description,
                               char *error_buf, uint32 error_buf_size)
{
    if (!total_size || element_size == 0 || *total_size > UINT32_MAX
        || count > ((uint64)UINT32_MAX - *total_size) / element_size) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Component instance allocation is too large at %s",
                         description);
        return false;
    }

    *total_size += count * element_size;
    return true;
}

WASMComponentInstance *
wasm_component_instance_allocate(WASMComponentIndexCount *index_count,
                                 char *error_buf, uint32 error_buf_size)
{
    uint64 total_size_64 = 0;
    uint32 total_size;
    WASMComponentInstance *comp_instance;

    if (!index_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Component index counts are NULL");
        return NULL;
    }

    LOG_DEBUG("Allocate memory for component instance with:");
    LOG_DEBUG(" %d components", index_count->components);
    LOG_DEBUG(" %d instances", index_count->instances);
    LOG_DEBUG(" %d core modules", index_count->core_modules);
    LOG_DEBUG(" %d core instances", index_count->core_instances);
    LOG_DEBUG(" %d core functions", index_count->core_functions);
    LOG_DEBUG(" %d core memories", index_count->core_memories);
    LOG_DEBUG(" %d core tables", index_count->core_tables);
    LOG_DEBUG(" %d core globals", index_count->core_globals);
    LOG_DEBUG(" %d core types", index_count->core_types);
    LOG_DEBUG(" %d functions", index_count->functions);
    LOG_DEBUG(" %d types", index_count->types);
    LOG_DEBUG(" %d values", index_count->values);
    LOG_DEBUG(" %d exports", index_count->exports);

#define ADD_INSTANCE_STORAGE(count, type, description)                       \
    do {                                                                     \
        if (!component_instance_add_storage(&total_size_64, (uint64)(count), \
                                            sizeof(type), description,       \
                                            error_buf, error_buf_size)) {    \
            return NULL;                                                     \
        }                                                                    \
    } while (0)

    ADD_INSTANCE_STORAGE(1, WASMComponentInstance, "instance header");
    ADD_INSTANCE_STORAGE(index_count->functions,
                         WASMComponentFunctionInstance *, "functions");
    ADD_INSTANCE_STORAGE(index_count->values, WASMComponentValue *, "values");
    ADD_INSTANCE_STORAGE(index_count->types, WASMComponentTypeInstance *,
                         "types");
    ADD_INSTANCE_STORAGE(index_count->instances, WASMComponentInstance *,
                         "component instances");
    ADD_INSTANCE_STORAGE(index_count->components, WASMComponent *,
                         "components");
    ADD_INSTANCE_STORAGE(index_count->core_functions, WASMFunctionInstance *,
                         "core functions");
    ADD_INSTANCE_STORAGE(index_count->core_tables, WASMTableInstance *,
                         "core tables");
    ADD_INSTANCE_STORAGE(index_count->core_memories, WASMMemoryInstance *,
                         "core memories");
    ADD_INSTANCE_STORAGE(index_count->core_globals, WASMGlobalInstance *,
                         "core globals");
    ADD_INSTANCE_STORAGE(index_count->core_types, WASMType *, "core types");
    ADD_INSTANCE_STORAGE(index_count->core_instances, WASMModuleInstance *,
                         "core instances");
    ADD_INSTANCE_STORAGE(index_count->core_modules, WASMModule *,
                         "core modules");
    ADD_INSTANCE_STORAGE(index_count->exports, WASMComponentExportInstance,
                         "exports");
    ADD_INSTANCE_STORAGE(index_count->types_total_size, uint8,
                         "defined type data");
    ADD_INSTANCE_STORAGE(index_count->defined_core_functions,
                         WASMFunctionInstance, "defined core functions");
    ADD_INSTANCE_STORAGE(index_count->defined_functions,
                         WASMComponentFunctionInstance,
                         "defined component functions");
    ADD_INSTANCE_STORAGE(index_count->defined_core_instances,
                         WASMModuleInstance *, "defined core instances");
    ADD_INSTANCE_STORAGE(index_count->defined_instances,
                         WASMComponentInstance *,
                         "defined component instances");
    ADD_INSTANCE_STORAGE(index_count->canon_options_funcs,
                         WASMComponentCanonOptsInstance,
                         "canonical option groups");
    ADD_INSTANCE_STORAGE(index_count->canon_options,
                         WASMComponentCanonOptInstance, "canonical options");

#undef ADD_INSTANCE_STORAGE

    total_size = (uint32)total_size_64;
    comp_instance = wasm_runtime_malloc(total_size);
    if (!comp_instance) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Failed to allocate component instance\n");
        return NULL;
    }
    memset(comp_instance, 0, total_size);
    wasm_runtime_instantiation_args_set_defaults(
        &comp_instance->instantiation_args);
    wasm_runtime_instantiation_args_set_default_stack_size(
        &comp_instance->instantiation_args, COMPONENT_DEFAULT_STACK_SIZE);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        &comp_instance->instantiation_args, COMPONENT_DEFAULT_HEAP_SIZE);
    comp_instance->default_wasm_stack_size = COMPONENT_DEFAULT_STACK_SIZE;
    // The types defined in this component will be stored in the memory region
    // right after the component instance, similar to Wasm Module implementation

    comp_instance->defined_types_size = (uint32)index_count->types_total_size;
    comp_instance->defined_canon_opts_size =
        (uint32)((uint64)index_count->canon_options_funcs
                     * sizeof(WASMComponentCanonOptsInstance)
                 + (uint64)index_count->canon_options
                       * sizeof(WASMComponentCanonOptInstance));
    comp_instance->functions =
        (WASMComponentFunctionInstance **)((uint8_t *)comp_instance
                                           + sizeof(WASMComponentInstance));
    comp_instance->values =
        (WASMComponentValue **)((uint8_t *)comp_instance->functions
                                + (index_count->functions
                                   * sizeof(WASMComponentFunctionInstance *)));
    comp_instance->types =
        (WASMComponentTypeInstance **)((uint8_t *)comp_instance->values
                                       + (index_count->values
                                          * sizeof(WASMComponentValue *)));
    comp_instance->component_instances =
        (WASMComponentInstance **)((uint8_t *)comp_instance->types
                                   + (index_count->types
                                      * sizeof(WASMComponentTypeInstance *)));
    comp_instance->components =
        (WASMComponent **)((uint8_t *)comp_instance->component_instances
                           + (index_count->instances
                              * sizeof(WASMComponentInstance *)));
    comp_instance->core_functions =
        (WASMFunctionInstance **)((uint8_t *)comp_instance->components
                                  + (index_count->components
                                     * sizeof(WASMComponent *)));
    comp_instance->core_tables =
        (WASMTableInstance **)((uint8_t *)comp_instance->core_functions
                               + (index_count->core_functions
                                  * sizeof(WASMFunctionInstance *)));
    comp_instance->core_memories =
        (WASMMemoryInstance **)((uint8_t *)comp_instance->core_tables
                                + (index_count->core_tables
                                   * sizeof(WASMTableInstance *)));
    comp_instance->core_globals =
        (WASMGlobalInstance **)((uint8_t *)comp_instance->core_memories
                                + (index_count->core_memories
                                   * sizeof(WASMMemoryInstance *)));
    comp_instance->core_types =
        (WASMType **)((uint8_t *)comp_instance->core_globals
                      + (index_count->core_globals
                         * sizeof(WASMGlobalInstance *)));
    comp_instance->core_module_instances =
        (WASMModuleInstance **)((uint8_t *)comp_instance->core_types
                                + (index_count->core_types
                                   * sizeof(WASMType *)));
    comp_instance->core_modules =
        (WASMModule **)((uint8_t *)comp_instance->core_module_instances
                        + (index_count->core_instances
                           * sizeof(WASMModuleInstance *)));
    comp_instance->exports =
        (WASMComponentExportInstance *)((uint8_t *)comp_instance->core_modules
                                        + (index_count->core_modules
                                           * sizeof(WASMModule *)));
    comp_instance->defined_types =
        (void *)((uint8_t *)comp_instance->exports
                 + (index_count->exports
                    * sizeof(WASMComponentExportInstance)));
    comp_instance->defined_core_functions =
        (WASMFunctionInstance *)((uint8_t *)comp_instance->defined_types
                                 + comp_instance->defined_types_size);
    comp_instance->defined_functions =
        (WASMComponentFunctionInstance *)((uint8_t *)comp_instance
                                              ->defined_core_functions
                                          + (index_count->defined_core_functions
                                             * sizeof(WASMFunctionInstance)));
    comp_instance->defined_instances =
        (WASMComponentInstance **)((uint8_t *)comp_instance->defined_functions
                                   + (index_count->defined_functions
                                      * sizeof(WASMComponentFunctionInstance)));
    comp_instance->defined_core_instances =
        (WASMModuleInstance **)((uint8_t *)comp_instance->defined_instances
                                + (index_count->defined_instances
                                   * sizeof(WASMComponentInstance *)));
    comp_instance->defined_canon_opts =
        (void *)((uint8_t *)comp_instance->defined_core_instances
                 + (index_count->defined_core_instances
                    * sizeof(WASMModuleInstance *)));

    /*
    Component instance and all dependant memory areas are allocated in a
    contignuous memory area, as described below

    +----------------------------------+
    | WASMComponentInstance            |
    +----------------------------------+ --> comp_instance->functions
    | < functions >                    |
    +----------------------------------+ --> comp_instance->values
    | < values >                       |
    +----------------------------------+ --> comp_instance->types
    | < types >                        |
    +----------------------------------+ --> comp_instance->component_instances
    | < component instances >          |
    +----------------------------------+ --> comp_instance->components
    | < components >                   |
    +----------------------------------+ --> comp_instance->core_functions
    | < core fuctions >                |
    +----------------------------------+ --> comp_instance->core_tables
    | < core tables >                  |
    +----------------------------------+ --> comp_instance->core_memories
    | < core memories >                |
    +----------------------------------+ --> comp_instance->core_globals
    | < core globals >                 |
    +----------------------------------+ --> comp_instance->core_types
    | < core types >                   |
    +----------------------------------+ -->
    comp_instance->core_module_instances | < core module instances >        |
    +----------------------------------+ --> comp_instance->core_modules
    | < core modules >                 |
    +----------------------------------+ --> comp_instance->exports
    | < exports >                      |
    +----------------------------------+ --> comp_instance->defined_types
    | < defined types >                |
    +----------------------------------+

*/

    // Start with initial size of 10, grow by 50% when needed
    comp_instance->table = wasm_component_table_init(10, 50);
    if (!comp_instance->table) {
        wasm_runtime_free(comp_instance);
        set_error_buf_ex(
            error_buf, error_buf_size,
            "ERROR: Failed to initialize component instance table\n");
        return NULL;
    }

    LOG_DEBUG(
        "Component instance memory allocation successfull, total size is %d\n",
        total_size);
    return comp_instance;
}

uint64
get_core_index_count(WASMInstExpr *instance_expression)
{
    uint32 idx = 0;
    uint64 total_size = 0;
    WASMComponentInlineExport *inline_expr = NULL;
    uint32 func_count = 0, table_count = 0, mem_count = 0, global_count = 0;
    for (idx = 0; idx < instance_expression->without_args.inline_expr_len;
         idx++) {
        inline_expr = instance_expression->without_args.inline_expr;
        switch (inline_expr->sort_idx->sort->core_sort) {
            case WASM_COMP_CORE_SORT_FUNC:
                func_count++;
                break;
            case WASM_COMP_CORE_SORT_TABLE:
                table_count++;
                break;
            case WASM_COMP_CORE_SORT_MEMORY:
                mem_count++;
                break;
            case WASM_COMP_CORE_SORT_GLOBAL:
                global_count++;
                break;
            default:
                break;
        }
    }
    total_size =
        offsetof(WASMModuleInstance, global_table_data.bytes)
        + (uint64)sizeof(table_elem_type_t) * table_count
        + sizeof(WASMMemoryInstance) * (uint64)mem_count
        + sizeof(WASMModuleInstanceExtra); /* Consider global data size == 0 */

    return total_size;
}

WASMComponentInstance *
wasm_component_instantiate_internal_ex(
    WASMComponent *component,
    WASMComponentInstArgInstances *instance_expression,
    const struct InstantiationArgs2 *args, char *error_buf,
    uint32 error_buf_size)
{
    LOG_DEBUG("Instantiate internal");
    if (!component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Invalid component\n");
        return NULL;
    }
    WASMComponentIndexCount *index_count =
        wasm_component_get_index_count(component, error_buf, error_buf_size);
    if (!index_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: failed to retrieve component index count\n");
        return NULL;
    }

    WASMComponentInstance *comp_instance = wasm_component_instance_allocate(
        index_count, error_buf, error_buf_size);
    if (!comp_instance) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: component instance allocation failed\n");
        goto done;
    }
    if (instance_expression) {
        comp_instance->parent = instance_expression->parent;
    }
    if (args) {
        comp_instance->instantiation_args = *args;
    }
    comp_instance->default_wasm_stack_size =
        comp_instance->instantiation_args.v1.default_stack_size;
    comp_instance->custom_data = comp_instance->instantiation_args.custom_data;
    comp_instance->component = component;
    uint32 section_idx = 0;
    WASMComponentSection *section = NULL;

#if WASM_ENABLE_LIBC_WASI_P2 != 0
    if (!comp_instance->parent) {
        const WASIArguments *wasi_args = &component->wasi_args;
        if (component->wasi_args.set_by_user
            && comp_instance->instantiation_args.wasi.set_by_user) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: WASI configuration was given via both "
                             "the component and InstantiationArgs2\n");
            wasm_component_deinstantiate(comp_instance);
            comp_instance = NULL;
            goto done;
        }
        if (comp_instance->instantiation_args.wasi.set_by_user
            || !component->wasi_args.set_by_user) {
            wasi_args = &comp_instance->instantiation_args.wasi;
        }
        if (!wasm_component_runtime_init_wasi(
                comp_instance, wasi_args->dir_list, wasi_args->dir_count,
                wasi_args->map_dir_list, wasi_args->map_dir_count,
                wasi_args->env, wasi_args->env_count, wasi_args->addr_pool,
                wasi_args->addr_count, wasi_args->ns_lookup_pool,
                wasi_args->ns_lookup_count, wasi_args->argv, wasi_args->argc,
                wasi_args->stdio[0], wasi_args->stdio[1], wasi_args->stdio[2],
                wasi_args->wasi_options, error_buf, error_buf_size)) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: Failed to initiate component wasi args\n");
            wasm_component_deinstantiate(comp_instance);
            comp_instance = NULL;
            goto done;
        }
    }
    else {
        comp_instance->wasi_ctx = comp_instance->parent->wasi_ctx;
    }
#endif

    for (section_idx = 0; section_idx < component->section_count;
         section_idx++) {
        section = &component->sections[section_idx];
        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
                // TODO: not in scope for now
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                LOG_DEBUG("%d : --Core module section", section_idx);
                comp_instance->core_modules[comp_instance->core_modules_count] =
                    (WASMModule *)section->parsed.core_module->module_handle;
                comp_instance->core_modules_count++;
                if (comp_instance->core_modules_count
                    > index_count->core_modules) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Core modules count exceeded in "
                                     "section %d, expected only %d modules",
                                     section_idx, index_count->core_modules);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                LOG_DEBUG("Added core module %d",
                          comp_instance->core_modules_count - 1);
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                LOG_DEBUG("%d : --Core instance section\n", section_idx);
                if (!wasm_resolve_core_instance(
                        section->parsed.core_instance_section, comp_instance,
                        error_buf, error_buf_size)) {
                    if (!error_buf || error_buf[0] == '\0') {
                        set_error_buf_ex(error_buf, error_buf_size,
                                         "ERROR: Core instance section %d "
                                         "instantiation failed\n",
                                         section->id);
                    }
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_CORE_TYPE:
                // TODO: need more info on this section (not found in any of the
                // test binaries)
                break;
            case WASM_COMP_SECTION_COMPONENT:
                LOG_DEBUG("%d : --Component section", section_idx);
                if (!section->parsed.component) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "WARNING: parsed component empty!\n");
                }
                comp_instance->components[comp_instance->components_count] =
                    section->parsed.component;
                comp_instance->components_count++;
                if (comp_instance->components_count > index_count->components) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Components count exceeded in "
                                     "section %d, expected only %d components",
                                     section_idx, index_count->components);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                LOG_DEBUG("Added component %d",
                          comp_instance->components_count - 1);
                break;
            case WASM_COMP_SECTION_INSTANCES:
                LOG_DEBUG("%d : --Instance section\n", section_idx);
                if (!wasm_resolve_instance(section->parsed.instance_section,
                                           comp_instance, error_buf,
                                           error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Instance section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_ALIASES:
                LOG_DEBUG("%d : --Alias section", section_idx);
                if (!wasm_resolve_alias(section->parsed.alias_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Alias section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_TYPE:
                LOG_DEBUG("%d : --Type section %d", section_idx,
                          comp_instance->types_count);
                if (!wasm_resolve_types(section->parsed.type_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: type section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_CANONS:
                LOG_DEBUG("%d : --Canon section", section_idx);
                if (!wasm_resolve_canon(section->parsed.canon_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Canonical section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_START:
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Component start sections are not "
                                 "supported\n");
                wasm_component_deinstantiate(comp_instance);
                comp_instance = NULL;
                goto done;
            case WASM_COMP_SECTION_IMPORTS:
                LOG_DEBUG("%d : --Import section", section_idx);
                bool import_failed = false;
                if (comp_instance->parent && instance_expression)
                    import_failed = !wasm_resolve_imports(
                        section->parsed.import_section, comp_instance,
                        instance_expression, error_buf, error_buf_size);
                else if (!comp_instance->parent)
                    import_failed = !wasm_resolve_imports_host(
                        section->parsed.import_section, comp_instance,
                        error_buf, error_buf_size);
                if (import_failed) {
                    if (!error_buf || error_buf[0] == '\0') {
                        set_error_buf_ex(
                            error_buf, error_buf_size,
                            "ERROR: Import section %d instantiation failed\n",
                            section->id);
                    }
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_EXPORTS:
                LOG_DEBUG("%d : --Export section", section_idx);
                if (!wasm_resolve_exports(section->parsed.export_section,
                                          comp_instance, error_buf,
                                          error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: export section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_VALUES:
                set_error_buf_ex(error_buf, error_buf_size,
                                 "ERROR: Component value sections are not "
                                 "supported\n");
                wasm_component_deinstantiate(comp_instance);
                comp_instance = NULL;
                goto done;
            default:
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "FATAL ERROR: Unknown/unsupported section (id=%u)\n",
                    section->id);
                break;
        }
    }
    comp_instance->may_leave = true;
    LOG_DEBUG("Instantiation done\n");
done:
    wasm_runtime_free(index_count);
    return comp_instance;
}

WASMComponentInstance *
wasm_component_instantiate_internal(
    WASMComponent *component,
    WASMComponentInstArgInstances *instance_expression, char *error_buf,
    uint32 error_buf_size)
{
    const struct InstantiationArgs2 *args =
        instance_expression && instance_expression->parent
            ? &instance_expression->parent->instantiation_args
            : NULL;
    return wasm_component_instantiate_internal_ex(
        component, instance_expression, args, error_buf, error_buf_size);
}

WASMComponentInstance *
wasm_component_instantiate(WASMComponent *component, char *error_buf,
                           uint32 error_buf_size)
{

    return wasm_component_instantiate_internal(component, NULL, error_buf,
                                               error_buf_size);
}

WASMComponentInstance *
wasm_component_instantiate_ex2(WASMComponent *component,
                               const struct InstantiationArgs2 *args,
                               char *error_buf, uint32 error_buf_size)
{
    if (!args) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: InstantiationArgs2 is NULL\n");
        return NULL;
    }
    return wasm_component_instantiate_internal_ex(component, NULL, args,
                                                  error_buf, error_buf_size);
}

static void
component_set_custom_data(WASMComponentInstance *comp_instance,
                          void *custom_data, uint32 depth)
{
    if (!comp_instance || depth > MAX_DEPTH_RECURSION) {
        return;
    }

    comp_instance->custom_data = custom_data;
    comp_instance->instantiation_args.custom_data = custom_data;

    for (uint32 idx = 0; idx < comp_instance->types_count; idx++) {
        WASMComponentTypeInstance *type = comp_instance->types[idx];
        WASMComponentResourceInstance *resource = NULL;

        if (type
            && (type->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                || type->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)
            && (resource = type->type_specific.resource)
            && resource->host_drop_attachment_is_custom_data) {
            resource->host_drop_attachment = custom_data;
        }
    }

    for (uint32 idx = 0; idx < comp_instance->defined_core_instances_count;
         idx++) {
        WASMModuleInstance *core_inst =
            comp_instance->defined_core_instances[idx];

        if (core_inst && core_inst->e) {
            wasm_runtime_set_custom_data_internal(
                (WASMModuleInstanceCommon *)core_inst, custom_data);
        }
    }

    for (uint32 idx = 0; idx < comp_instance->defined_instances_count; idx++) {
        component_set_custom_data(comp_instance->defined_instances[idx],
                                  custom_data, depth + 1);
    }
}

void
wasm_component_set_custom_data(WASMComponentInstance *comp_instance,
                               void *custom_data)
{
    component_set_custom_data(comp_instance, custom_data, 0);
}

void *
wasm_component_get_custom_data(WASMComponentInstance *comp_instance)
{
    return comp_instance ? comp_instance->custom_data : NULL;
}

void *
wasm_component_get_custom_data_from_exec_env(wasm_exec_env_t exec_env)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;
    WASMComponentInstance *comp_instance =
        internal_exec_env ? internal_exec_env->component_inst : NULL;

    while (comp_instance && comp_instance->parent) {
        comp_instance = comp_instance->parent;
    }
    return wasm_component_get_custom_data(comp_instance);
}

bool
wasm_component_exec_env_is_callback(wasm_exec_env_t exec_env)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;

    return internal_exec_env && internal_exec_env->component_callback_active;
}

typedef struct HostResourceMatch {
    const char *interface_name;
    const char *resource_name;
    WASMComponentResourceInstance *resource;
    bool ambiguous;
} HostResourceMatch;

static void
match_host_resource(HostResourceMatch *match,
                    WASMComponentResourceInstance *resource)
{
    if (!match || !resource || !resource->is_host || !resource->interface_name
        || !resource->name
        || strcmp(resource->interface_name, match->interface_name) != 0
        || strcmp(resource->name, match->resource_name) != 0) {
        return;
    }

    if (match->resource && match->resource != resource) {
        match->ambiguous = true;
    }
    else {
        match->resource = resource;
    }
}

static void
find_host_resource_in_type(HostResourceMatch *match,
                           WASMComponentTypeInstance *type, uint32 depth)
{
    if (!match || !type || match->ambiguous || depth > MAX_DEPTH_RECURSION) {
        return;
    }

    switch (type->type) {
        case COMPONENT_VAL_TYPE_RESOURCE_SYNC:
        case COMPONENT_VAL_TYPE_RESOURCE_ASYNC:
            match_host_resource(match, type->type_specific.resource);
            break;
        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
            if (type->type_specific.resource_handle) {
                match_host_resource(
                    match, type->type_specific.resource_handle->resource);
            }
            break;
        case COMPONENT_VAL_TYPE_RECORD:
            if (type->type_specific.record) {
                for (uint32 idx = 0; idx < type->type_specific.record->count;
                     idx++) {
                    find_host_resource_in_type(
                        match, type->type_specific.record->fields[idx].type,
                        depth + 1);
                }
            }
            break;
        case COMPONENT_VAL_TYPE_VARIANT:
            if (type->type_specific.variant) {
                for (uint32 idx = 0; idx < type->type_specific.variant->count;
                     idx++) {
                    find_host_resource_in_type(
                        match,
                        type->type_specific.variant->cases[idx].value_type,
                        depth + 1);
                }
            }
            break;
        case COMPONENT_VAL_TYPE_LIST:
            if (type->type_specific.list) {
                find_host_resource_in_type(
                    match, type->type_specific.list->element_type, depth + 1);
            }
            break;
        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
            if (type->type_specific.list_len) {
                find_host_resource_in_type(
                    match, type->type_specific.list_len->element_type,
                    depth + 1);
            }
            break;
        case COMPONENT_VAL_TYPE_TUPLE:
            if (type->type_specific.tuple) {
                for (uint32 idx = 0; idx < type->type_specific.tuple->count;
                     idx++) {
                    find_host_resource_in_type(
                        match, type->type_specific.tuple->element_types[idx],
                        depth + 1);
                }
            }
            break;
        case COMPONENT_VAL_TYPE_OPTION:
            if (type->type_specific.option) {
                find_host_resource_in_type(
                    match, type->type_specific.option->element_type, depth + 1);
            }
            break;
        case COMPONENT_VAL_TYPE_RESULT:
            if (type->type_specific.result) {
                find_host_resource_in_type(
                    match, type->type_specific.result->result_type, depth + 1);
                find_host_resource_in_type(
                    match, type->type_specific.result->error_type, depth + 1);
            }
            break;
        default:
            break;
    }
}

static WASMComponentResourceInstance *
find_host_resource_in_func(WASMComponentFuncTypeInstance *func_type,
                           const char *interface_name,
                           const char *resource_name, bool search_results)
{
    HostResourceMatch match = {
        .interface_name = interface_name,
        .resource_name = resource_name,
    };

    if (!func_type || !interface_name || !resource_name) {
        return NULL;
    }

    if (search_results) {
        if (func_type->results) {
            find_host_resource_in_type(&match, func_type->results->result, 0);
        }
    }
    else if (func_type->params) {
        for (uint32 idx = 0; idx < func_type->params->count; idx++) {
            find_host_resource_in_type(&match,
                                       func_type->params->params[idx].type, 0);
        }
    }

    return match.ambiguous ? NULL : match.resource;
}

bool
wasm_component_host_resource_new(wasm_exec_env_t exec_env,
                                 const char *interface_name,
                                 const char *resource_name,
                                 uint32_t representation, uint32_t *out_handle)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;
    WASMComponentFuncTypeInstance *func_type = NULL;
    WASMComponentInstance *component_inst = NULL;
    WASMComponentResourceInstance *resource = NULL;

    if (!out_handle) {
        return false;
    }
    *out_handle = 0;
    if (!internal_exec_env || !interface_name || !resource_name
        || representation == 0
        || !(component_inst = internal_exec_env->component_inst)
        || !(func_type = wasm_get_component_func_type(internal_exec_env))) {
        return false;
    }

    resource = find_host_resource_in_func(func_type, interface_name,
                                          resource_name, true);
    if (!resource
        || !canon_resource_new(resource, component_inst, representation,
                               out_handle)) {
        return false;
    }

    return true;
}

bool
wasm_component_host_resource_rep(wasm_exec_env_t exec_env,
                                 const char *interface_name,
                                 const char *resource_name, uint32_t handle,
                                 uint32_t *out_representation)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;
    WASMComponentFuncTypeInstance *func_type = NULL;
    WASMComponentInstance *component_inst = NULL;
    WASMComponentResourceInstance *resource = NULL;

    if (!out_representation) {
        return false;
    }
    *out_representation = 0;
    if (!internal_exec_env || !interface_name || !resource_name
        || !(component_inst = internal_exec_env->component_inst)
        || !(func_type = wasm_get_component_func_type(internal_exec_env))) {
        return false;
    }

    resource = find_host_resource_in_func(func_type, interface_name,
                                          resource_name, false);
    return resource
           && canon_resource_rep(resource, component_inst, handle,
                                 out_representation);
}

bool
wasm_component_host_resource_take(wasm_exec_env_t exec_env,
                                  const char *interface_name,
                                  const char *resource_name, uint32_t handle,
                                  uint32_t *out_representation)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;
    WASMComponentFuncTypeInstance *func_type = NULL;
    WASMComponentInstance *component_inst = NULL;
    WASMComponentResourceInstance *resource = NULL;
    WASMResourceHandle *resource_handle = NULL;

    if (!out_representation) {
        return false;
    }
    *out_representation = 0;
    if (!internal_exec_env || !interface_name || !resource_name
        || !(component_inst = internal_exec_env->component_inst)
        || !(func_type = wasm_get_component_func_type(internal_exec_env))) {
        return false;
    }

    resource = find_host_resource_in_func(func_type, interface_name,
                                          resource_name, false);
    resource_handle =
        resource ? wasm_component_table_get(component_inst->table, handle,
                                            WASM_TABLE_ELEM_RESOURCE_HANDLE)
                 : NULL;
    if (!resource_handle || resource_handle->rt != resource
        || !resource_handle->own || resource_handle->num_lends != 0) {
        return false;
    }

    *out_representation = resource_handle->rep;
    if (!wasm_component_table_remove(component_inst->table, handle)) {
        *out_representation = 0;
        return false;
    }
    return true;
}

static uint32
set_host_resource_drop_callback(
    WASMComponentInstance *comp_instance, const char *interface_name,
    const char *resource_name,
    wasm_component_host_resource_drop_callback_t callback, void *attachment,
    uint32 depth)
{
    uint32 match_count = 0;

    if (!comp_instance || depth > MAX_DEPTH_RECURSION) {
        return 0;
    }

    for (uint32 idx = 0; idx < comp_instance->types_count; idx++) {
        WASMComponentTypeInstance *type = comp_instance->types[idx];
        WASMComponentResourceInstance *resource = NULL;

        if (!type
            || (type->type != COMPONENT_VAL_TYPE_RESOURCE_SYNC
                && type->type != COMPONENT_VAL_TYPE_RESOURCE_ASYNC)
            || !(resource = type->type_specific.resource) || !resource->is_host
            || !resource->interface_name || !resource->name
            || strcmp(resource->interface_name, interface_name) != 0
            || strcmp(resource->name, resource_name) != 0) {
            continue;
        }

        resource->host_drop_callback = callback;
        resource->host_drop_attachment = attachment;
        resource->host_drop_attachment_is_custom_data = false;
        match_count++;
    }

    for (uint32 idx = 0; idx < comp_instance->defined_instances_count; idx++) {
        match_count += set_host_resource_drop_callback(
            comp_instance->defined_instances[idx], interface_name,
            resource_name, callback, attachment, depth + 1);
    }

    return match_count;
}

bool
wasm_component_set_host_resource_drop_callback(
    WASMComponentInstance *comp_instance, const char *interface_name,
    const char *resource_name,
    wasm_component_host_resource_drop_callback_t callback, void *attachment)
{
    if (!comp_instance || !interface_name || !resource_name || !callback) {
        return false;
    }

    return set_host_resource_drop_callback(comp_instance, interface_name,
                                           resource_name, callback, attachment,
                                           0)
           > 0;
}

void
wasm_component_terminate(WASMComponentInstance *comp_instance)
{
    if (!comp_instance) {
        return;
    }

    for (uint32 idx = 0; idx < comp_instance->defined_core_instances_count;
         idx++) {
        WASMModuleInstance *core_inst =
            comp_instance->defined_core_instances[idx];
        if (core_inst && core_inst->e) {
            wasm_runtime_terminate((wasm_module_inst_t)core_inst);
        }
    }

    for (uint32 idx = 0; idx < comp_instance->defined_instances_count; idx++) {
        wasm_component_terminate(comp_instance->defined_instances[idx]);
    }
}

#if WASM_ENABLE_LIBC_WASI_P2 != 0
void
wasm_component_runtime_destroy_wasi(WASMComponentInstance *comp_instance)
{
    WASIContext *wasi_ctx = comp_instance->wasi_ctx;

    if (wasi_ctx) {
        if (wasi_ctx->argv_environ) {
            argv_environ_destroy(wasi_ctx->argv_environ);
            wasm_runtime_free(wasi_ctx->argv_environ);
        }
        if (wasi_ctx->curfds) {
            fd_table_destroy(wasi_ctx->curfds);
            wasm_runtime_free(wasi_ctx->curfds);
        }
        if (wasi_ctx->prestats) {
            fd_prestats_destroy(wasi_ctx->prestats);
            wasm_runtime_free(wasi_ctx->prestats);
        }
        if (wasi_ctx->addr_pool) {
            addr_pool_destroy(wasi_ctx->addr_pool);
            wasm_runtime_free(wasi_ctx->addr_pool);
        }
        if (wasi_ctx->argv_buf)
            wasm_runtime_free(wasi_ctx->argv_buf);
        if (wasi_ctx->argv_list)
            wasm_runtime_free(wasi_ctx->argv_list);
        if (wasi_ctx->env_buf)
            wasm_runtime_free(wasi_ctx->env_buf);
        if (wasi_ctx->env_list)
            wasm_runtime_free(wasi_ctx->env_list);
        if (wasi_ctx->ns_lookup_buf)
            wasm_runtime_free(wasi_ctx->ns_lookup_buf);
        if (wasi_ctx->ns_lookup_list)
            wasm_runtime_free(wasi_ctx->ns_lookup_list);
        if (wasi_ctx->wasi_options)
            wasm_runtime_free(wasi_ctx->wasi_options);

        wasm_runtime_free(wasi_ctx);
    }
}
#endif

void
wasm_component_deinstantiate(WASMComponentInstance *comp_instance)
{
    if (!comp_instance) {
        return;
    }
    uint32 idx = 0;

    /* Drop owned resources while all destructor implementations are still
     * live. This must precede both nested-component and core deinstantiation.
     */
    if (comp_instance->table) {
        wasm_component_table_destroy(comp_instance->table);
        comp_instance->table = NULL;
    }

    // Free nested component instances
    for (idx = 0; idx < comp_instance->defined_instances_count; idx++) {
        if (comp_instance->defined_instances[idx]) {
            wasm_component_deinstantiate(comp_instance->defined_instances[idx]);
            comp_instance->defined_instances[idx] = NULL;
        }
    }

    // Free core instances after every component resource has been dropped
    for (idx = 0; idx < comp_instance->defined_core_instances_count; idx++) {
        if (!comp_instance->defined_core_instances[idx]->e) {
            wasm_runtime_free(
                comp_instance->defined_core_instances
                    [idx]); // This instance was generated from exports list,
                            // not from a core module
        }
        else
            wasm_deinstantiate(comp_instance->defined_core_instances[idx],
                               false);
        comp_instance->defined_core_instances[idx] = NULL;
    }

    // Free runtime canonical options for component functions (canon.lift)
    for (idx = 0; idx < comp_instance->defined_functions_count; idx++) {
        WASMComponentFunctionInstance *func =
            &comp_instance->defined_functions[idx];
        if (func && func->canon_options) {
            free_canonical_options(func->canon_options);
            func->canon_options = NULL;
        }
    }

    // Free runtime canonical options for core functions (canon.lower)
    for (idx = 0; idx < comp_instance->defined_core_functions_count; idx++) {
        WASMFunctionInstance *func =
            &comp_instance->defined_core_functions[idx];
        if (func && func->canon_options) {
            free_canonical_options(func->canon_options);
            func->canon_options = NULL;
        }
    }

#if WASM_ENABLE_LIBC_WASI_P2 != 0
    if (comp_instance->wasi_ctx && !comp_instance->parent) {
        // destroy component wasi context
        wasm_component_runtime_destroy_wasi(comp_instance);
    }
#endif
    wasm_runtime_free(comp_instance);
    comp_instance = NULL;
}

WASMComponentFunctionInstance *
wasm_component_lookup_function(const WASMComponentInstance *component_inst,
                               const char *name)
{
    if (!component_inst || !component_inst->exports || !name)
        return NULL;

    // Iterate through exports
    uint32 index = 0;
    const char *exported_name = NULL;

    for (index = 0; index < component_inst->exports_count; index++) {
        WASMComponentExportInstance *export_inst =
            &component_inst->exports[index];

        if (export_inst->type == WASM_COMP_EXTERN_FUNC) {
            WASMComponentExportName *export_name = export_inst->export_name;

            if (export_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
                exported_name = export_name->exported.simple.name->name;
            }
            else if (export_name->tag == WASM_COMP_IMPORTNAME_VERSIONED) {
                exported_name = export_name->exported.versioned.name->name;
            }

            if (exported_name && strcmp(exported_name, name) == 0) {
                return export_inst->exp.function;
            }
        }

        if (export_inst->type == WASM_COMP_EXTERN_INSTANCE) {
            uint32 index2 = 0;
            WASMComponentInstance *extern_inst = export_inst->exp.instance;

            for (index2 = 0; index2 < extern_inst->exports_count; index2++) {
                WASMComponentExportInstance *export_inst2 =
                    &extern_inst->exports[index2];

                if (export_inst2->type == WASM_COMP_EXTERN_FUNC) {
                    WASMComponentExportName *export_name =
                        export_inst2->export_name;

                    if (export_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
                        exported_name = export_name->exported.simple.name->name;
                    }
                    else if (export_name->tag
                             == WASM_COMP_IMPORTNAME_VERSIONED) {
                        exported_name =
                            export_name->exported.versioned.name->name;
                    }

                    if (exported_name && strcmp(exported_name, name) == 0) {
                        return export_inst2->exp.function;
                    }
                }
            }
        }
    }

    return NULL; // Function not found
}

static const char *
component_export_name(const WASMComponentExportInstance *export_inst)
{
    WASMComponentExportName *export_name;

    if (!export_inst || !(export_name = export_inst->export_name)) {
        return NULL;
    }

    if (export_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
        return export_name->exported.simple.name
                   ? export_name->exported.simple.name->name
                   : NULL;
    }
    if (export_name->tag == WASM_COMP_IMPORTNAME_VERSIONED) {
        return export_name->exported.versioned.name
                   ? export_name->exported.versioned.name->name
                   : NULL;
    }
    return NULL;
}

WASMComponentFunctionInstance *
wasm_component_lookup_function_qualified(
    const WASMComponentInstance *component_inst, const char *interface_name,
    const char *function_name)
{
    uint32 interface_index, function_index;

    if (!component_inst || !component_inst->exports || !interface_name
        || !function_name) {
        return NULL;
    }

    for (interface_index = 0; interface_index < component_inst->exports_count;
         interface_index++) {
        WASMComponentExportInstance *interface_export =
            &component_inst->exports[interface_index];
        WASMComponentInstance *interface_inst;
        const char *exported_interface_name;

        if (interface_export->type != WASM_COMP_EXTERN_INSTANCE
            || !(interface_inst = interface_export->exp.instance)
            || !(exported_interface_name =
                     component_export_name(interface_export))
            || strcmp(exported_interface_name, interface_name) != 0) {
            continue;
        }

        for (function_index = 0; function_index < interface_inst->exports_count;
             function_index++) {
            WASMComponentExportInstance *function_export =
                &interface_inst->exports[function_index];
            const char *exported_function_name;

            if (function_export->type != WASM_COMP_EXTERN_FUNC
                || !(exported_function_name =
                         component_export_name(function_export))) {
                continue;
            }
            if (strcmp(exported_function_name, function_name) == 0) {
                return function_export->exp.function;
            }
        }

        /* Component export names are unique, so an exact interface match
         * cannot be followed by a second candidate. */
        return NULL;
    }

    return NULL;
}

WASMMemoryInstance *
canon_get_memory(CanonicalOptions *canon_opts)
{
    if (!canon_opts || !canon_opts->lift_lower_opts
        || !canon_opts->lift_lower_opts->lift_opts) {
        return NULL;
    }
    else {
        return canon_opts->lift_lower_opts->lift_opts->memory;
    }
}

static WASMMemoryInstance *
get_active_component_callback_memory(WASMExecEnv *exec_env)
{
    WASMMemoryInstance *memory = NULL;

    if (!exec_env || !exec_env->component_callback_active
        || !exec_env->component_inst || !exec_env->core_func || !exec_env->cx
        || !exec_env->cx->canonical_opts
        || !exec_env->core_func->component_function || !exec_env->memory) {
        return NULL;
    }

    if (exec_env->cx->inst != exec_env->component_inst
        || (exec_env->core_func->canon_options
            && exec_env->cx->canonical_opts
                   != exec_env->core_func->canon_options)) {
        return NULL;
    }

    memory = canon_get_memory(exec_env->cx->canonical_opts);
    return memory == exec_env->memory ? memory : NULL;
}

static bool
get_component_callback_memory_range(WASMExecEnv *exec_env, uint32 app_offset,
                                    uint32 size, uint8 **p_native_addr)
{
    const uint64 wasm32_address_space_size = (uint64)UINT32_MAX + 1;
    WASMMemoryInstance *memory = NULL;
    WASMModuleInstanceCommon *module_inst_comm = NULL;
    uint64 range_end = 0;
    bool is_valid = false;

    if (p_native_addr) {
        *p_native_addr = NULL;
    }
    if (!p_native_addr
        || !(memory = get_active_component_callback_memory(exec_env))) {
        return false;
    }

    module_inst_comm = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst_comm
        || (module_inst_comm->module_type != Wasm_Module_Bytecode
            && module_inst_comm->module_type != Wasm_Module_AoT)) {
        return false;
    }

    range_end = (uint64)app_offset + (uint64)size;
    if (range_end > wasm32_address_space_size) {
        goto fail;
    }

    SHARED_MEMORY_LOCK(memory);
    if (range_end <= memory->memory_data_size
        && (memory->memory_data || range_end == 0)) {
        *p_native_addr =
            memory->memory_data ? memory->memory_data + app_offset : NULL;
        is_valid = true;
    }
    SHARED_MEMORY_UNLOCK(memory);

    if (is_valid) {
        return true;
    }

fail:
    wasm_runtime_set_exception(module_inst_comm, "out of bounds memory access");
    return false;
}

bool
wasm_component_validate_memory_range(wasm_exec_env_t exec_env,
                                     uint32_t app_offset, uint32_t size)
{
    uint8 *native_addr = NULL;

    return get_component_callback_memory_range(exec_env, app_offset, size,
                                               &native_addr);
}

bool
wasm_component_get_memory_range(wasm_exec_env_t exec_env, uint32_t app_offset,
                                uint32_t size, uint8_t **p_native_addr)
{
    uint8 *native_addr = NULL;

    if (p_native_addr) {
        *p_native_addr = NULL;
    }
    if (!p_native_addr
        || !get_component_callback_memory_range(exec_env, app_offset, size,
                                                &native_addr)) {
        return false;
    }

    *p_native_addr = native_addr;
    return true;
}

bool
wasm_component_get_memory_range_const(wasm_exec_env_t exec_env,
                                      uint32_t app_offset, uint32_t size,
                                      const uint8_t **p_native_addr)
{
    uint8 *native_addr = NULL;

    if (p_native_addr) {
        *p_native_addr = NULL;
    }
    if (!p_native_addr
        || !get_component_callback_memory_range(exec_env, app_offset, size,
                                                &native_addr)) {
        return false;
    }

    *p_native_addr = native_addr;
    return true;
}

typedef void (*GenericFunctionPointer)(void);

/// @brief Allocates memory inside the wasm linerar memory (WASMMemoryInstance)
/// @param exec_env current execution environment, that holds the memory
/// instance
/// @param size size of memroy to be allocated
/// @param p_native_addr will return a pointer to the allocated memory area
/// @return numeric offset to the start of the newly allocated memory, from
/// start of wasm linear memory
uint64
wasm_module_malloc_p2(WASMExecEnv *exec_env, uint64 size, void **p_native_addr)
{
    WASMMemoryInstance *memory = exec_env->memory;
    uint8 *addr = NULL;

    WASMModuleInstance *module_inst =
        (WASMModuleInstance *)get_module_inst(exec_env);

    bh_assert(size <= UINT32_MAX);

    if (!memory) {
        wasm_set_exception(module_inst, "uninitialized memory");
        return 0;
    }

    if (memory->heap_handle) {
        addr = mem_allocator_malloc(memory->heap_handle, (uint32)size);
    }
    // TODO: to be seen if else branch from wasm_module_malloc_internal needs to
    // be handled, after canon lift/lower implementation is finished (possible
    // use of cabi-realloc)

    if (!addr) {
        if (memory->heap_handle
            && mem_allocator_is_heap_corrupted(memory->heap_handle)) {
            wasm_runtime_show_app_heap_corrupted_prompt();
            wasm_set_exception(module_inst, "app heap corrupted");
        }
        else {
            LOG_WARNING("warning: allocate %" PRIu64 " bytes memory failed",
                        size);
        }
        return 0;
    }
    if (p_native_addr)
        *p_native_addr = addr;

    return (uint64)(addr - memory->memory_data);
}

/// @brief validate if the offset corresponds to a valid memory address inside
/// wasm linear memory
/// @param exec_env execution environment holding the desired linear memory
/// @param app_offset offset to start of desired memory area inside wasm linear
/// memory
/// @param size size of the desired memory area insode wasm linear memory
/// @return true if memory is valid, false otherwise
bool
wasm_runtime_validate_app_addr_p2(wasm_exec_env_t exec_env, uint64 app_offset,
                                  uint64 size)
{
    WASMModuleInstanceCommon *module_inst_comm =
        wasm_runtime_get_module_inst(exec_env);
    WASMModuleInstance *module_inst = (WASMModuleInstance *)module_inst_comm;
    WASMMemoryInstance *memory_inst = NULL;
    uint64 max_linear_memory_size = MAX_LINEAR_MEMORY_SIZE;

    bh_assert(module_inst_comm->module_type == Wasm_Module_Bytecode
              || module_inst_comm->module_type == Wasm_Module_AoT);

#if WASM_CONFIGURABLE_BOUNDS_CHECKS != 0
    if (!wasm_runtime_is_bounds_checks_enabled(module_inst_comm)) {
        return true;
    }
#endif

    memory_inst = exec_env->memory;
    if (!memory_inst) {
        goto fail;
    }

    // TODO: to be seen if shared heap functionality is applicable in the
    // constext of component model
#if WASM_ENABLE_SHARED_HEAP != 0
    if (is_app_addr_in_shared_heap(module_inst_comm, memory_inst->is_memory64,
                                   app_offset, size)) {
        return true;
    }
#endif

#if WASM_ENABLE_MEMORY64 != 0
    if (memory_inst->is_memory64)
        max_linear_memory_size = MAX_LINEAR_MEM64_MEMORY_SIZE;
#endif
    /* boundary overflow check */
    if (size > max_linear_memory_size
        || app_offset > max_linear_memory_size - size) {
        goto fail;
    }

    SHARED_MEMORY_LOCK(memory_inst);

    if (app_offset + size <= memory_inst->memory_data_size) {
        SHARED_MEMORY_UNLOCK(memory_inst);
        return true;
    }

    SHARED_MEMORY_UNLOCK(memory_inst);

fail:
    wasm_set_exception(module_inst, "out of bounds memory access");
    return false;
}

static bool
call_canonical_realloc(LiftLowerContext *cx, int32_t old_ptr, int32_t old_size,
                       int32_t align, int32_t new_size, uint32_t *out_ptr)
{
    WASMFunctionInstance *realloc_func = get_realloc_func(cx);
    wasm_val_t results[1] = { 0 };

    if (out_ptr) {
        *out_ptr = 0;
    }
    if (!out_ptr) {
        return false;
    }
    if (!realloc_func) {
        set_component_exception(cx, "realloc function not provided");
        return false;
    }

    WASMExecEnv *exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)realloc_func->module_instance);
    if (!exec_env) {
        set_component_exception(cx, "create singleton exec_env failed");
        return false;
    }

    // The singleton exec_env may be changed by a nested import call, thus we
    // need to save and restore it
    WASMModuleInstanceCommon *saved_module_inst =
        wasm_runtime_get_module_inst(exec_env);
    wasm_exec_env_set_module_inst(
        exec_env, (WASMModuleInstanceCommon *)realloc_func->module_instance);

    wasm_val_t args[4] = { { .kind = WASM_I32, .of.i32 = old_ptr },
                           { .kind = WASM_I32, .of.i32 = old_size },
                           { .kind = WASM_I32, .of.i32 = align },
                           { .kind = WASM_I32, .of.i32 = new_size } };
#ifdef OS_ENABLE_HW_BOUND_CHECK
    WASMExecEnv *saved_tls = wasm_runtime_get_exec_env_tls();
    wasm_runtime_set_exec_env_tls(NULL);
#endif
    if (!wasm_runtime_call_wasm_a(exec_env,
                                  (WASMFunctionInstanceCommon *)realloc_func, 1,
                                  results, 4, args)) {
        const char *ex = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)realloc_func->module_instance);
#ifdef OS_ENABLE_HW_BOUND_CHECK
        wasm_runtime_set_exec_env_tls(saved_tls);
#endif
        wasm_exec_env_restore_module_inst(exec_env, saved_module_inst);
        set_component_exception(cx, ex ? ex : "realloc call failed");
        return false;
    }
#ifdef OS_ENABLE_HW_BOUND_CHECK
    wasm_runtime_set_exec_env_tls(saved_tls);
#endif
    wasm_exec_env_restore_module_inst(exec_env, saved_module_inst);
    *out_ptr = results[0].of.i32;
    return true;
}

uint32_t
wasm_runtime_call_realloc(LiftLowerContext *cx, int32_t old_ptr,
                          int32_t old_size, int32_t align, int32_t new_size)
{
    uint32_t ptr = 0;

    if (!call_canonical_realloc(cx, old_ptr, old_size, align, new_size, &ptr)) {
        return 0;
    }
    return ptr;
}

bool
wasm_component_cabi_realloc(wasm_exec_env_t exec_env, uint32_t old_offset,
                            uint32_t old_size, uint32_t alignment,
                            uint32_t new_size, uint32_t *out_offset)
{
    WASMExecEnv *internal_exec_env = (WASMExecEnv *)exec_env;
    WASMComponentInstance *saved_component_inst = NULL;
    WASMFunctionInstance *saved_core_func = NULL;
    WASMMemoryInstance *saved_memory = NULL;
    WASMModuleInstanceCommon *saved_module_inst = NULL;
    LiftLowerContext *saved_cx = NULL;
    void *saved_attachment = NULL;
    uint32_t new_offset = 0;
    bool realloc_succeeded = false;
    bool context_preserved = false;

    if (out_offset) {
        *out_offset = 0;
    }
    if (!out_offset || !internal_exec_env
        || !internal_exec_env->component_callback_active
        || !(saved_memory =
                 get_active_component_callback_memory(internal_exec_env))
        || alignment == 0 || alignment > 8 || (alignment & (alignment - 1)) != 0
        || ((old_offset == 0) != (old_size == 0))) {
        return false;
    }

    saved_component_inst = internal_exec_env->component_inst;
    saved_core_func = internal_exec_env->core_func;
    saved_module_inst = wasm_runtime_get_module_inst(internal_exec_env);
    saved_cx = internal_exec_env->cx;
    saved_attachment = internal_exec_env->attachment;

    if (old_size != 0
        && ((old_offset & (alignment - 1)) != 0
            || !wasm_component_validate_memory_range(exec_env, old_offset,
                                                     old_size))) {
        return false;
    }

    realloc_succeeded = call_canonical_realloc(
        saved_cx, (int32_t)old_offset, (int32_t)old_size, (int32_t)alignment,
        (int32_t)new_size, &new_offset);

    context_preserved =
        internal_exec_env->component_callback_active
        && internal_exec_env->component_inst == saved_component_inst
        && internal_exec_env->core_func == saved_core_func
        && internal_exec_env->memory == saved_memory
        && internal_exec_env->cx == saved_cx
        && internal_exec_env->attachment == saved_attachment
        && wasm_runtime_get_module_inst(internal_exec_env) == saved_module_inst;
    if (!context_preserved) {
        internal_exec_env->component_inst = saved_component_inst;
        internal_exec_env->core_func = saved_core_func;
        internal_exec_env->memory = saved_memory;
        internal_exec_env->cx = saved_cx;
        internal_exec_env->attachment = saved_attachment;
        internal_exec_env->component_callback_active = true;
        wasm_exec_env_restore_module_inst(internal_exec_env, saved_module_inst);
        set_component_exception(
            saved_cx, "cabi_realloc changed component callback context");
        return false;
    }
    if (!realloc_succeeded) {
        return false;
    }

    if (new_size == 0) {
        return true;
    }
    if (new_offset == 0 || (new_offset & (alignment - 1)) != 0) {
        set_component_exception(
            saved_cx, "cabi_realloc returned a null or misaligned pointer");
        return false;
    }
    if (!wasm_component_validate_memory_range(exec_env, new_offset, new_size)) {
        return false;
    }

    *out_offset = new_offset;
    return true;
}

/// @brief convert numeric offset to start of memory area inside wasm linear
/// memory to absolute address
/// @param exec_env execution environment holding the desired linear memory
/// @param app_offset offset to start of desired memory area inside wasm linear
/// memory
/// @return pointer to specified memory area inside wasm linear memory
void *
wasm_runtime_addr_app_to_native_p2(WASMExecEnv *exec_env, uint64 app_offset)
{
    WASMMemoryInstance *memory_inst = NULL;
    uint8 *addr = NULL;
    bool bounds_checks = true;

    memory_inst = exec_env->memory;
    if (!memory_inst) {
        return NULL;
    }

    // TODO: to be seen if shared heap functionality is applicable in the
    // constext of component model
#if WASM_ENABLE_SHARED_HEAP != 0
    WASMModuleInstanceCommon *module_inst_comm = get_module_inst(exec_env);
    if (is_app_addr_in_shared_heap(module_inst_comm, memory_inst->is_memory64,
                                   app_offset, 1)) {
        return get_last_used_shared_heap_base_addr_adj(module_inst_comm)
               + app_offset;
    }
#endif

    SHARED_MEMORY_LOCK(memory_inst);

    addr = memory_inst->memory_data + (uintptr_t)app_offset;

#if WASM_CONFIGURABLE_BOUNDS_CHECKS != 0
    bounds_checks = wasm_runtime_is_bounds_checks_enabled(module_inst_comm);
#endif

    if (bounds_checks) {
        if (memory_inst->memory_data <= addr
            && addr < memory_inst->memory_data_end) {
            SHARED_MEMORY_UNLOCK(memory_inst);
            return addr;
        }
        SHARED_MEMORY_UNLOCK(memory_inst);
        return NULL;
    }

    /* If bounds checks is disabled, return the address directly */
    SHARED_MEMORY_UNLOCK(memory_inst);
    return addr;
}

WASMComponentFuncTypeInstance *
wasm_get_component_func_type(WASMExecEnv *exec_env)
{
    if (!exec_env || !exec_env->core_func
        || !exec_env->core_func->component_function
        || !exec_env->core_func->component_function->func_type) {
        return NULL;
    }
    return exec_env->core_func->component_function->func_type;
}

uint64
wasm_runtime_addr_native_to_app_p2(WASMExecEnv *exec_env, void *native_ptr)
{
    WASMMemoryInstance *memory_inst = NULL;
    uint8 *addr = (uint8 *)native_ptr;
    bool bounds_checks = false;
    uint64 ret = 0;

    WASMModuleInstanceCommon *module_inst_comm = get_module_inst(exec_env);

    bh_assert(module_inst_comm->module_type == Wasm_Module_Bytecode
              || module_inst_comm->module_type == Wasm_Module_AoT);

#if WASM_CONFIGURABLE_BOUNDS_CHECKS != 0
    bounds_checks = is_bounds_checks_enabled(module_inst_comm);
#endif

    memory_inst = exec_env->memory;
    if (!memory_inst) {
        return 0;
    }

#if WASM_ENABLE_SHARED_HEAP != 0
    if (is_native_addr_in_shared_heap(module_inst_comm,
                                      memory_inst->is_memory64, addr, 1)) {
        return (uint64)(uintptr_t)(addr
                                   - get_last_used_shared_heap_base_addr_adj(
                                       module_inst_comm));
    }
#endif

    SHARED_MEMORY_LOCK(memory_inst);

    if (bounds_checks) {
        if (memory_inst->memory_data <= addr
            && addr < memory_inst->memory_data_end) {
            ret = (uint64)(addr - memory_inst->memory_data);
            SHARED_MEMORY_UNLOCK(memory_inst);
            return ret;
        }
    }
    /* If bounds checks is disabled, return the offset directly */
    else if (addr != NULL) {
        ret = (uint64)(addr - memory_inst->memory_data);
        SHARED_MEMORY_UNLOCK(memory_inst);
        return ret;
    }

    SHARED_MEMORY_UNLOCK(memory_inst);
    return 0;
}

void
invokeNative(GenericFunctionPointer f, uint64 *args, uint64 n_stacks);

typedef float64 (*Float64FuncPtr)(GenericFunctionPointer, uint64 *, uint64);
typedef float32 (*Float32FuncPtr)(GenericFunctionPointer, uint64 *, uint64);
typedef int64 (*Int64FuncPtr)(GenericFunctionPointer, uint64 *, uint64);
typedef int32 (*Int32FuncPtr)(GenericFunctionPointer, uint64 *, uint64);
typedef void (*VoidFuncPtr)(GenericFunctionPointer, uint64 *, uint64);

// NOLINTBEGIN(performance-no-int-to-ptr)
static volatile Float64FuncPtr invokeNative_Float64 =
    (Float64FuncPtr)(uintptr_t)invokeNative;
static volatile Float32FuncPtr invokeNative_Float32 =
    (Float32FuncPtr)(uintptr_t)invokeNative;
static volatile Int64FuncPtr invokeNative_Int64 =
    (Int64FuncPtr)(uintptr_t)invokeNative;
static volatile Int32FuncPtr invokeNative_Int32 =
    (Int32FuncPtr)(uintptr_t)invokeNative;
static volatile VoidFuncPtr invokeNative_Void =
    (VoidFuncPtr)(uintptr_t)invokeNative;
// NOLINTEND(performance-no-int-to-ptr)

/// @brief Executes a wasi preview 2 function wrapper
/// @param exec_env current execution enviroment
/// @param cur_func wasi preview 2 function to be executed
/// @param argv function arguments
/// @param argc number of passed function argumnets
/// @param argv_ret return value fo the invoked wasi p2 function
/// @return true on successfull execution, false on failure
bool
wasm_runtime_invoke_native_p2(WASMExecEnv *exec_env,
                              WASMFunctionInstance *cur_func, uint32 *argv,
                              uint32 argc, uint32 *argv_ret)
{
    /* Layout depends on which invokeNative assembly variant is compiled in:
     *   Fast interp (SIMD): invokeNative_em64_simd.s uses movdqu — xmm regs
     *     are 16 bytes each (offsets 0x00-0x70), ints start at 0x80.
     *     fps must be v128* (16-byte steps) to match.
     *   Classic interp (no SIMD): invokeNative_em64.s uses movq — xmm regs
     *     are 8 bytes each (offsets 0x00-0x38), ints start at 0x40.
     *     fps must be uint64* (8-byte steps) to match. */
#if WASM_ENABLE_SIMD != 0 && WASM_ENABLE_FAST_INTERP != 0
    uint64 argv_buf[64] = { 0 };
#else
    uint64 argv_buf[32] = { 0 };
#endif
    uint64 *argv1 = argv_buf, *ints = NULL, *stacks = NULL, size = 0,
           arg_i64 = 0;
    uint32 *argv_src = argv, i = 0, argc1 = 0, n_ints = 0, n_stacks = 0;
    uint32 arg_i32 = 0;
    WASMFuncType *func_type = cur_func->u.func_import->func_type;

    WASMModuleInstance *module =
        (WASMModuleInstance *)wasm_runtime_get_module_inst(exec_env);

    WASMComponentInstance *saved_component_inst = exec_env->component_inst;
    WASMFunctionInstance *saved_core_func = exec_env->core_func;
    WASMMemoryInstance *saved_memory = exec_env->memory;
    LiftLowerContext *saved_cx = exec_env->cx;
    void *saved_attachment = exec_env->attachment;

    WASMComponentInstance *root_comp = module->comp_instance;
    while (root_comp && root_comp->parent)
        root_comp = root_comp->parent;

    exec_env->component_inst = root_comp;

    void *func_ptr = cur_func->u.func_import->func_ptr_linked;
    void *attachment = cur_func->u.func_import->attachment;

    uint32 result_count = func_type->result_count;
    uint32 ext_ret_count = result_count > 1 ? result_count - 1 : 0;
    bool ret = false;
    const char *signature = cur_func->u.func_import->signature;
#if WASM_ENABLE_SIMD != 0 && WASM_ENABLE_FAST_INTERP != 0
    V128 *fps = NULL;
#else
    uint64 *fps = NULL;
#endif
    uint64 app_offset = 0, ptr_len = 0;
    int n_fps = 0;

#if WASM_ENABLE_SIMD != 0 && WASM_ENABLE_FAST_INTERP != 0
    /* SIMD assembly: each xmm slot = 16 bytes = 2 uint64 slots */
    argc1 = 1 + COMPONENT_MAX_REG_FLOATS * 2 + (uint32)func_type->param_count
            + ext_ret_count;
#else
    /* Non-SIMD assembly: each xmm slot = 8 bytes = 1 uint64 slot */
    argc1 = 1 + COMPONENT_MAX_REG_FLOATS + (uint32)func_type->param_count
            + ext_ret_count;
#endif

    if (argc1 > sizeof(argv_buf) / sizeof(uint64)) {
        size = (uint64)(sizeof(uint64) * (uint64)argc1);
        argv1 = wasm_runtime_malloc((uint32)size);
        if (!argv1) {
            exec_env->component_inst = saved_component_inst;
            return false;
        }
    }

    WASMMemoryInstance *memory = canon_get_memory(cur_func->canon_options);

    Subtask *subtask = subtask_create();
    if (!subtask) {
        wasm_set_exception(module, "failed to create subtask");
        if (argv1 != argv_buf)
            wasm_runtime_free(argv1);
        exec_env->component_inst = saved_component_inst;
        return false;
    }

    LiftLowerContext cx;
    cx.canonical_opts = cur_func->canon_options;
    cx.inst = module->comp_instance;
    cx.borrow_scope_type = BORROW_SCOPE_SUBTASK;
    cx.borrow_scope.subtask = subtask;

    exec_env->cx = &cx;

    exec_env->memory = memory;
    exec_env->core_func = cur_func;

#if WASM_ENABLE_SIMD != 0 && WASM_ENABLE_FAST_INTERP != 0
    /* Each SIMD register occupies two uint64 slots in argv1. */
    fps = (V128 *)argv1;
    ints = argv1 + COMPONENT_MAX_REG_FLOATS * 2;
#else
    /* fp slots are 8 bytes; integer registers follow. */
    fps = argv1;
    ints = fps + COMPONENT_MAX_REG_FLOATS;
#endif

    stacks = ints + COMPONENT_MAX_REG_INTS;

    ints[n_ints++] = (uint64)(uintptr_t)exec_env;

    for (i = 0; i < func_type->param_count; i++) {
        switch (func_type->types[i]) {
            case VALUE_TYPE_I32:
#if WASM_ENABLE_GC == 0 && WASM_ENABLE_REF_TYPES != 0
            case VALUE_TYPE_FUNCREF:
#endif
            {
                arg_i32 = *argv_src++;
                arg_i64 = arg_i32;
                if (signature
#if WASM_ENABLE_MEMORY64 != 0
                    && !is_memory64
#endif
                ) {
                    if (signature[i + 1] == '\0')
                        goto fail; // TODO: temporary solution untill func type
                                   // flatten is available at instantiation
                                   // (then signature integrity will be checked
                                   // there)
                    if (signature[i + 1] == '*') {
                        /* param is a pointer */
                        app_offset = (uint64)arg_i32;

                        if (signature[i + 2] == '~')
                            /* pointer with length followed */
                            ptr_len = *argv_src;
                        else
                            /* pointer without length followed */
                            ptr_len = 1;

                        if (!wasm_runtime_validate_app_addr_p2(
                                exec_env, app_offset, ptr_len)) {
                            LOG_ERROR("Invalid memory access to wasm memory, "
                                      "at offset %d",
                                      app_offset);
                            goto fail;
                        }
                        arg_i64 = (uint64)memory->memory_data + app_offset;
                    }
                    else if (signature[i + 1] == '$') {
                        /* param is a string */
                        app_offset = (uint64)arg_i32;
                        arg_i64 = (uint64)memory->memory_data + app_offset;
                    }
                }
                if (n_ints < COMPONENT_MAX_REG_INTS)
                    ints[n_ints++] = arg_i64;
                else
                    stacks[n_stacks++] = arg_i64;
                break;
            }
            case VALUE_TYPE_I64:
                if (n_ints < COMPONENT_MAX_REG_INTS)
                    ints[n_ints++] = *(uint64 *)argv_src;
                else
                    stacks[n_stacks++] = *(uint64 *)argv_src;
                argv_src += 2;
                break;
            case VALUE_TYPE_F32:
                if (n_fps < COMPONENT_MAX_REG_FLOATS) {
                    *(float32 *)&fps[n_fps++] = *(float32 *)argv_src++;
                }
                else {
                    *(float32 *)&stacks[n_stacks++] = *(float32 *)argv_src++;
                }
                break;
            case VALUE_TYPE_F64:
                if (n_fps < COMPONENT_MAX_REG_FLOATS) {
                    *(float64 *)&fps[n_fps++] = *(float64 *)argv_src;
                }
                else {
                    *(float64 *)&stacks[n_stacks++] = *(float64 *)argv_src;
                }
                argv_src += 2;
                break;
            default:
                bh_assert(0);
                break;
        }
    }
    /* Save extra result values' address to argv1 */
    for (i = 0; i < ext_ret_count; i++) {
        if (n_ints < COMPONENT_MAX_REG_INTS)
            ints[n_ints++] = *(uint64 *)argv_src;
        else
            stacks[n_stacks++] = *(uint64 *)argv_src;
        argv_src += 2;
    }

    exec_env->attachment = attachment;
    if (result_count == 0) {
        invokeNative_Void(func_ptr, argv1, n_stacks);
    }
    else {
        /* Invoke the native function and get the first result value */
        switch (func_type->types[func_type->param_count]) {
            case VALUE_TYPE_I32:
#if WASM_ENABLE_GC == 0 && WASM_ENABLE_REF_TYPES != 0
            case VALUE_TYPE_FUNCREF:
#endif
                argv_ret[0] =
                    (uint32)invokeNative_Int32(func_ptr, argv1, n_stacks);
                break;
            case VALUE_TYPE_I64:
#if WASM_ENABLE_GC != 0
            case REF_TYPE_FUNCREF:
            case REF_TYPE_EXTERNREF:
            case REF_TYPE_ANYREF:
            case REF_TYPE_EQREF:
            case REF_TYPE_HT_NULLABLE:
            case REF_TYPE_HT_NON_NULLABLE:
            case REF_TYPE_I31REF:
            case REF_TYPE_NULLFUNCREF:
            case REF_TYPE_NULLEXTERNREF:
            case REF_TYPE_STRUCTREF:
            case REF_TYPE_ARRAYREF:
            case REF_TYPE_NULLREF:
#if WASM_ENABLE_STRINGREF != 0
            case REF_TYPE_STRINGREF:
            case REF_TYPE_STRINGVIEWWTF8:
            case REF_TYPE_STRINGVIEWWTF16:
            case REF_TYPE_STRINGVIEWITER:
#endif
#endif
                PUT_I64_TO_ADDR(argv_ret,
                                invokeNative_Int64(func_ptr, argv1, n_stacks));
                break;
            case VALUE_TYPE_F32:
            {
                float32 value = invokeNative_Float32(func_ptr, argv1, n_stacks);
                memcpy(argv_ret, &value, sizeof(value));
                break;
            }
            case VALUE_TYPE_F64:
            {
                float64 value = invokeNative_Float64(func_ptr, argv1, n_stacks);
                memcpy(argv_ret, &value, sizeof(value));
                break;
            }
            default:
                bh_assert(0);
                break;
        }
    }
    exec_env->attachment = saved_attachment;

    subtask->state = SUBTASK_STATE_RETURNED;

    subtask_deliver_resolve(subtask);

    ret = !wasm_copy_exception(module, NULL);

fail:
    exec_env->attachment = saved_attachment;
    exec_env->cx = saved_cx;
    exec_env->memory = saved_memory;
    exec_env->core_func = saved_core_func;
    exec_env->component_inst = saved_component_inst;
    if (argv1 != argv_buf)
        wasm_runtime_free(argv1);
    subtask_destroy(subtask);

    exec_env->attachment = saved_attachment;
    exec_env->cx = saved_cx;
    exec_env->memory = saved_memory;
    exec_env->core_func = saved_core_func;
    exec_env->component_inst = saved_component_inst;

    return ret;
}

#if WASM_ENABLE_LIBC_WASI_P2 != 0

bool
wasm_component_runtime_init_wasi(
    WASMComponentInstance *comp_instance, const char *dir_list[],
    uint32 dir_count, const char *map_dir_list[], uint32 map_dir_count,
    const char *env[], uint32 env_count, const char *addr_pool[],
    uint32 addr_pool_size, const char *ns_lookup_pool[],
    uint32 ns_lookup_pool_size, char *argv[], uint32 argc,
    os_raw_file_handle stdinfd, os_raw_file_handle stdoutfd,
    os_raw_file_handle stderrfd, const libc_wasi_options_t *wasi_options,
    char *error_buf, uint32 error_buf_size)
{
    WASIContext *wasi_ctx = NULL;
    libc_wasi_options_t *owned_options =
        wasm_runtime_malloc(sizeof(*owned_options));

    if (!owned_options) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Failed to allocate WASI Preview 2 options");
        return false;
    }
    if (wasi_options) {
        *owned_options = *wasi_options;
    }
    else {
        memset(owned_options, 0, sizeof(*owned_options));
        owned_options->cli = 1;
        owned_options->common = 1;
        owned_options->preview2 = 1;
    }

    wasi_ctx = wasm_runtime_init_wasi_internal(
        dir_list, dir_count, map_dir_list, map_dir_count, env, env_count,
        addr_pool, addr_pool_size, ns_lookup_pool, ns_lookup_pool_size, argv,
        argc, stdinfd, stdoutfd, stderrfd, error_buf, error_buf_size);

    if (!wasi_ctx) {
        wasm_runtime_free(owned_options);
        LOG_DEBUG("Component wasi args init failed\n");
        return false;
    }
    wasi_ctx->wasi_options = owned_options;
    comp_instance->wasi_ctx = wasi_ctx;
    return true;
}

static WASIArguments *
get_wasi_component_args_from_module(WASMComponent *component)
{
    WASIArguments *wasi_args = NULL;

    wasi_args = &(component)->wasi_args;

    return wasi_args;
}

void
wasm_component_runtime_set_wasi_args_ex(
    WASMComponent *component, const char *dir_list[], uint32 dir_count,
    const char *map_dir_list[], uint32 map_dir_count, const char *env_list[],
    uint32 env_count, char *argv[], int argc, int64 stdinfd, int64 stdoutfd,
    int64 stderrfd)
{
    if (!component) {
        return;
    }

    WASIArguments *wasi_args = get_wasi_component_args_from_module(component);

    bh_assert(wasi_args);

    wasi_args->dir_list = dir_list;
    wasi_args->dir_count = dir_count;
    wasi_args->map_dir_list = map_dir_list;
    wasi_args->map_dir_count = map_dir_count;
    wasi_args->env = env_list;
    wasi_args->env_count = env_count;
    wasi_args->argv = argv;
    wasi_args->argc = (uint32)argc;
    wasi_args->stdio[0] = (os_raw_file_handle)stdinfd;
    wasi_args->stdio[1] = (os_raw_file_handle)stdoutfd;
    wasi_args->stdio[2] = (os_raw_file_handle)stderrfd;
    wasi_args->set_by_user = true;

    component->import_wasi_api = true;
}

void
wasm_component_runtime_set_wasi_args(WASMComponent *component,
                                     const char *dir_list[], uint32 dir_count,
                                     const char *map_dir_list[],
                                     uint32 map_dir_count,
                                     const char *env_list[], uint32 env_count,
                                     char *argv[], int argc)
{
    wasm_component_runtime_set_wasi_args_ex(
        component, dir_list, dir_count, map_dir_list, map_dir_count, env_list,
        env_count, argv, argc, -1, -1, -1);
}

void
wasm_component_runtime_set_wasi_addr_pool(WASMComponent *component,
                                          const char *addr_pool[],
                                          uint32 addr_pool_size)
{
    if (!component) {
        return;
    }

    WASIArguments *wasi_args = get_wasi_component_args_from_module(component);

    if (wasi_args) {
        wasi_args->addr_pool = addr_pool;
        wasi_args->addr_count = addr_pool_size;
        wasi_args->set_by_user = true;
    }
}

void
wasm_component_runtime_set_wasi_ns_lookup_pool(WASMComponent *component,
                                               const char *ns_lookup_pool[],
                                               uint32 ns_lookup_pool_size)
{
    if (!component) {
        return;
    }

    WASIArguments *wasi_args = get_wasi_component_args_from_module(component);

    if (wasi_args) {
        wasi_args->ns_lookup_pool = ns_lookup_pool;
        wasi_args->ns_lookup_count = ns_lookup_pool_size;
        wasi_args->set_by_user = true;
    }
}

void
wasm_component_runtime_set_wasi_options(WASMComponent *component,
                                        libc_wasi_options_t *wasi_options)
{
    if (!component) {
        return;
    }

    WASIArguments *wasi_args = get_wasi_component_args_from_module(component);

    if (wasi_args) {
        wasi_args->wasi_options = wasi_options;
        wasi_args->set_by_user = true;
    }
}

WASMComponentFunctionInstance *
wasm_component_runtime_lookup_wasi_start_function(
    WASMComponentInstance *component_inst)
{
    WASMComponentFunctionInstance *func =
        wasm_component_lookup_function(component_inst, "_start");
    if (func) {
        if (func->func_type->params->count != 0
            || func->func_type->results->count != 0) {
            LOG_ERROR("Lookup wasi _start function failed: "
                      "invalid function type.\n");
            return NULL;
        }
        return func;
    }
    return NULL;
}

#endif /* WASM_ENABLE_LIBC_WASI_P2 != 0 */
#endif /* WASM_ENABLE_COMPONENT_MODEL != 0*/
