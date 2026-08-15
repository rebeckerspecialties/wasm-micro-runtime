/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASI_P2_IO_H
#define WASI_P2_IO_H

#include <stdint.h>
#include <stdbool.h>

#include "wasi_p2_types.h"

struct WASMExecEnv;
typedef struct WASMExecEnv *wasm_exec_env_t;
struct HostResource;
struct StreamResourceType;

#ifdef __cplusplus
extern "C" {
#endif

// wasi:io/error
void
wasi_pollable_block(wasi_pollable_context_t *pollable);

typedef enum wasi_p2_wait_status {
    WASI_P2_WAIT_READY,
    WASI_P2_WAIT_INTERRUPTED,
    WASI_P2_WAIT_FAILED,
} wasi_p2_wait_status_t;

wasi_p2_wait_status_t
wasi_pollable_block_interruptible(wasm_exec_env_t exec_env,
                                  wasi_pollable_context_t *pollable);

bool
wasi_pollable_ready(wasi_pollable_context_t *pollable);

void
wasi_poll(const wasi_pollable_context_t **pollables, uint32_t n_pollables,
          wasi_list_u32_t *ret);

wasi_p2_wait_status_t
wasi_poll_interruptible(wasm_exec_env_t exec_env,
                        const wasi_pollable_context_t **pollables,
                        uint32_t n_pollables, wasi_list_u32_t *ret);

void
wasi_p2_interrupt_wait_request(wasm_exec_env_t exec_env);

void
wasi_p2_interrupt_wait_destroy(wasm_exec_env_t exec_env);

// wasi:io/streams
void
wasi_input_stream_read(wasi_input_stream_t stream, uint64_t len,
                       wasi_result_list_u8_stream_error_t *ret);

void
wasi_input_stream_blocking_read(wasi_input_stream_t stream, uint64_t len,
                                wasi_result_list_u8_stream_error_t *ret);

void
wasi_input_stream_skip(wasi_input_stream_t stream, uint64_t len,
                       wasi_result_u64_stream_error_t *ret);

void
wasi_input_stream_blocking_skip(wasi_input_stream_t stream, uint64_t len,
                                wasi_result_u64_stream_error_t *ret);

void
wasi_output_stream_check_write(wasi_output_stream_t stream,
                               wasi_result_u64_stream_error_t *ret);

void
wasi_output_stream_write(wasi_output_stream_t stream,
                         const wasi_list_u8_t *payload,
                         wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_blocking_write_and_flush(
    wasi_output_stream_t stream, const wasi_list_u8_t *payload,
    wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_flush(wasi_output_stream_t stream,
                         wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_blocking_flush(wasi_output_stream_t stream,
                                  wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_write_zeroes(wasi_output_stream_t stream, uint64_t len,
                                wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_blocking_write_zeroes_and_flush(
    wasi_output_stream_t stream, uint64_t len,
    wasi_result_void_stream_error_t *ret);

void
wasi_output_stream_splice(wasi_output_stream_t stream, wasi_input_stream_t src,
                          uint64_t len, wasi_result_u64_stream_error_t *ret);

void
wasi_output_stream_blocking_splice(wasi_output_stream_t stream,
                                   wasi_input_stream_t src, uint64_t len,
                                   wasi_result_u64_stream_error_t *ret);

/* Resource-aware stream operations. File streams carry a logical cursor and
 * use positional I/O so duplicating a filesystem descriptor never aliases its
 * kernel cursor or append flags. Non-file streams delegate to the ordinary
 * descriptor-based operations above. */
void
wasi_p2_stream_resource_read(struct StreamResourceType *stream, uint64_t len,
                             bool blocking,
                             wasi_result_list_u8_stream_error_t *ret);

void
wasi_p2_stream_resource_skip(struct StreamResourceType *stream, uint64_t len,
                             bool blocking,
                             wasi_result_u64_stream_error_t *ret);

void
wasi_p2_stream_resource_write(struct StreamResourceType *stream,
                              const wasi_list_u8_t *payload, bool blocking,
                              bool flush, wasi_result_void_stream_error_t *ret);

void
wasi_p2_stream_resource_write_zeroes(struct StreamResourceType *stream,
                                     uint64_t len, bool blocking, bool flush,
                                     wasi_result_void_stream_error_t *ret);

void
wasi_p2_stream_resources_splice(struct StreamResourceType *stream,
                                struct StreamResourceType *src, uint64_t len,
                                bool blocking,
                                wasi_result_u64_stream_error_t *ret);

void
wasi_p2_callback_input_stream_to_resource_splice(
    struct HostResource *src, struct StreamResourceType *stream, uint64_t len,
    bool blocking, wasi_result_u64_stream_error_t *ret);

/* Filesystem descriptor writes/truncates use the same lock as logical file
 * streams so append selection and positional mutations are serialized across
 * every guest-visible file operation in this process. */
void
wasi_p2_file_io_lock(void);

void
wasi_p2_file_io_unlock(void);

void
pollable_dtor(void *data);

#ifdef __cplusplus
}
#endif

#endif /* WASI_IO_H */
