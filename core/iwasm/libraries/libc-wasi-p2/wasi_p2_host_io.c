/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_host_io.h"
#include "wasi_p2_io.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "bh_platform.h"
#include "component-model/wasm_component_runtime.h"

#define WASI_IO_STREAMS_INTERFACE "wasi:io/streams@0.2.6"
#define WASI_IO_POLL_INTERFACE "wasi:io/poll@0.2.6"

typedef struct WasiP2CallbackInputStream {
    IOStreamType type;
    wasm_component_wasi_input_stream_callbacks_t callbacks;
    void *attachment;
    bool dropped;
} WasiP2CallbackInputStream;

struct wasm_component_wasi_pollable_signal {
    korp_mutex lock;
    uint32_t references;
    bool active;
    int read_fd;
    int write_fd;
};

typedef struct WasiP2CallbackPollable {
    wasi_pollable_context_t pollable;
    wasm_component_wasi_pollable_callbacks_t callbacks;
    void *attachment;
    wasm_component_wasi_pollable_signal_t signal;
    bool dropped;
} WasiP2CallbackPollable;

static void
set_stream_error(wasi_stream_error_t *error, wasi_stream_error_kind_t kind,
                 int code)
{
    error->kind = kind;
    error->payload.error = code;
}

static bool
input_stream_callbacks_valid(
    const wasm_component_wasi_input_stream_callbacks_t *callbacks)
{
    return callbacks
           && callbacks->struct_size
                  == sizeof(wasm_component_wasi_input_stream_callbacks_t)
           && callbacks->read && callbacks->skip && callbacks->drop;
}

static void
input_stream_drop(void *data)
{
    WasiP2CallbackInputStream *stream = data;

    if (!stream || stream->dropped) {
        return;
    }
    stream->dropped = true;
    stream->callbacks.drop(stream->attachment);
}

bool
wasm_component_wasi_input_stream_new(
    wasm_exec_env_t exec_env,
    const wasm_component_wasi_input_stream_callbacks_t *callbacks,
    void *attachment, uint32_t *out_handle)
{
    HostResourceTable *table = NULL;
    HostResource *resource = NULL;
    WasiP2CallbackInputStream *stream = NULL;
    uint32_t representation = 0;

    if (out_handle) {
        *out_handle = 0;
    }
    if (!input_stream_callbacks_valid(callbacks)) {
        return false;
    }
    if (!out_handle || !wasm_component_exec_env_is_callback(exec_env)
        || !(table = get_global_host_resource_table())) {
        callbacks->drop(attachment);
        return false;
    }

    resource = host_resource_create(WASI_P2_IO_INPUT_STREAM,
                                    sizeof(WasiP2CallbackInputStream));
    if (!resource) {
        callbacks->drop(attachment);
        return false;
    }
    stream = resource->data;
    memset(stream, 0, sizeof(*stream));
    stream->type = STREAM_TYPE_HOST_CALLBACK;
    stream->callbacks = *callbacks;
    stream->attachment = attachment;
    host_resource_set_dtor(resource, input_stream_drop);

    representation = host_resource_table_add(table, resource);
    if (representation == 0) {
        destroy_host_resource(resource);
        return false;
    }
    if (!wasm_component_host_resource_new(exec_env, WASI_IO_STREAMS_INTERFACE,
                                          "input-stream", representation,
                                          out_handle)) {
        host_resource_table_delete(table, representation);
        return false;
    }
    return true;
}

bool
wasi_p2_is_callback_input_stream(const HostResource *resource)
{
    const StreamResourceType *stream = NULL;

    if (!resource || resource->type != WASI_P2_IO_INPUT_STREAM
        || !resource->data) {
        return false;
    }
    stream = resource->data;
    return stream->type == STREAM_TYPE_HOST_CALLBACK;
}

void
wasi_p2_callback_input_stream_read(HostResource *resource, uint64_t len,
                                   wasi_result_list_u8_stream_error_t *ret)
{
    WasiP2CallbackInputStream *stream = NULL;
    wasm_component_wasi_input_stream_status_t status;
    uint32_t capacity = 0;
    uint32_t bytes_read = 0;
    uint8_t *buffer = NULL;

    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));
    if (!wasi_p2_is_callback_input_stream(resource)) {
        ret->is_err = true;
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (len == 0) {
        return;
    }

    stream = resource->data;
    capacity = len < WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES
                   ? (uint32_t)len
                   : WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES;
    buffer = wasm_runtime_malloc(capacity);
    if (!buffer) {
        ret->is_err = true;
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, ENOMEM);
        return;
    }

    status = stream->callbacks.read(stream->attachment, buffer, capacity,
                                    &bytes_read);
    if (status == WASM_COMPONENT_WASI_INPUT_STREAM_OK && bytes_read > 0
        && bytes_read <= capacity) {
        ret->u.ok.buf = buffer;
        ret->u.ok.buf_len = bytes_read;
        return;
    }

    wasm_runtime_free(buffer);
    ret->is_err = true;
    if (status == WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED && bytes_read == 0) {
        set_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED, 0);
    }
    else if (status == WASM_COMPONENT_WASI_INPUT_STREAM_FAILED
             && bytes_read == 0) {
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EIO);
    }
    else {
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                         EOVERFLOW);
    }
}

void
wasi_p2_callback_input_stream_skip(HostResource *resource, uint64_t len,
                                   wasi_result_u64_stream_error_t *ret)
{
    WasiP2CallbackInputStream *stream = NULL;
    wasm_component_wasi_input_stream_status_t status;
    uint64_t max_bytes = 0;
    uint64_t bytes_skipped = 0;

    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));
    if (!wasi_p2_is_callback_input_stream(resource)) {
        ret->is_err = true;
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (len == 0) {
        return;
    }

    stream = resource->data;
    max_bytes = len < WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES
                    ? len
                    : WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES;
    status =
        stream->callbacks.skip(stream->attachment, max_bytes, &bytes_skipped);
    if (status == WASM_COMPONENT_WASI_INPUT_STREAM_OK && bytes_skipped > 0
        && bytes_skipped <= max_bytes) {
        ret->u.ok = bytes_skipped;
        return;
    }

    ret->is_err = true;
    if (status == WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED
        && bytes_skipped == 0) {
        set_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED, 0);
    }
    else if (status == WASM_COMPONENT_WASI_INPUT_STREAM_FAILED
             && bytes_skipped == 0) {
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EIO);
    }
    else {
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                         EOVERFLOW);
    }
}

void
wasi_p2_callback_input_stream_splice(HostResource *resource,
                                     wasi_output_stream_t output_stream,
                                     uint64_t len, bool blocking,
                                     wasi_result_u64_stream_error_t *ret)
{
    uint64_t total_spliced = 0;

    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));
    if (!wasi_p2_is_callback_input_stream(resource)) {
        ret->is_err = true;
        set_stream_error(&ret->u.err,
                         WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }

    while (total_spliced < len) {
        wasi_result_u64_stream_error_t check_result = { 0 };
        wasi_result_list_u8_stream_error_t read_result = { 0 };
        wasi_result_void_stream_error_t write_result = { 0 };
        uint64_t remaining = len - total_spliced;
        uint64_t read_len = remaining;

        if (blocking) {
            wasi_pollable_context_t output_pollable = {
                .fd = output_stream,
                .own_fd = false,
                .type = WASI_POLLABLE_OUT,
                .host_context = NULL,
            };
            do {
                wasi_pollable_block(&output_pollable);
                wasi_output_stream_check_write(output_stream, &check_result);
                if (check_result.is_err) {
                    *ret = check_result;
                    return;
                }
            } while (check_result.u.ok == 0);
        }
        else {
            wasi_output_stream_check_write(output_stream, &check_result);
            if (check_result.is_err) {
                *ret = check_result;
                return;
            }
            if (check_result.u.ok == 0) {
                break;
            }
        }

        if (read_len > check_result.u.ok) {
            read_len = check_result.u.ok;
        }
        wasi_p2_callback_input_stream_read(resource, read_len, &read_result);
        if (read_result.is_err) {
            if (read_result.u.err.kind == WASI_STREAM_ERROR_KIND_CLOSED
                && total_spliced != 0) {
                break;
            }
            ret->is_err = true;
            ret->u.err = read_result.u.err;
            return;
        }

        wasi_output_stream_write(output_stream, &read_result.u.ok,
                                 &write_result);
        if (write_result.is_err) {
            wasm_runtime_free(read_result.u.ok.buf);
            ret->is_err = true;
            ret->u.err = write_result.u.err;
            return;
        }
        total_spliced += read_result.u.ok.buf_len;
        wasm_runtime_free(read_result.u.ok.buf);

        if (!blocking) {
            break;
        }
    }

    ret->u.ok = total_spliced;
}

static bool
set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int fd_flags = 0;

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    fd_flags = fcntl(fd, F_GETFD, 0);
    return fd_flags >= 0 && fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0;
}

static wasm_component_wasi_pollable_signal_t
pollable_signal_create(void)
{
    wasm_component_wasi_pollable_signal_t signal = NULL;
    int fds[2] = { -1, -1 };
    bool lock_initialized = false;

    signal = wasm_runtime_malloc(sizeof(*signal));
    if (!signal) {
        return NULL;
    }
    memset(signal, 0, sizeof(*signal));
    signal->read_fd = -1;
    signal->write_fd = -1;
    if (os_mutex_init(&signal->lock) == BHT_OK) {
        lock_initialized = true;
    }
    if (!lock_initialized || pipe(fds) != 0 || !set_nonblocking_cloexec(fds[0])
        || !set_nonblocking_cloexec(fds[1])) {
        if (fds[0] >= 0) {
            close(fds[0]);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
        }
        if (lock_initialized) {
            os_mutex_destroy(&signal->lock);
        }
        wasm_runtime_free(signal);
        return NULL;
    }
    signal->references = 1;
    signal->active = true;
    signal->read_fd = fds[0];
    signal->write_fd = fds[1];
    return signal;
}

static void
pollable_signal_retain(wasm_component_wasi_pollable_signal_t signal)
{
    os_mutex_lock(&signal->lock);
    signal->references++;
    os_mutex_unlock(&signal->lock);
}

void
wasm_component_wasi_pollable_signal_release(
    wasm_component_wasi_pollable_signal_t signal)
{
    bool destroy = false;

    if (!signal) {
        return;
    }
    os_mutex_lock(&signal->lock);
    if (signal->references > 0) {
        signal->references--;
    }
    destroy = signal->references == 0;
    os_mutex_unlock(&signal->lock);
    if (destroy) {
        os_mutex_destroy(&signal->lock);
        wasm_runtime_free(signal);
    }
}

bool
wasm_component_wasi_pollable_notify(
    wasm_component_wasi_pollable_signal_t signal)
{
    uint8_t byte = 1;
    ssize_t written = -1;
    int saved_errno = 0;
    bool active = false;

    if (!signal) {
        return false;
    }
    os_mutex_lock(&signal->lock);
    active = signal->active && signal->write_fd >= 0;
    if (active) {
        do {
            written = write(signal->write_fd, &byte, sizeof(byte));
        } while (written < 0 && errno == EINTR);
        saved_errno = errno;
    }
    os_mutex_unlock(&signal->lock);
    return active
           && (written == (ssize_t)sizeof(byte)
               || (written < 0
                   && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)));
}

static void
pollable_signal_deactivate(wasm_component_wasi_pollable_signal_t signal)
{
    if (!signal) {
        return;
    }
    os_mutex_lock(&signal->lock);
    signal->active = false;
    if (signal->read_fd >= 0) {
        close(signal->read_fd);
        signal->read_fd = -1;
    }
    if (signal->write_fd >= 0) {
        close(signal->write_fd);
        signal->write_fd = -1;
    }
    os_mutex_unlock(&signal->lock);
}

static bool
pollable_callbacks_valid(
    const wasm_component_wasi_pollable_callbacks_t *callbacks)
{
    return callbacks
           && callbacks->struct_size
                  == sizeof(wasm_component_wasi_pollable_callbacks_t)
           && callbacks->ready && callbacks->drop;
}

static void
pollable_drop(void *data)
{
    WasiP2CallbackPollable *pollable = data;

    if (!pollable || pollable->dropped) {
        return;
    }
    pollable->dropped = true;
    pollable_signal_deactivate(pollable->signal);
    pollable->callbacks.drop(pollable->attachment);
    wasm_component_wasi_pollable_signal_release(pollable->signal);
    pollable->signal = NULL;
}

bool
wasm_component_wasi_pollable_new(
    wasm_exec_env_t exec_env,
    const wasm_component_wasi_pollable_callbacks_t *callbacks, void *attachment,
    wasm_component_wasi_pollable_signal_t *out_signal, uint32_t *out_handle)
{
    HostResourceTable *table = NULL;
    HostResource *resource = NULL;
    WasiP2CallbackPollable *pollable = NULL;
    wasm_component_wasi_pollable_signal_t signal = NULL;
    uint32_t representation = 0;

    if (out_signal) {
        *out_signal = NULL;
    }
    if (out_handle) {
        *out_handle = 0;
    }
    if (!pollable_callbacks_valid(callbacks)) {
        return false;
    }
    if (!out_signal || !out_handle
        || !wasm_component_exec_env_is_callback(exec_env)
        || !(table = get_global_host_resource_table())) {
        callbacks->drop(attachment);
        return false;
    }

    signal = pollable_signal_create();
    if (!signal) {
        callbacks->drop(attachment);
        return false;
    }
    resource =
        host_resource_create(WASI_P2_POLLABLE, sizeof(WasiP2CallbackPollable));
    if (!resource) {
        pollable_signal_deactivate(signal);
        wasm_component_wasi_pollable_signal_release(signal);
        callbacks->drop(attachment);
        return false;
    }
    pollable = resource->data;
    memset(pollable, 0, sizeof(*pollable));
    pollable->callbacks = *callbacks;
    pollable->attachment = attachment;
    pollable->signal = signal;
    SET_HOST_CALLBACK_POLLABLE(&pollable->pollable, signal->read_fd, pollable);
    host_resource_set_dtor(resource, pollable_drop);

    representation = host_resource_table_add(table, resource);
    if (representation == 0) {
        destroy_host_resource(resource);
        return false;
    }
    if (!wasm_component_host_resource_new(exec_env, WASI_IO_POLL_INTERFACE,
                                          "pollable", representation,
                                          out_handle)) {
        host_resource_table_delete(table, representation);
        return false;
    }

    pollable_signal_retain(signal);
    *out_signal = signal;
    return true;
}

bool
wasi_p2_host_pollable_ready(void *host_context)
{
    WasiP2CallbackPollable *pollable = host_context;

    return pollable && !pollable->dropped
           && pollable->callbacks.ready(pollable->attachment);
}

void
wasi_p2_host_pollable_drain(void *host_context)
{
    WasiP2CallbackPollable *pollable = host_context;
    wasm_component_wasi_pollable_signal_t signal = NULL;
    uint8_t bytes[64];
    ssize_t count = 0;

    if (!pollable || pollable->dropped || !(signal = pollable->signal)) {
        return;
    }
    os_mutex_lock(&signal->lock);
    if (signal->active && signal->read_fd >= 0) {
        do {
            count = read(signal->read_fd, bytes, sizeof(bytes));
        } while (count > 0 || (count < 0 && errno == EINTR));
    }
    os_mutex_unlock(&signal->lock);
}
