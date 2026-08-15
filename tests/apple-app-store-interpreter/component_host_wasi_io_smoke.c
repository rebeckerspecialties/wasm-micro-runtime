/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wasm_export.h"

enum {
    ERROR_BUFFER_SIZE = 512,
    CUSTOM_DATA_COOKIE = 0xC0DEC0DE,
    FIRST_FILE_REPRESENTATION = 101,
    FIRST_SCANNER_REPRESENTATION = 201,
};

typedef enum StreamMode {
    STREAM_MODE_NORMAL,
    STREAM_MODE_OVERREPORT,
} StreamMode;

typedef struct HostIoState HostIoState;

typedef struct StreamAttachment {
    HostIoState *state;
    StreamMode mode;
    uint32_t cursor;
    bool dropped;
} StreamAttachment;

typedef struct PollAttachment {
    HostIoState *state;
    bool dropped;
} PollAttachment;

struct HostIoState {
    uint32_t cookie;
    uint32_t next_file_representation;
    uint32_t next_scanner_representation;
    StreamMode next_stream_mode;
    uint32_t file_drops;
    uint32_t scanner_drops;
    uint32_t stream_constructions;
    uint32_t stream_drops;
    uint32_t stream_reads;
    uint32_t stream_skips;
    uint32_t maximum_read_capacity;
    uint64_t maximum_skip_capacity;
    uint32_t poll_constructions;
    uint32_t poll_drops;
    uint32_t poll_ready_queries;
    uint32_t callback_failures;
    pthread_mutex_t signal_lock;
    pthread_cond_t signal_ready;
    wasm_component_wasi_pollable_signal_t signal;
    bool cancel_notifier;
    bool pause_pollable_factory;
    bool release_pollable_factory;
    uint32_t pause_ready_query_at;
    bool release_ready_query;
    uint32_t ready_query_epoch;
    atomic_bool poll_level_ready;
    bool notify_result;
};

static const uint8_t stream_payload[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static HostIoState *
get_host_state(wasm_exec_env_t exec_env)
{
    HostIoState *state = wasm_component_get_custom_data_from_exec_env(exec_env);

    return state && state->cookie == CUSTOM_DATA_COOKIE ? state : NULL;
}

static wasm_component_wasi_input_stream_status_t
stream_read(void *attachment, uint8_t *buffer, uint32_t capacity,
            uint32_t *out_bytes_read)
{
    StreamAttachment *stream = attachment;
    uint32_t remaining = 0;
    uint32_t count = 0;

    if (!stream || !stream->state || stream->dropped || !buffer
        || !out_bytes_read || capacity == 0) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    stream->state->stream_reads++;
    if (capacity > stream->state->maximum_read_capacity) {
        stream->state->maximum_read_capacity = capacity;
    }
    if (stream->mode == STREAM_MODE_OVERREPORT) {
        *out_bytes_read = capacity + 1;
        return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
    }
    remaining = (uint32_t)sizeof(stream_payload) - stream->cursor;
    if (remaining == 0) {
        *out_bytes_read = 0;
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    count = remaining < capacity ? remaining : capacity;
    if (count > 4) {
        count = 4;
    }
    memcpy(buffer, stream_payload + stream->cursor, count);
    stream->cursor += count;
    *out_bytes_read = count;
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

static wasm_component_wasi_input_stream_status_t
stream_skip(void *attachment, uint64_t max_bytes, uint64_t *out_bytes_skipped)
{
    StreamAttachment *stream = attachment;
    uint64_t remaining = 0;
    uint64_t count = 0;

    if (!stream || !stream->state || stream->dropped || !out_bytes_skipped
        || max_bytes == 0) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    stream->state->stream_skips++;
    if (max_bytes > stream->state->maximum_skip_capacity) {
        stream->state->maximum_skip_capacity = max_bytes;
    }
    if (stream->mode == STREAM_MODE_OVERREPORT) {
        *out_bytes_skipped = max_bytes + 1;
        return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
    }
    remaining = sizeof(stream_payload) - stream->cursor;
    if (remaining == 0) {
        *out_bytes_skipped = 0;
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }
    count = remaining < max_bytes ? remaining : max_bytes;
    if (count > 2) {
        count = 2;
    }
    stream->cursor += (uint32_t)count;
    *out_bytes_skipped = count;
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

static void
stream_drop(void *attachment)
{
    StreamAttachment *stream = attachment;

    if (!stream || !stream->state || stream->dropped) {
        abort();
    }
    stream->dropped = true;
    stream->state->stream_drops++;
    free(stream);
}

static bool
poll_ready(void *attachment)
{
    PollAttachment *pollable = attachment;
    bool ready = false;

    if (!pollable || !pollable->state || pollable->dropped) {
        abort();
    }
    pthread_mutex_lock(&pollable->state->signal_lock);
    pollable->state->poll_ready_queries++;
    pollable->state->ready_query_epoch++;
    pthread_cond_broadcast(&pollable->state->signal_ready);
    while (pollable->state->pause_ready_query_at
               == pollable->state->ready_query_epoch
           && !pollable->state->release_ready_query) {
        pthread_cond_wait(&pollable->state->signal_ready,
                          &pollable->state->signal_lock);
    }
    pthread_mutex_unlock(&pollable->state->signal_lock);
    ready = atomic_load_explicit(&pollable->state->poll_level_ready,
                                 memory_order_acquire);
    return ready;
}

static void
poll_drop(void *attachment)
{
    PollAttachment *pollable = attachment;

    if (!pollable || !pollable->state || pollable->dropped) {
        abort();
    }
    pollable->dropped = true;
    pollable->state->poll_drops++;
    free(pollable);
}

static void
make_file_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    HostIoState *state = get_host_state(exec_env);
    uint32_t handle = 0;

    if (!state
        || !wasm_component_host_resource_new(
            exec_env, "w3c:file-system-access/read-only-file-picker@0.0.2",
            "selected-file", state->next_file_representation++, &handle)) {
        if (state) {
            state->callback_failures++;
        }
        return;
    }
    canonical_cells[0] = handle;
}

static void
make_scanner_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    HostIoState *state = get_host_state(exec_env);
    uint32_t handle = 0;

    if (!state
        || !wasm_component_host_resource_new(
            exec_env, "snqr:shape-detection-camera/live-camera-detection@0.0.1",
            "camera-barcode-scanner", state->next_scanner_representation++,
            &handle)) {
        if (state) {
            state->callback_failures++;
        }
        return;
    }
    canonical_cells[0] = handle;
}

static void
contents_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    static const wasm_component_wasi_input_stream_callbacks_t callbacks = {
        .struct_size = sizeof(callbacks),
        .read = stream_read,
        .skip = stream_skip,
        .drop = stream_drop,
    };
    HostIoState *state = get_host_state(exec_env);
    StreamAttachment *stream = NULL;
    uint32_t representation = 0;
    uint32_t handle = 0;

    if (!state
        || !wasm_component_host_resource_rep(
            exec_env, "w3c:file-system-access/read-only-file-picker@0.0.2",
            "selected-file", (uint32_t)canonical_cells[0], &representation)
        || representation < FIRST_FILE_REPRESENTATION) {
        if (state) {
            state->callback_failures++;
        }
        return;
    }
    stream = calloc(1, sizeof(*stream));
    if (!stream) {
        state->callback_failures++;
        return;
    }
    stream->state = state;
    stream->mode = state->next_stream_mode;
    state->next_stream_mode = STREAM_MODE_NORMAL;
    state->stream_constructions++;
    if (!wasm_component_wasi_input_stream_new(exec_env, &callbacks, stream,
                                              &handle)) {
        state->callback_failures++;
        return;
    }
    canonical_cells[0] = handle;
}

static void
event_pollable_raw(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    static const wasm_component_wasi_pollable_callbacks_t callbacks = {
        .struct_size = sizeof(callbacks),
        .ready = poll_ready,
        .drop = poll_drop,
    };
    HostIoState *state = get_host_state(exec_env);
    PollAttachment *pollable = NULL;
    wasm_component_wasi_pollable_signal_t signal = NULL;
    uint32_t representation = 0;
    uint32_t handle = 0;

    if (!state
        || !wasm_component_host_resource_rep(
            exec_env, "snqr:shape-detection-camera/live-camera-detection@0.0.1",
            "camera-barcode-scanner", (uint32_t)canonical_cells[0],
            &representation)
        || representation < FIRST_SCANNER_REPRESENTATION) {
        if (state) {
            state->callback_failures++;
        }
        return;
    }
    pollable = calloc(1, sizeof(*pollable));
    if (!pollable) {
        state->callback_failures++;
        return;
    }
    pollable->state = state;
    state->poll_constructions++;
    if (!wasm_component_wasi_pollable_new(exec_env, &callbacks, pollable,
                                          &signal, &handle)) {
        state->callback_failures++;
        return;
    }

    pthread_mutex_lock(&state->signal_lock);
    if (state->signal != NULL) {
        state->callback_failures++;
    }
    else {
        state->signal = signal;
        pthread_cond_broadcast(&state->signal_ready);
        while (state->pause_pollable_factory
               && !state->release_pollable_factory) {
            pthread_cond_wait(&state->signal_ready, &state->signal_lock);
        }
    }
    pthread_mutex_unlock(&state->signal_lock);
    canonical_cells[0] = handle;
}

static bool
drop_file(void *attachment, uint32_t representation)
{
    HostIoState *state = attachment;

    if (!state || state->cookie != CUSTOM_DATA_COOKIE
        || representation < FIRST_FILE_REPRESENTATION) {
        return false;
    }
    state->file_drops++;
    return true;
}

static bool
drop_scanner(void *attachment, uint32_t representation)
{
    HostIoState *state = attachment;

    if (!state || state->cookie != CUSTOM_DATA_COOKIE
        || representation < FIRST_SCANNER_REPRESENTATION) {
        return false;
    }
    state->scanner_drops++;
    return true;
}

static wasm_component_wasi_input_stream_status_t
failure_read(void *attachment, uint8_t *buffer, uint32_t capacity,
             uint32_t *out_bytes_read)
{
    (void)attachment;
    (void)buffer;
    (void)capacity;
    *out_bytes_read = 0;
    return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
}

static wasm_component_wasi_input_stream_status_t
failure_skip(void *attachment, uint64_t max_bytes, uint64_t *out_bytes_skipped)
{
    (void)attachment;
    (void)max_bytes;
    *out_bytes_skipped = 0;
    return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
}

static void
failure_drop(void *attachment)
{
    uint32_t *drops = attachment;

    (*drops)++;
}

static bool
failure_ready(void *attachment)
{
    (void)attachment;
    return false;
}

static bool
test_constructor_failures(void)
{
    wasm_component_wasi_input_stream_callbacks_t stream_callbacks = {
        .struct_size = sizeof(stream_callbacks),
        .read = failure_read,
        .skip = failure_skip,
        .drop = failure_drop,
    };
    wasm_component_wasi_pollable_callbacks_t poll_callbacks = {
        .struct_size = sizeof(poll_callbacks),
        .ready = failure_ready,
        .drop = failure_drop,
    };
    wasm_component_wasi_pollable_signal_t signal =
        (wasm_component_wasi_pollable_signal_t)(uintptr_t)1;
    uint32_t stream_drops = 0;
    uint32_t poll_drops = 0;
    uint32_t invalid_drops = 0;
    uint32_t handle = UINT32_MAX;

    if (WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES != 64U * 1024U
        || wasm_component_wasi_input_stream_new(NULL, &stream_callbacks,
                                                &stream_drops, &handle)
        || handle != 0 || stream_drops != 1) {
        return false;
    }
    handle = UINT32_MAX;
    if (wasm_component_wasi_pollable_new(NULL, &poll_callbacks, &poll_drops,
                                         &signal, &handle)
        || signal != NULL || handle != 0 || poll_drops != 1) {
        return false;
    }

    stream_callbacks.read = NULL;
    handle = UINT32_MAX;
    if (wasm_component_wasi_input_stream_new(NULL, &stream_callbacks,
                                             &invalid_drops, &handle)
        || handle != 0 || invalid_drops != 0) {
        return false;
    }
    poll_callbacks.ready = NULL;
    signal = (wasm_component_wasi_pollable_signal_t)(uintptr_t)1;
    handle = UINT32_MAX;
    return !wasm_component_wasi_pollable_new(NULL, &poll_callbacks,
                                             &invalid_drops, &signal, &handle)
           && signal == NULL && handle == 0 && invalid_drops == 0;
}

static uint8_t *
read_file(const char *path, uint32_t *size_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long size = 0;

    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0
        || size > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (uint32_t)size;
    return bytes;
}

static bool
call_export(WASMComponentInstance *instance, const char *name,
            uint32_t result_count, wasm_val_t *result, char *error,
            uint32_t error_size)
{
    WASMComponentPreparedCall *call =
        wasm_component_prepare_export_call(instance, name, error, error_size);
    bool success =
        call
        && wasm_component_call_prepared(call, result_count, result, 0, NULL)
        && wasm_component_prepared_call_post_return(call);

    wasm_component_destroy_prepared_call(call);
    return success;
}

static void *
notify_pollable(void *attachment)
{
    HostIoState *state = attachment;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    wasm_component_wasi_pollable_signal_t signal = NULL;

    pthread_mutex_lock(&state->signal_lock);
    while (state->signal == NULL && !state->cancel_notifier) {
        pthread_cond_wait(&state->signal_ready, &state->signal_lock);
    }
    signal = state->signal;
    pthread_mutex_unlock(&state->signal_lock);

    if (!signal) {
        return NULL;
    }

    nanosleep(&delay, NULL);
    atomic_store_explicit(&state->poll_level_ready, true, memory_order_release);
    state->notify_result = wasm_component_wasi_pollable_notify(signal);
    return NULL;
}

typedef struct AsyncComponentCall {
    WASMComponentInstance *instance;
    const char *export_name;
    bool succeeded;
    atomic_bool completed;
    char error[ERROR_BUFFER_SIZE];
} AsyncComponentCall;

typedef enum TerminationCaseMode {
    TERMINATE_BEFORE_ARM,
    TERMINATE_DURING_READY_CHECK,
    TERMINATE_WHILE_WAITING,
} TerminationCaseMode;

static bool
host_state_init(HostIoState *state)
{
    memset(state, 0, sizeof(*state));
    state->cookie = CUSTOM_DATA_COOKIE;
    state->next_file_representation = FIRST_FILE_REPRESENTATION;
    state->next_scanner_representation = FIRST_SCANNER_REPRESENTATION;
    if (pthread_mutex_init(&state->signal_lock, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&state->signal_ready, NULL) != 0) {
        pthread_mutex_destroy(&state->signal_lock);
        return false;
    }
    atomic_init(&state->poll_level_ready, false);
    return true;
}

static void
host_state_destroy(HostIoState *state)
{
    pthread_cond_destroy(&state->signal_ready);
    pthread_mutex_destroy(&state->signal_lock);
}

static WASMComponentInstance *
instantiate_component(WASMComponent *component, HostIoState *state, char *error,
                      uint32_t error_size)
{
    struct InstantiationArgs2 *args = NULL;
    WASMComponentInstance *instance = NULL;

    if (!wasm_runtime_instantiation_args_create(&args)) {
        return NULL;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(args, 256 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 0);
    wasm_runtime_instantiation_args_set_custom_data(args, state);
    instance =
        wasm_component_instantiate_ex2(component, args, error, error_size);
    wasm_runtime_instantiation_args_destroy(args);
    return instance;
}

static void *
call_export_async(void *attachment)
{
    AsyncComponentCall *call = attachment;
    wasm_val_t result = { 0 };

    call->succeeded = call_export(call->instance, call->export_name, 1, &result,
                                  call->error, (uint32_t)sizeof(call->error));
    atomic_store_explicit(&call->completed, true, memory_order_release);
    return NULL;
}

static bool
wait_for_host_progress(HostIoState *state, uint32_t ready_query_epoch)
{
    struct timespec deadline;
    int wait_result = 0;
    bool reached = false;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 2;

    pthread_mutex_lock(&state->signal_lock);
    while (
        (state->signal == NULL || state->ready_query_epoch < ready_query_epoch)
        && wait_result == 0) {
        wait_result = pthread_cond_timedwait(&state->signal_ready,
                                             &state->signal_lock, &deadline);
    }
    reached =
        state->signal != NULL && state->ready_query_epoch >= ready_query_epoch;
    pthread_mutex_unlock(&state->signal_lock);
    return reached;
}

static bool
join_completed_call(pthread_t thread, AsyncComponentCall *call)
{
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };

    for (uint32_t i = 0; i < 2000; i++) {
        if (atomic_load_explicit(&call->completed, memory_order_acquire)) {
            pthread_join(thread, NULL);
            return true;
        }
        nanosleep(&delay, NULL);
    }
    return false;
}

static bool
run_termination_case(WASMComponent *component, const char *export_name,
                     TerminationCaseMode mode, bool repeat_termination)
{
    HostIoState state;
    AsyncComponentCall call = { 0 };
    WASMComponentInstance *instance = NULL;
    wasm_component_wasi_pollable_signal_t signal = NULL;
    pthread_t thread;
    struct timespec wait_delay = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    const char *exception = NULL;
    char error[ERROR_BUFFER_SIZE] = { 0 };
    bool passed = false;

    if (!host_state_init(&state)) {
        return false;
    }
    state.pause_pollable_factory = mode == TERMINATE_BEFORE_ARM;
    state.pause_ready_query_at = mode == TERMINATE_DURING_READY_CHECK ? 2 : 0;

    instance = instantiate_component(component, &state, error, sizeof(error));
    if (!instance) {
        fprintf(stderr, "termination fixture instantiation failed: %s\n",
                error);
        goto cleanup;
    }

    call.instance = instance;
    call.export_name = export_name;
    atomic_init(&call.completed, false);
    if (pthread_create(&thread, NULL, call_export_async, &call) != 0) {
        fprintf(stderr, "failed to start termination fixture call\n");
        goto cleanup;
    }
    if (!wait_for_host_progress(&state, mode == TERMINATE_BEFORE_ARM ? 0 : 2)) {
        fprintf(stderr, "termination fixture did not reach wait setup\n");
        goto terminate_and_join;
    }
    if (mode == TERMINATE_WHILE_WAITING) {
        nanosleep(&wait_delay, NULL);
    }

    wasm_component_terminate(instance);
    if (repeat_termination) {
        wasm_component_terminate(instance);
    }

    pthread_mutex_lock(&state.signal_lock);
    state.release_pollable_factory = true;
    state.release_ready_query = true;
    pthread_cond_broadcast(&state.signal_ready);
    pthread_mutex_unlock(&state.signal_lock);

    if (!join_completed_call(thread, &call)) {
        fprintf(stderr, "terminated component call did not return\n");
        abort();
    }
    exception = wasm_component_runtime_get_exception(instance);
    if (call.succeeded || !exception
        || strstr(exception, "terminated by user") == NULL
        || state.callback_failures != 0 || state.poll_constructions != 1) {
        fprintf(stderr, "termination fixture returned wrong outcome: %s\n",
                exception ? exception : call.error);
        goto cleanup;
    }

    pthread_mutex_lock(&state.signal_lock);
    signal = state.signal;
    state.signal = NULL;
    pthread_mutex_unlock(&state.signal_lock);
    if (!signal || !wasm_component_wasi_pollable_notify(signal)) {
        fprintf(stderr, "late termination notify was not lifetime-safe\n");
        goto cleanup;
    }
    wasm_component_deinstantiate(instance);
    instance = NULL;
    if (state.poll_drops != 1 || state.scanner_drops != 1
        || wasm_component_wasi_pollable_notify(signal)) {
        fprintf(stderr, "termination fixture signal/drop lifetime failed\n");
        goto cleanup;
    }
    wasm_component_wasi_pollable_signal_release(signal);
    signal = NULL;
    passed = true;
    goto cleanup;

terminate_and_join:
    wasm_component_terminate(instance);
    pthread_mutex_lock(&state.signal_lock);
    state.release_pollable_factory = true;
    state.release_ready_query = true;
    pthread_cond_broadcast(&state.signal_ready);
    pthread_mutex_unlock(&state.signal_lock);
    if (!join_completed_call(thread, &call)) {
        abort();
    }

cleanup:
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    if (!signal) {
        pthread_mutex_lock(&state.signal_lock);
        signal = state.signal;
        state.signal = NULL;
        pthread_mutex_unlock(&state.signal_lock);
    }
    if (signal) {
        wasm_component_wasi_pollable_signal_release(signal);
    }
    host_state_destroy(&state);
    return passed;
}

int
main(int argc, char **argv)
{
    static NativeSymbol factory_symbols[] = {
        { "make-file", (void *)make_file_raw, "()i", NULL },
        { "make-scanner", (void *)make_scanner_raw, "()i", NULL },
    };
    static NativeSymbol file_symbols[] = {
        { "[method]selected-file.contents", (void *)contents_raw, "(i)i",
          NULL },
    };
    static NativeSymbol camera_symbols[] = {
        { "[method]camera-barcode-scanner.event-pollable",
          (void *)event_pollable_raw, "(i)i", NULL },
    };
    RuntimeInitArgs init_args = { 0 };
    LoadArgs load_args = { 0 };
    struct InstantiationArgs2 *instantiation_args = NULL;
    WASMComponent *component = NULL;
    WASMComponentInstance *instance = NULL;
    HostIoState state = {
        .cookie = CUSTOM_DATA_COOKIE,
        .next_file_representation = FIRST_FILE_REPRESENTATION,
        .next_scanner_representation = FIRST_SCANNER_REPRESENTATION,
    };
    pthread_t notifier;
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    wasm_val_t result = { 0 };
    char error[ERROR_BUFFER_SIZE] = { 0 };
    int exit_code = 1;
    bool notifier_started = false;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <component-host-wasi-io.wasm>\n", argv[0]);
        return 2;
    }
    bytes = read_file(argv[1], &size);
    if (!bytes) {
        fprintf(stderr, "failed to read component fixture: %s\n", argv[1]);
        return 1;
    }
    if (pthread_mutex_init(&state.signal_lock, NULL) != 0
        || pthread_cond_init(&state.signal_ready, NULL) != 0) {
        fprintf(stderr, "failed to initialize poll synchronization\n");
        goto cleanup;
    }
    atomic_init(&state.poll_level_ready, false);

    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args) || !test_constructor_failures()) {
        fprintf(stderr, "runtime initialization or constructor test failed\n");
        goto destroy_runtime;
    }
#define REGISTER_RAW(interface_name, symbols) \
    wasm_runtime_register_natives_raw(        \
        interface_name, symbols,              \
        (uint32_t)(sizeof(symbols) / sizeof((symbols)[0])))
    if (!REGISTER_RAW("test:project/component-host-io@0.1.0", factory_symbols)
        || !REGISTER_RAW("w3c:file-system-access/read-only-file-picker@0.0.2",
                         file_symbols)
        || !REGISTER_RAW(
            "snqr:shape-detection-camera/live-camera-detection@0.0.1",
            camera_symbols)) {
        fprintf(stderr, "component host I/O registration failed\n");
        goto destroy_runtime;
    }
#undef REGISTER_RAW

    load_args.name = "component-host-wasi-io";
    load_args.is_component = true;
    component =
        wasm_component_load(bytes, size, &load_args, error, sizeof(error));
    if (!component) {
        fprintf(stderr, "component load failed: %s\n", error);
        goto destroy_runtime;
    }
    if (!wasm_component_register_host_resource_drop_callback(
            component, "w3c:file-system-access/read-only-file-picker@0.0.2",
            "selected-file", drop_file)
        || !wasm_component_register_host_resource_drop_callback(
            component,
            "snqr:shape-detection-camera/live-camera-detection@0.0.1",
            "camera-barcode-scanner", drop_scanner)
        || !wasm_runtime_instantiation_args_create(&instantiation_args)) {
        fprintf(stderr, "component resource registration failed\n");
        goto unload_component;
    }
    wasm_runtime_instantiation_args_set_default_stack_size(instantiation_args,
                                                           256 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        instantiation_args, 0);
    wasm_runtime_instantiation_args_set_custom_data(instantiation_args, &state);
    instance = wasm_component_instantiate_ex2(component, instantiation_args,
                                              error, sizeof(error));
    wasm_runtime_instantiation_args_destroy(instantiation_args);
    instantiation_args = NULL;
    if (!instance) {
        fprintf(stderr, "component instantiation failed: %s\n", error);
        goto unload_component;
    }

    if (!call_export(instance, "stream-explicit", 1, &result, error,
                     sizeof(error))
        || result.kind != WASM_I32 || result.of.i32 != 1
        || state.stream_constructions != 1 || state.stream_drops != 1
        || state.stream_reads != 2 || state.stream_skips != 2
        || state.maximum_read_capacity
               != WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES
        || state.maximum_skip_capacity != 2) {
        fprintf(stderr, "explicit stream exercise failed: %s\n", error);
        goto deinstantiate;
    }

    state.next_stream_mode = STREAM_MODE_OVERREPORT;
    if (!call_export(instance, "stream-overreport", 0, NULL, error,
                     sizeof(error))
        || state.stream_constructions != 2 || state.stream_drops != 2
        || state.stream_reads != 3 || state.stream_skips != 3) {
        fprintf(stderr, "stream over-report exercise failed: %s\n", error);
        goto deinstantiate;
    }

    if (!call_export(instance, "stream-splice", 0, NULL, error, sizeof(error))
        || state.stream_constructions != 3 || state.stream_drops != 3
        || state.stream_reads != 5) {
        fprintf(stderr, "stream splice exercise failed: %s\n", error);
        goto deinstantiate;
    }

    result = (wasm_val_t){ 0 };
    if (!call_export(instance, "poll-always-ready", 1, &result, error,
                     sizeof(error))
        || result.kind != WASM_I32 || result.of.i32 != 1
        || state.stream_constructions != 4 || state.stream_drops != 4) {
        fprintf(stderr, "always-ready mixed poll exercise failed: %s\n", error);
        goto deinstantiate;
    }

    if (pthread_create(&notifier, NULL, notify_pollable, &state) != 0) {
        fprintf(stderr, "failed to start poll notifier\n");
        goto deinstantiate;
    }
    notifier_started = true;
    result = (wasm_val_t){ 0 };
    if (!call_export(instance, "poll-wait", 1, &result, error, sizeof(error))
        || result.kind != WASM_I32 || result.of.i32 != 1) {
        fprintf(stderr, "externally-woken mixed poll failed: %s\n", error);
        goto deinstantiate;
    }
    pthread_join(notifier, NULL);
    notifier_started = false;
    if (!state.notify_result || state.poll_constructions != 1
        || state.poll_drops != 1 || state.poll_ready_queries == 0
        || wasm_component_wasi_pollable_notify(state.signal)) {
        fprintf(stderr, "poll signal lifetime exercise failed\n");
        goto deinstantiate;
    }
    wasm_component_wasi_pollable_signal_release(state.signal);
    state.signal = NULL;

    atomic_store_explicit(&state.poll_level_ready, false, memory_order_release);
    state.notify_result = false;
    if (pthread_create(&notifier, NULL, notify_pollable, &state) != 0) {
        fprintf(stderr, "failed to start blocking poll notifier\n");
        goto deinstantiate;
    }
    notifier_started = true;
    result = (wasm_val_t){ 0 };
    if (!call_export(instance, "poll-block-wait", 1, &result, error,
                     sizeof(error))
        || result.kind != WASM_I32 || result.of.i32 != 1) {
        fprintf(stderr, "externally-woken pollable.block failed: %s\n", error);
        goto deinstantiate;
    }
    pthread_join(notifier, NULL);
    notifier_started = false;
    if (!state.notify_result || state.poll_constructions != 2
        || state.poll_drops != 2
        || wasm_component_wasi_pollable_notify(state.signal)) {
        fprintf(stderr, "blocking poll signal lifetime exercise failed\n");
        goto deinstantiate;
    }
    wasm_component_wasi_pollable_signal_release(state.signal);
    state.signal = NULL;

    if (!call_export(instance, "stream-implicit", 0, NULL, error, sizeof(error))
        || !call_export(instance, "poll-implicit", 0, NULL, error,
                        sizeof(error))
        || state.stream_constructions != 5 || state.stream_drops != 4
        || state.poll_constructions != 3 || state.poll_drops != 2) {
        fprintf(stderr, "implicit teardown setup failed: %s\n", error);
        goto deinstantiate;
    }
    pthread_mutex_lock(&state.signal_lock);
    wasm_component_wasi_pollable_signal_release(state.signal);
    state.signal = NULL;
    pthread_mutex_unlock(&state.signal_lock);

    if (state.callback_failures != 0 || state.file_drops != 5
        || state.scanner_drops != 3) {
        fprintf(stderr, "component resource accounting failed\n");
        goto deinstantiate;
    }
    exit_code = 0;

deinstantiate:
    if (notifier_started) {
        wasm_component_terminate(instance);
        pthread_mutex_lock(&state.signal_lock);
        state.cancel_notifier = true;
        pthread_cond_broadcast(&state.signal_ready);
        pthread_mutex_unlock(&state.signal_lock);
        pthread_join(notifier, NULL);
    }
    wasm_component_deinstantiate(instance);
    instance = NULL;
    if (exit_code == 0
        && (state.stream_drops != state.stream_constructions
            || state.poll_drops != state.poll_constructions)) {
        fprintf(stderr,
                "component teardown missed resource drops: streams=%u/%u "
                "pollables=%u/%u\n",
                state.stream_drops, state.stream_constructions,
                state.poll_drops, state.poll_constructions);
        exit_code = 1;
    }
    if (exit_code == 0
        && (!run_termination_case(component, "poll-block-wait",
                                  TERMINATE_BEFORE_ARM, false)
            || !run_termination_case(component, "poll-block-wait",
                                     TERMINATE_DURING_READY_CHECK, false)
            || !run_termination_case(component, "poll-block-wait",
                                     TERMINATE_WHILE_WAITING, true)
            || !run_termination_case(component, "poll-wait",
                                     TERMINATE_WHILE_WAITING, true))) {
        fprintf(stderr, "component no-notifier termination exercise failed\n");
        exit_code = 1;
    }
unload_component:
    if (instantiation_args) {
        wasm_runtime_instantiation_args_destroy(instantiation_args);
    }
    wasm_component_unload(component);
destroy_runtime:
    wasm_runtime_destroy();
    pthread_cond_destroy(&state.signal_ready);
    pthread_mutex_destroy(&state.signal_lock);
cleanup:
    free(bytes);
    if (exit_code == 0) {
        puts("component callback-backed WASI I/O smoke passed");
    }
    return exit_code;
}
