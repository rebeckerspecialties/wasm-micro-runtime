/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component.h"
#include "../interpreter/wasm.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "wasm_loader_common.h"
#include "wasm_runtime_common.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
#include "wasm_component_validate.h"
#include <stdio.h>

/*
 * Install each section wrapper in the component before parsing its payload.
 * Section parsers fill zero-initialized structures incrementally, so this
 * makes the normal component destructor the single owner of partial results.
 */
#define ALLOC_SECTION_WRAPPER(section, member, type, label)           \
    do {                                                              \
        type *wrapper = wasm_runtime_malloc(sizeof(type));            \
        if (!wrapper) {                                               \
            set_error_buf_ex(error_buf, error_buf_size,               \
                             "Failed to allocate %s section", label); \
            goto fail;                                                \
        }                                                             \
        memset(wrapper, 0, sizeof(type));                             \
        (section)->parsed.member = wrapper;                           \
    } while (0)

bool
wasm_component_parse_sections_ex(const uint8_t *buf, uint32_t size,
                                 WASMComponent *out_component, LoadArgs *args,
                                 unsigned int depth, char *error_buf,
                                 uint32_t error_buf_size)
{
    const uint8_t *p, *end;
    WASMComponentSection *sections;
    uint32_t section_capacity = 8;

    if (!buf || size < 8 || !out_component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Invalid component buffer or output pointer");
        return false;
    }
    if (out_component->sections || out_component->section_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Component output is already initialized");
        return false;
    }
    if (!wasm_decode_header(buf, size, &out_component->header)
        || !is_wasm_component(out_component->header)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Invalid WebAssembly component header");
        return false;
    }

    p = buf + 8;
    end = buf + size;
    sections = wasm_runtime_malloc(section_capacity * sizeof(*sections));
    if (!sections) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Failed to allocate component section table");
        return false;
    }
    memset(sections, 0, section_capacity * sizeof(*sections));
    out_component->sections = sections;

    while (p < end) {
        WASMComponentSection *section;
        const uint8_t *payload_start;
        uint64 payload_len = 0;
        uint32_t consumed_len = 0;
        uint8_t section_id = *p++;
        bool parse_success = false;
        char section_error[256] = { 0 };

        if (!read_leb((uint8_t **)&p, end, 32, false, &payload_len,
                      section_error, sizeof(section_error))) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "Failed to read component section %u size: %s",
                             section_id, section_error);
            goto fail;
        }
        if (payload_len > (uint64)(end - p)) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "Component section %u payload exceeds input",
                             section_id);
            goto fail;
        }

        if (out_component->section_count == section_capacity) {
            WASMComponentSection *new_sections;
            uint32_t old_capacity = section_capacity;
            uint64_t byte_count;

            if (section_capacity > UINT32_MAX / 2) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Component has too many sections");
                goto fail;
            }
            section_capacity *= 2;
            byte_count = (uint64_t)section_capacity * sizeof(*sections);
            if (byte_count > UINT32_MAX) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Component section table is too large");
                goto fail;
            }
            new_sections = wasm_runtime_realloc(sections, (uint32_t)byte_count);
            if (!new_sections) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to grow component section table");
                goto fail;
            }
            sections = new_sections;
            memset(sections + old_capacity, 0,
                   (section_capacity - old_capacity) * sizeof(*sections));
            out_component->sections = sections;
        }

        payload_start = p;
        section = &sections[out_component->section_count++];
        memset(section, 0, sizeof(*section));
        section->id = section_id;
        section->payload = payload_start;
        section->payload_len = (uint32_t)payload_len;

        switch (section_id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
                ALLOC_SECTION_WRAPPER(section, core_custom,
                                      WASMComponentCoreCustomSection, "custom");
                parse_success = wasm_component_parse_core_custom_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.core_custom, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                ALLOC_SECTION_WRAPPER(section, core_module,
                                      WASMComponentCoreModuleWrapper,
                                      "core module");
                parse_success = wasm_component_parse_core_module_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.core_module, args, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                ALLOC_SECTION_WRAPPER(section, core_instance_section,
                                      WASMComponentCoreInstSection,
                                      "core instance");
                parse_success = wasm_component_parse_core_instance_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.core_instance_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_CORE_TYPE:
                ALLOC_SECTION_WRAPPER(section, core_type_section,
                                      WASMComponentCoreTypeSection,
                                      "core type");
                parse_success = wasm_component_parse_core_type_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.core_type_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_COMPONENT:
                ALLOC_SECTION_WRAPPER(section, component, WASMComponent,
                                      "nested component");
                parse_success = wasm_component_parse_component_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.component, section_error,
                    sizeof(section_error), args, depth, &consumed_len);
                break;
            case WASM_COMP_SECTION_INSTANCES:
                ALLOC_SECTION_WRAPPER(section, instance_section,
                                      WASMComponentInstSection, "instance");
                parse_success = wasm_component_parse_instances_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.instance_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_ALIASES:
                ALLOC_SECTION_WRAPPER(section, alias_section,
                                      WASMComponentAliasSection, "alias");
                parse_success = wasm_component_parse_alias_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.alias_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_TYPE:
                ALLOC_SECTION_WRAPPER(section, type_section,
                                      WASMComponentTypeSection, "type");
                parse_success = wasm_component_parse_types_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.type_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_CANONS:
                ALLOC_SECTION_WRAPPER(section, canon_section,
                                      WASMComponentCanonSection, "canon");
                parse_success = wasm_component_parse_canons_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.canon_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_START:
                ALLOC_SECTION_WRAPPER(section, start_section,
                                      WASMComponentStartSection, "start");
                parse_success = wasm_component_parse_start_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.start_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_IMPORTS:
                ALLOC_SECTION_WRAPPER(section, import_section,
                                      WASMComponentImportSection, "import");
                parse_success = wasm_component_parse_imports_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.import_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_EXPORTS:
                ALLOC_SECTION_WRAPPER(section, export_section,
                                      WASMComponentExportSection, "export");
                parse_success = wasm_component_parse_exports_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.export_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            case WASM_COMP_SECTION_VALUES:
                ALLOC_SECTION_WRAPPER(section, value_section,
                                      WASMComponentValueSection, "value");
                parse_success = wasm_component_parse_values_section(
                    &payload_start, (uint32_t)payload_len,
                    section->parsed.value_section, section_error,
                    sizeof(section_error), &consumed_len);
                break;
            default:
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Unsupported component section id %u",
                                 section_id);
                goto fail;
        }

        if (!parse_success) {
            set_error_buf_ex(
                error_buf, error_buf_size,
                "Failed to parse component section %u: %s", section_id,
                section_error[0] ? section_error : "unspecified parser error");
            goto fail;
        }
        if (consumed_len != payload_len) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "Component section %u consumed %u of %u bytes",
                             section_id, consumed_len, (uint32_t)payload_len);
            goto fail;
        }
        p += payload_len;
    }

    return true;

fail:
    wasm_component_free(out_component);
    return false;
}

bool
wasm_component_parse_sections(const uint8_t *buf, uint32_t size,
                              WASMComponent *out_component, LoadArgs *args,
                              unsigned int depth)
{
    return wasm_component_parse_sections_ex(buf, size, out_component, args,
                                            depth, NULL, 0);
}

#undef ALLOC_SECTION_WRAPPER

// Check if Header is Component
bool
is_wasm_component(WASMHeader header)
{
    if (header.magic != WASM_MAGIC_NUMBER
        || header.version != WASM_COMPONENT_VERSION
        || header.layer != WASM_COMPONENT_LAYER) {
        return false;
    }

    return true;
}

WASMComponent *
wasm_component_load(uint8_t *buf, uint32_t size, const LoadArgs *load_args,
                    char *error_buf, uint32_t error_buf_size)
{
    WASMComponent *component = NULL;
    LoadArgs args = { 0 };
    static char default_name[] = "Component";

    if (!buf || size == 0) {
        set_error_buf_ex(error_buf, error_buf_size, "Invalid component buffer");
        return NULL;
    }
    if (load_args) {
        args = *load_args;
    }
    if (!args.name) {
        args.name = default_name;
    }
    args.is_component = true;

    component = wasm_runtime_malloc(sizeof(*component));
    if (!component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Failed to allocate component");
        return NULL;
    }
    memset(component, 0, sizeof(*component));
#if WASM_ENABLE_LIBC_WASI_P2 != 0
    wasi_args_set_defaults(&component->wasi_args);
#endif

    if (!wasm_component_parse_sections_ex(buf, size, component, &args, 0,
                                          error_buf, error_buf_size)) {
        wasm_component_free(component);
        wasm_runtime_free(component);
        return NULL;
    }
    if (!wasm_component_validate(component, NULL, error_buf, error_buf_size)) {
        wasm_component_free(component);
        wasm_runtime_free(component);
        return NULL;
    }

    return component;
}

WASMComponent *
wasm_component_load_ex2(uint8_t *buf, uint32_t size, const LoadArgs2 *load_args,
                        char *error_buf, uint32_t error_buf_size)
{
    WASMAllocationQuotaScope scope;
    LoadArgs legacy_args;
    WASMComponent *component;

    if (!wasm_runtime_load_args2_normalize(load_args, &legacy_args, error_buf,
                                           error_buf_size)) {
        return NULL;
    }
    legacy_args.is_component = true;
    if (!wasm_allocation_quota_scope_enter_for_load(&scope, load_args)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Allocation quota owner could not be established");
        return NULL;
    }

    component =
        wasm_component_load(buf, size, &legacy_args, error_buf, error_buf_size);
    wasm_allocation_quota_scope_leave(&scope);
    return component;
}

static char *
component_string_dup(const char *value)
{
    size_t length;
    char *copy;

    if (!value || (length = strlen(value)) >= UINT32_MAX) {
        return NULL;
    }
    copy = wasm_runtime_malloc((uint32_t)length + 1);
    if (copy) {
        memcpy(copy, value, length + 1);
    }
    return copy;
}

bool
wasm_component_register_host_resource_drop_callback(
    WASMComponent *component, const char *interface_name,
    const char *resource_name,
    wasm_component_host_resource_drop_callback_t callback)
{
    WASMComponentHostResourceDropRegistration *registration;
    char *interface_copy = NULL, *resource_copy = NULL;

    if (!component || !interface_name || !interface_name[0] || !resource_name
        || !resource_name[0] || !callback) {
        return false;
    }

    for (uint32_t idx = 0; idx < component->host_resource_drop_count; idx++) {
        registration = &component->host_resource_drops[idx];
        if (strcmp(registration->interface_name, interface_name) == 0
            && strcmp(registration->resource_name, resource_name) == 0) {
            registration->callback = callback;
            return true;
        }
    }

    interface_copy = component_string_dup(interface_name);
    resource_copy = component_string_dup(resource_name);
    if (!interface_copy || !resource_copy) {
        goto fail;
    }

    if (component->host_resource_drop_count
        == component->host_resource_drop_capacity) {
        uint32_t new_capacity = component->host_resource_drop_capacity
                                    ? component->host_resource_drop_capacity * 2
                                    : 4;
        uint64_t byte_count =
            (uint64_t)new_capacity * sizeof(*component->host_resource_drops);
        WASMComponentHostResourceDropRegistration *new_registrations;

        if (new_capacity < component->host_resource_drop_capacity
            || byte_count > UINT32_MAX) {
            goto fail;
        }
        new_registrations = wasm_runtime_realloc(component->host_resource_drops,
                                                 (uint32_t)byte_count);
        if (!new_registrations) {
            goto fail;
        }
        component->host_resource_drops = new_registrations;
        component->host_resource_drop_capacity = new_capacity;
    }

    registration =
        &component->host_resource_drops[component->host_resource_drop_count++];
    registration->interface_name = interface_copy;
    registration->resource_name = resource_copy;
    registration->callback = callback;
    return true;

fail:
    if (interface_copy) {
        wasm_runtime_free(interface_copy);
    }
    if (resource_copy) {
        wasm_runtime_free(resource_copy);
    }
    return false;
}

bool
wasm_component_find_host_resource_drop_callback(
    const WASMComponent *component, const char *interface_name,
    const char *resource_name,
    wasm_component_host_resource_drop_callback_t *callback)
{
    if (!component || !interface_name || !resource_name || !callback) {
        return false;
    }

    for (uint32_t idx = 0; idx < component->host_resource_drop_count; idx++) {
        const WASMComponentHostResourceDropRegistration *registration =
            &component->host_resource_drops[idx];
        if (strcmp(registration->interface_name, interface_name) == 0
            && strcmp(registration->resource_name, resource_name) == 0) {
            *callback = registration->callback;
            return true;
        }
    }
    return false;
}

void
wasm_component_unload(WASMComponent *component)
{
    WASMAllocationQuotaScope scope;

    if (!component)
        return;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&scope, component)) {
        LOG_ERROR("failed to establish allocation quota owner while unloading "
                  "a component");
        return;
    }

    wasm_component_free(component);
    wasm_runtime_free(component);
    wasm_allocation_quota_scope_leave(&scope);
}

// Main component free function
void
wasm_component_free(WASMComponent *component)
{
    if (!component)
        return;

    if (component->sections) {
        for (uint32_t i = 0; i < component->section_count; ++i) {
            WASMComponentSection *sec = &component->sections[i];

            switch (sec->id) {
                case WASM_COMP_SECTION_CORE_CUSTOM: // Section 0
                    wasm_component_free_core_custom_section(sec);
                    break;

                case WASM_COMP_SECTION_CORE_MODULE: // Section 1
                    wasm_component_free_core_module_section(sec);
                    break;

                case WASM_COMP_SECTION_CORE_INSTANCE: // Section 2
                    wasm_component_free_core_instance_section(sec);
                    break;

                case WASM_COMP_SECTION_CORE_TYPE: // Section 3
                    wasm_component_free_core_type_section(sec);
                    break;

                case WASM_COMP_SECTION_COMPONENT: // Section 4
                    wasm_component_free_component_section(sec);
                    break;

                case WASM_COMP_SECTION_INSTANCES: // Section 5
                    wasm_component_free_instances_section(sec);
                    break;

                case WASM_COMP_SECTION_ALIASES: // Section 6
                    wasm_component_free_alias_section(sec);
                    break;

                case WASM_COMP_SECTION_TYPE: // Section 7
                    wasm_component_free_types_section(sec);
                    break;

                case WASM_COMP_SECTION_CANONS: // Section 8
                    wasm_component_free_canons_section(sec);
                    break;

                case WASM_COMP_SECTION_START: // Section 9
                    wasm_component_free_start_section(sec);
                    break;

                case WASM_COMP_SECTION_IMPORTS: // Section 10
                    wasm_component_free_imports_section(sec);
                    break;

                case WASM_COMP_SECTION_EXPORTS: // Section 11
                    wasm_component_free_exports_section(sec);
                    break;

                case WASM_COMP_SECTION_VALUES: // Section 12
                    wasm_component_free_values_section(sec);
                    break;

                default:
                    // For other sections that don't have parsed data or are
                    // stubs Just free the any pointer if it exists
                    if (sec->parsed.any) {
                        wasm_runtime_free(sec->parsed.any);
                        sec->parsed.any = NULL;
                    }
                    break;
            }
        }

        wasm_runtime_free(component->sections);
        component->sections = NULL;
        component->section_count = 0;
    }

    for (uint32_t idx = 0; idx < component->host_resource_drop_count; idx++) {
        wasm_runtime_free(component->host_resource_drops[idx].interface_name);
        wasm_runtime_free(component->host_resource_drops[idx].resource_name);
    }
    if (component->host_resource_drops) {
        wasm_runtime_free(component->host_resource_drops);
    }
    component->host_resource_drops = NULL;
    component->host_resource_drop_count = 0;
    component->host_resource_drop_capacity = 0;
}
