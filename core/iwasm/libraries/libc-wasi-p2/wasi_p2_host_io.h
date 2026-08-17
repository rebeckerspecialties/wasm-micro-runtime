/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASI_P2_HOST_IO_H
#define WASI_P2_HOST_IO_H

#include "component-model/wasm_component_host_resource.h"
#include "wasi_p2_types.h"
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

bool
wasi_p2_is_callback_input_stream(const HostResource *resource);

void
wasi_p2_callback_input_stream_read(HostResource *resource, uint64_t len,
                                   wasi_result_list_u8_stream_error_t *ret);

/* Read into caller-owned scratch storage. The returned list aliases `buffer`
 * and must not be freed. This is used by multi-step operations that reserve
 * all fallible storage before invoking the first host callback. */
void
wasi_p2_callback_input_stream_read_into(
    HostResource *resource, uint64_t len, uint8_t *buffer,
    uint32_t buffer_capacity, wasi_result_list_u8_stream_error_t *ret);

void
wasi_p2_callback_input_stream_skip(HostResource *resource, uint64_t len,
                                   wasi_result_u64_stream_error_t *ret);

void
wasi_p2_callback_input_stream_splice(HostResource *resource,
                                     wasi_output_stream_t output_stream,
                                     uint64_t len, bool blocking,
                                     wasi_result_u64_stream_error_t *ret);

bool
wasi_p2_host_pollable_ready(void *host_context);

void
wasi_p2_host_pollable_drain(void *host_context);

#ifdef __cplusplus
}
#endif

#endif /* WASI_P2_HOST_IO_H */
