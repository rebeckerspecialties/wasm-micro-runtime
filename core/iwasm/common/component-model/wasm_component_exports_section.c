/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "wasm_loader_common.h"
#include "wasm_runtime_common.h"
#include "wasm_export.h"
#include <stdio.h>

// Section 11: exports section
bool
wasm_component_parse_exports_section(const uint8_t **payload,
                                     uint32_t payload_len,
                                     WASMComponentExportSection *out,
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

    if (p >= end) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Unexpected end of buffer when reading exports count");
        if (consumed_len)
            *consumed_len = (uint32_t)(p - *payload);
        return false;
    }
    uint64_t export_count_leb = 0;
    if (!read_leb((uint8_t **)&p, end, 32, false, &export_count_leb, error_buf,
                  error_buf_size)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Failed to read exports count");
        if (consumed_len)
            *consumed_len = (uint32_t)(p - *payload);
        return false;
    }

    uint32_t export_count = (uint32_t)export_count_leb;
    out->count = export_count;

    if (export_count > 0) {
        out->exports = wasm_component_checked_calloc(
            export_count, sizeof(WASMComponentExport), p, end, 1,
            "component export", error_buf, error_buf_size);
        if (!out->exports) {
            if (consumed_len)
                *consumed_len = (uint32_t)(p - *payload);
            return false;
        }

        // Parsing every export
        for (uint32_t i = 0; i < export_count; i++) {
            // Parsing 'exportname'
            if (p >= end) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Unexpected end of buffer when reading export "
                                 "name for export %u",
                                 i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            WASMComponentExportName *export_name =
                wasm_runtime_malloc(sizeof(WASMComponentExportName));
            if (!export_name) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to allocate memory for export_name");
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            memset(export_name, 0, sizeof(*export_name));
            out->exports[i].export_name = export_name;

            bool status = parse_component_export_name(
                &p, end, export_name, error_buf, error_buf_size);
            if (!status) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to parse component name for export %u",
                                 i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            // Parsing 'sortidx'
            out->exports[i].sort_idx =
                wasm_runtime_malloc(sizeof(WASMComponentSortIdx));
            if (!out->exports[i].sort_idx) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to allocate memory for sort_idx");
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            memset(out->exports[i].sort_idx, 0,
                   sizeof(*out->exports[i].sort_idx));

            status = parse_sort_idx(&p, end, out->exports[i].sort_idx,
                                    error_buf, error_buf_size, false);
            if (!status) {
                set_error_buf_ex(error_buf, error_buf_size,
                                 "Failed to parse sort_idx for export %u", i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            LOG_DEBUG("Export section: name = \"%s\", sort idx = %d\n",
                      out->exports[i].export_name->exported.simple.name->name,
                      out->exports[i].sort_idx->sort->sort);

            // Parsing 'externdesc' (OPTIONAL)
            if (p >= end) {
                LOG_DEBUG("Parsing Extern desc\n");

                set_error_buf_ex(error_buf, error_buf_size,
                                 "Unexpected end of buffer when reading "
                                 "optional extern_desc for export %u",
                                 i);
                if (consumed_len)
                    *consumed_len = (uint32_t)(p - *payload);
                return false;
            }
            uint8_t opt_extern_desc = *p++;
            if (opt_extern_desc == WASM_COMP_OPTIONAL_TRUE) {
                if (p >= end) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "Unexpected end of buffer when parsing "
                                     "extern_desc for export %u",
                                     i);
                    if (consumed_len)
                        *consumed_len = (uint32_t)(p - *payload);
                    return false;
                }
                WASMComponentExternDesc *extern_desc =
                    wasm_runtime_malloc(sizeof(WASMComponentExternDesc));
                if (!extern_desc) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "Failed to allocate extern_desc for "
                                     "export %u",
                                     i);
                    if (consumed_len)
                        *consumed_len = (uint32_t)(p - *payload);
                    return false;
                }
                memset(extern_desc, 0, sizeof(*extern_desc));
                out->exports[i].extern_desc = extern_desc;
                bool extern_status = parse_extern_desc(
                    &p, end, extern_desc, error_buf, error_buf_size);
                if (!extern_status) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "Failed to parse extern_desc for export %u", i);
                    if (consumed_len)
                        *consumed_len = (uint32_t)(p - *payload);
                    return false;
                }
                LOG_DEBUG("Extern desc added\n");
            }
            else if (opt_extern_desc == WASM_COMP_OPTIONAL_FALSE) {
                // Explicitly mark absence of extern_desc
                out->exports[i].extern_desc = NULL;
                LOG_DEBUG("Extern desc set to NULL\n");
            }
            else {
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "Malformed binary: invalid optional tag 0x%02x",
                    opt_extern_desc);
                return false;
            }
        }
    }

    if (consumed_len)
        *consumed_len = (uint32_t)(p - *payload);

    return true;
}

// Individual section free functions
void
wasm_component_free_exports_section(WASMComponentSection *section)
{
    if (!section || !section->parsed.export_section)
        return;

    WASMComponentExportSection *export_sec = section->parsed.export_section;
    if (export_sec->exports) {
        for (uint32_t j = 0; j < export_sec->count; ++j) {
            WASMComponentExport *export = &export_sec->exports[j];

            // Free export name
            if (export->export_name) {
                free_component_export_name(export->export_name);
                wasm_runtime_free(export->export_name);
                export->export_name = NULL;
            }

            // Free sort index
            if (export->sort_idx) {
                if (export->sort_idx->sort) {
                    wasm_runtime_free(export->sort_idx->sort);
                    export->sort_idx->sort = NULL;
                }
                wasm_runtime_free(export->sort_idx);
                export->sort_idx = NULL;
            }

            // Free extern desc (optional)
            if (export->extern_desc) {
                free_extern_desc(export->extern_desc);
                wasm_runtime_free(export->extern_desc);
                export->extern_desc = NULL;
            }
        }
        wasm_runtime_free(export_sec->exports);
        export_sec->exports = NULL;
    }
    wasm_runtime_free(export_sec);
    section->parsed.export_section = NULL;
}
