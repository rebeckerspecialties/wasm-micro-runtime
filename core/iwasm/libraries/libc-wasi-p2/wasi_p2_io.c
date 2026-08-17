/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* splice(2) / SPLICE_F_NONBLOCK used below are GNU extensions; request them
   before any system header is pulled in. Defining it in the TU keeps the file
   building where the compile flags don't set it (e.g. CodeQL's autobuild). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "wasi_p2_io.h"
#include "wasi_p2_error.h"
#include "wasi_p2_host_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/uio.h>

#include "wasi_p2_common.h"

#include <sys/stat.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <fcntl.h>
#endif
#include <sys/socket.h>
#include <pthread.h>

void
pollable_dtor(void *data)
{
    wasi_pollable_context_t *pollable = (wasi_pollable_context_t *)data;
    if (pollable->own_fd) {
        close(pollable->fd);
    }
}

/**
 * @brief A node in a linked list used to track output streams that are
 *        currently undergoing a `flush` operation.
 */
typedef struct FlushingStreamNode {
    wasi_output_stream_t stream;
    struct FlushingStreamNode *next;
} FlushingStreamNode;

typedef struct FlushingStreamReservation {
    FlushingStreamNode *node;
    bool regular_file;
    bool track_output_queue;
} FlushingStreamReservation;

/**
 * @brief The head of the global linked list of flushing streams. Access to this
 *        list must be protected by the `flushing_streams_list_lock`.
 */
static FlushingStreamNode *flushing_streams_list = NULL;

/**
 * @brief A mutex to ensure thread-safe access to the `flushing_streams_list`.
 */
static pthread_mutex_t flushing_streams_list_lock = PTHREAD_MUTEX_INITIALIZER;

/* A duplicated descriptor shares its open-file description. Keep every
 * guest-visible positional file mutation under one lock so logical stream
 * cursors and append-at-EOF selection remain race-free without changing the
 * descriptor resource's kernel cursor or status flags. */
static pthread_mutex_t file_io_lock = PTHREAD_MUTEX_INITIALIZER;

void
wasi_p2_file_io_lock(void)
{
    pthread_mutex_lock(&file_io_lock);
}

void
wasi_p2_file_io_unlock(void)
{
    pthread_mutex_unlock(&file_io_lock);
}

#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
struct WasiP2WaitInterrupt {
    int read_fd;
    int write_fd;
    bool interrupted;
    WasiP2NativeFdQuotaLease fd_lease;
};

static bool
wait_interrupt_set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int fd_flags = 0;

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    fd_flags = fcntl(fd, F_GETFD, 0);
    return fd_flags >= 0 && fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0;
}

static WasiP2WaitInterrupt *
wait_interrupt_get(wasm_exec_env_t exec_env)
{
    WasiP2WaitInterrupt *interrupt = NULL;
    int fds[2] = { -1, -1 };

    if (!exec_env) {
        return NULL;
    }

    os_mutex_lock(&exec_env->wait_lock);
    interrupt = exec_env->wasi_p2_wait_interrupt;
    if (interrupt) {
        os_mutex_unlock(&exec_env->wait_lock);
        return interrupt;
    }

    interrupt = wasm_runtime_malloc(sizeof(*interrupt));
    if (!interrupt) {
        os_mutex_unlock(&exec_env->wait_lock);
        return NULL;
    }
    memset(interrupt, 0, sizeof(*interrupt));
    interrupt->read_fd = -1;
    interrupt->write_fd = -1;
    if (!wasi_p2_native_fd_quota_reserve(exec_env, 2, &interrupt->fd_lease)) {
        wasm_runtime_free(interrupt);
        os_mutex_unlock(&exec_env->wait_lock);
        return NULL;
    }
    if (pipe(fds) != 0 || !wait_interrupt_set_nonblocking_cloexec(fds[0])
        || !wait_interrupt_set_nonblocking_cloexec(fds[1])) {
        if (fds[0] >= 0) {
            close(fds[0]);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
        }
        wasi_p2_native_fd_quota_release(&interrupt->fd_lease);
        wasm_runtime_free(interrupt);
        os_mutex_unlock(&exec_env->wait_lock);
        return NULL;
    }

    interrupt->read_fd = fds[0];
    interrupt->write_fd = fds[1];
    interrupt->interrupted = (WASM_SUSPEND_FLAGS_GET(exec_env->suspend_flags)
                              & WASM_SUSPEND_FLAG_TERMINATE)
                             != 0;
    exec_env->wasi_p2_wait_interrupt = interrupt;
    os_mutex_unlock(&exec_env->wait_lock);
    return interrupt;
}

static bool
wait_interrupt_is_requested(wasm_exec_env_t exec_env,
                            const WasiP2WaitInterrupt *interrupt)
{
    bool requested = false;

    os_mutex_lock(&exec_env->wait_lock);
    requested = exec_env->wasi_p2_wait_interrupt == interrupt
                && (interrupt->interrupted
                    || (WASM_SUSPEND_FLAGS_GET(exec_env->suspend_flags)
                        & WASM_SUSPEND_FLAG_TERMINATE)
                           != 0);
    os_mutex_unlock(&exec_env->wait_lock);
    return requested;
}

static bool
wait_interrupt_exec_env_terminated(wasm_exec_env_t exec_env)
{
    return (WASM_SUSPEND_FLAGS_GET(exec_env->suspend_flags)
            & WASM_SUSPEND_FLAG_TERMINATE)
           != 0;
}

void
wasi_p2_interrupt_wait_request(wasm_exec_env_t exec_env)
{
    WasiP2WaitInterrupt *interrupt = NULL;
    uint8_t byte = 1;
    ssize_t written = -1;

    if (!exec_env) {
        return;
    }

    os_mutex_lock(&exec_env->wait_lock);
    interrupt = exec_env->wasi_p2_wait_interrupt;
    if (interrupt && !interrupt->interrupted) {
        interrupt->interrupted = true;
        do {
            written = write(interrupt->write_fd, &byte, sizeof(byte));
        } while (written < 0 && errno == EINTR);
        (void)written;
    }
    os_mutex_unlock(&exec_env->wait_lock);
}

void
wasi_p2_interrupt_wait_destroy(wasm_exec_env_t exec_env)
{
    WasiP2WaitInterrupt *interrupt = NULL;

    if (!exec_env) {
        return;
    }

    os_mutex_lock(&exec_env->wait_lock);
    interrupt = exec_env->wasi_p2_wait_interrupt;
    exec_env->wasi_p2_wait_interrupt = NULL;
    os_mutex_unlock(&exec_env->wait_lock);

    if (interrupt) {
        if (interrupt->read_fd >= 0) {
            close(interrupt->read_fd);
        }
        if (interrupt->write_fd >= 0) {
            close(interrupt->write_fd);
        }
        wasi_p2_native_fd_quota_release(&interrupt->fd_lease);
        wasm_runtime_free(interrupt);
    }
}
#else
void
wasi_p2_interrupt_wait_request(wasm_exec_env_t exec_env)
{
    (void)exec_env;
}

void
wasi_p2_interrupt_wait_destroy(wasm_exec_env_t exec_env)
{
    (void)exec_env;
}
#endif

static void
set_flush_error(wasi_result_void_stream_error_t *ret, int error)
{
    ret->is_err = true;
    ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
    ret->u.err.payload.error = error;
}

static void
release_flushing_stream_reservation(FlushingStreamReservation *reservation)
{
    if (reservation && reservation->node) {
        wasm_runtime_free(reservation->node);
        reservation->node = NULL;
    }
}

/* Reserve every quota-sensitive flush-tracking allocation before a caller can
 * mutate the stream.  Regular files and FIFOs never enter the tracking list;
 * Darwin also has no TIOCOUTQ path and therefore never needs a node. */
static bool
prepare_flushing_stream_reservation(wasi_output_stream_t stream,
                                    FlushingStreamReservation *reservation,
                                    wasi_result_void_stream_error_t *ret)
{
    struct stat statbuf;

    memset(reservation, 0, sizeof(*reservation));
    if (fstat(stream, &statbuf) < 0) {
        set_flush_error(ret, errno);
        return false;
    }

    reservation->regular_file = S_ISREG(statbuf.st_mode);
    if (reservation->regular_file || S_ISFIFO(statbuf.st_mode)) {
        return true;
    }

#if defined(__APPLE__)
    return true;
#else
    reservation->track_output_queue = true;
    reservation->node = wasm_runtime_malloc(sizeof(*reservation->node));
    if (!reservation->node) {
        set_flush_error(ret, ENOMEM);
        return false;
    }
    reservation->node->stream = stream;
    reservation->node->next = NULL;
    return true;
#endif
}

/* Thread safety is handled by the caller.  Ownership of the reserved node is
 * transferred to the flushing list without another allocation. */
static void
add_reserved_to_flushing_list(FlushingStreamReservation *reservation)
{
    reservation->node->next = flushing_streams_list;
    flushing_streams_list = reservation->node;
    reservation->node = NULL;
}

/**
 * @brief Checks if a stream is in the global list of flushing streams.
 * @details This is an internal helper function used to determine if a `flush`
 *          operation is currently in progress for a given stream. According to
 *          the `wasi:io/streams` specification, `check-write` should return 0
 *          while a flush is ongoing. Thread safety is handled by the
 *          calling function.
 * @param stream The output stream handle to check.
 * @return `true` if the stream is currently in the flushing list, `false`
 * otherwise.
 */
static bool
is_in_flushing_list(wasi_output_stream_t stream)
{
    FlushingStreamNode *curr = flushing_streams_list;
    while (curr) {
        if (curr->stream == stream) {
            return true;
        }
        curr = curr->next;
    }
    return false;
}

/**
 * @brief Removes an output stream from the global list of flushing streams.
 * @details This is an internal helper function, likely called after a `flush`
 *          operation has completed for a stream. It uses a pointer-to-pointer
 *          traversal to efficiently remove the node from the linked list.
 *          Thread safety is handled by the calling function.
 * @param stream The output stream handle to remove from the list.
 */
static void
remove_from_flushing_list(wasi_output_stream_t stream)
{
    // Use a pointer-to-pointer to traverse the list. This allows modifying
    // the `next` pointer of the previous node (or the list head) directly.
    FlushingStreamNode **p = &flushing_streams_list;
    while (*p) {
        FlushingStreamNode *entry = *p;
        if (entry->stream == stream) {
            // Found the node. Bypass it by pointing the previous `next`
            // pointer to the current node's `next`.
            *p = entry->next;
            wasm_runtime_free(entry);
            break;
        }
        // Advance to the address of the next pointer in the list.
        p = &(*p)->next;
    }
}

static void
output_stream_flush_reserved(wasi_output_stream_t stream,
                             FlushingStreamReservation *reservation,
                             wasi_result_void_stream_error_t *ret);

static void
output_stream_blocking_flush_reserved(wasi_output_stream_t stream,
                                      FlushingStreamReservation *reservation,
                                      wasi_result_void_stream_error_t *ret);

// wasi:io/poll

static short
pollable_get_events(const wasi_pollable_context_t *pollable)
{
    switch (pollable->type) {
        case WASI_POLLABLE_IN:
            return POLLIN;
        case WASI_POLLABLE_OUT:
            return POLLOUT;
        case WASI_POLLABLE_SOCK:
            return POLLIN | POLLPRI | POLLOUT;
        case WASI_POLLABLE_HOST_CALLBACK:
            return POLLIN;
        default:
            return 0;
    }
}

wasi_p2_wait_status_t
wasi_pollable_block_interruptible(wasm_exec_env_t exec_env,
                                  wasi_pollable_context_t *pollable)
{
    struct pollfd pfds[2];
    uint32_t pollfd_count = 1;
    int poll_result = 0;
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
    WasiP2WaitInterrupt *interrupt = NULL;

    if (exec_env) {
        if (wait_interrupt_exec_env_terminated(exec_env)) {
            return WASI_P2_WAIT_INTERRUPTED;
        }
        if (!(interrupt = wait_interrupt_get(exec_env))) {
            return WASI_P2_WAIT_FAILED;
        }
        if (wait_interrupt_is_requested(exec_env, interrupt)) {
            return WASI_P2_WAIT_INTERRUPTED;
        }
        pfds[1].fd = interrupt->read_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        pollfd_count = 2;
    }
#else
    (void)exec_env;
#endif

    if (!pollable || pollable->type == WASI_POLLABLE_ALWAYS_READY) {
        return pollable ? WASI_P2_WAIT_READY : WASI_P2_WAIT_FAILED;
    }
    if (pollable->type == WASI_POLLABLE_HOST_CALLBACK) {
        for (;;) {
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
            if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
                return WASI_P2_WAIT_INTERRUPTED;
            }
#endif
            if (wasi_p2_host_pollable_ready(pollable->host_context)) {
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
                if (interrupt
                    && wait_interrupt_is_requested(exec_env, interrupt)) {
                    return WASI_P2_WAIT_INTERRUPTED;
                }
#endif
                return WASI_P2_WAIT_READY;
            }
            wasi_p2_host_pollable_drain(pollable->host_context);
            if (wasi_p2_host_pollable_ready(pollable->host_context)) {
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
                if (interrupt
                    && wait_interrupt_is_requested(exec_env, interrupt)) {
                    return WASI_P2_WAIT_INTERRUPTED;
                }
#endif
                return WASI_P2_WAIT_READY;
            }
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
            if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
                return WASI_P2_WAIT_INTERRUPTED;
            }
#endif
            pfds[0].fd = pollable->fd;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            do {
                pfds[1].revents = 0;
                poll_result = poll(pfds, pollfd_count, -1);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result < 0) {
                return WASI_P2_WAIT_FAILED;
            }
        }
    }
    pfds[0].fd = pollable->fd;
    pfds[0].events = pollable_get_events(pollable);
    pfds[0].revents = 0;
    do {
        pfds[1].revents = 0;
        poll_result = poll(pfds, pollfd_count, -1);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
        return WASI_P2_WAIT_FAILED;
    }
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
    if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
        return WASI_P2_WAIT_INTERRUPTED;
    }
#endif
    return pfds[0].revents != 0 ? WASI_P2_WAIT_READY : WASI_P2_WAIT_FAILED;
}

void
wasi_pollable_block(wasi_pollable_context_t *pollable)
{
    (void)wasi_pollable_block_interruptible(NULL, pollable);
}

/**
 * @brief Check if a pollable resource is ready.
 * @details Implements the non-blocking check for a `pollable` resource from
 *          the `wasi:io/poll` interface. This function returns immediately
 *          with the current readiness state of the pollable.
 * @param pollable The pollable handle to check. In this POSIX implementation,
 *                 this is expected to be a file descriptor.
 * @return `true` if the pollable is ready, `false` otherwise.
 */
bool
wasi_pollable_ready(wasi_pollable_context_t *pollable)
{
    struct pollfd pfd;

    if (!pollable) {
        return false;
    }
    if (pollable->type == WASI_POLLABLE_ALWAYS_READY) {
        return true;
    }
    if (pollable->type == WASI_POLLABLE_HOST_CALLBACK) {
        return wasi_p2_host_pollable_ready(pollable->host_context);
    }
    pfd.fd = pollable->fd;
    pfd.events = pollable_get_events(pollable);
    pfd.revents = 0;

    // Use the underlying POSIX poll function with a timeout of 0.
    // This makes the call non-blocking, returning immediately with the
    // current status of the descriptor.
    int ret = poll(&pfd, 1, 0);

    // `poll` returns the number of descriptors that are ready.
    // We return true only if our single descriptor is ready.
    return ret == 1;
}

/**
 * @brief Wait for a set of pollable resources to become ready.
 * @details Implements the `poll` function from the `wasi:io/poll` interface.
 *          This function takes a list of pollables and blocks until at least
 *          one of them is ready. It is similar to the `poll` system call in
 * POSIX.
 *
 * @note The caller is responsible for freeing the returned list (`ret->buf`).
 *
 * @param pollables An array of pointers to pollable context to wait on.
 * @param n_pollables The number of pollable contexts in the array.
 * @param[out] ret A pointer to a list struct that will be populated with the
 *                 indices of the pollables that have become ready.
 */
static uint32_t
collect_ready_pollables(const wasi_pollable_context_t **pollables,
                        const struct pollfd *pfds, uint32_t n_pollables,
                        uint32_t *ready_indices)
{
    uint32_t ready_count = 0;

    for (uint32_t i = 0; i < n_pollables; i++) {
        if (pollables[i]->type == WASI_POLLABLE_ALWAYS_READY
            || (pollables[i]->type == WASI_POLLABLE_HOST_CALLBACK
                && wasi_p2_host_pollable_ready(pollables[i]->host_context))
            || (pollables[i]->type != WASI_POLLABLE_HOST_CALLBACK
                && pfds[i].revents != 0)) {
            ready_indices[ready_count++] = i;
        }
    }
    return ready_count;
}

wasi_p2_wait_status_t
wasi_poll_interruptible(wasm_exec_env_t exec_env,
                        const wasi_pollable_context_t **pollables,
                        uint32_t n_pollables, wasi_list_u32_t *ret)
{
    uint32_t *ready_indices = NULL;
    uint32_t ready_count = 0;
    uint32_t pollfd_count = n_pollables;
    int poll_result = 0;
    wasi_p2_wait_status_t status = WASI_P2_WAIT_FAILED;
    struct pollfd *pfds = NULL;
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
    WasiP2WaitInterrupt *interrupt = NULL;
#else
    (void)exec_env;
#endif

    if (!ret) {
        return WASI_P2_WAIT_FAILED;
    }
    ret->buf = NULL;
    ret->len = 0;

    if (n_pollables == 0) {
        return WASI_P2_WAIT_READY;
    }
    if (!pollables || n_pollables > UINT32_MAX / sizeof(struct pollfd)
        || n_pollables > UINT32_MAX / sizeof(uint32_t)) {
        return WASI_P2_WAIT_FAILED;
    }

#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
    if (exec_env) {
        if (wait_interrupt_exec_env_terminated(exec_env)) {
            return WASI_P2_WAIT_INTERRUPTED;
        }
        if (!(interrupt = wait_interrupt_get(exec_env))
            || n_pollables == UINT32_MAX) {
            return WASI_P2_WAIT_FAILED;
        }
        if (wait_interrupt_is_requested(exec_env, interrupt)) {
            return WASI_P2_WAIT_INTERRUPTED;
        }
        pollfd_count++;
    }
#endif

    if (pollfd_count > UINT32_MAX / sizeof(struct pollfd)) {
        return WASI_P2_WAIT_FAILED;
    }
    pfds = wasm_runtime_malloc(sizeof(struct pollfd) * pollfd_count);
    if (!pfds) {
        return WASI_P2_WAIT_FAILED;
    }

    ready_indices = wasm_runtime_malloc(sizeof(uint32_t) * n_pollables);
    if (!ready_indices) {
        wasm_runtime_free(pfds);
        return WASI_P2_WAIT_FAILED;
    }

    // Initialize the pollfd array for both fd-backed and host pollables.
    for (uint32_t i = 0; i < n_pollables; i++) {
        if (!pollables[i]) {
            goto cleanup;
        }
        pfds[i].fd = pollables[i]->fd;
        pfds[i].events = pollable_get_events(pollables[i]);
        pfds[i].revents = 0;
    }
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
    if (interrupt) {
        pfds[n_pollables].fd = interrupt->read_fd;
        pfds[n_pollables].events = POLLIN;
        pfds[n_pollables].revents = 0;
    }
#endif

    for (;;) {
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
        if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
            status = WASI_P2_WAIT_INTERRUPTED;
            break;
        }
#endif
        do {
            poll_result = poll(pfds, pollfd_count, 0);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            break;
        }

        ready_count = collect_ready_pollables(pollables, pfds, n_pollables,
                                              ready_indices);
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
        if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
            status = WASI_P2_WAIT_INTERRUPTED;
            break;
        }
#endif
        if (ready_count != 0) {
            ret->buf = ready_indices;
            ret->len = ready_count;
            ready_indices = NULL;
            status = WASI_P2_WAIT_READY;
            break;
        }

        /* A notify is only a wake edge. Drain it, then recheck the host's
         * level before entering the blocking poll to avoid a lost wake. */
        for (uint32_t i = 0; i < n_pollables; i++) {
            if (pollables[i]->type == WASI_POLLABLE_HOST_CALLBACK) {
                wasi_p2_host_pollable_drain(pollables[i]->host_context);
            }
            pfds[i].revents = 0;
        }
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
        if (interrupt) {
            pfds[n_pollables].revents = 0;
            if (wait_interrupt_is_requested(exec_env, interrupt)) {
                status = WASI_P2_WAIT_INTERRUPTED;
                break;
            }
        }
#endif

        do {
            poll_result = poll(pfds, pollfd_count, 0);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            break;
        }
        ready_count = collect_ready_pollables(pollables, pfds, n_pollables,
                                              ready_indices);
#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_THREAD_MGR != 0
        if (interrupt && wait_interrupt_is_requested(exec_env, interrupt)) {
            status = WASI_P2_WAIT_INTERRUPTED;
            break;
        }
#endif
        for (uint32_t i = 0; i < pollfd_count; i++) {
            pfds[i].revents = 0;
        }
        if (ready_count != 0) {
            ret->buf = ready_indices;
            ret->len = ready_count;
            ready_indices = NULL;
            status = WASI_P2_WAIT_READY;
            break;
        }

        do {
            poll_result = poll(pfds, pollfd_count, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            break;
        }
    }

cleanup:
    if (ready_indices) {
        wasm_runtime_free(ready_indices);
    }
    wasm_runtime_free(pfds);
    return status;
}

void
wasi_poll(const wasi_pollable_context_t **pollables, uint32_t n_pollables,
          wasi_list_u32_t *ret)
{
    (void)wasi_poll_interruptible(NULL, pollables, n_pollables, ret);
}

// wasi:io/streams

/* A zero-length read must not consume input, but it still must not turn an
 * invalid or write-only descriptor into a successful input stream. */
static void
validate_zero_length_input_stream(wasi_input_stream_t stream,
                                  wasi_result_list_u8_stream_error_t *ret)
{
    int flags = fcntl(stream, F_GETFL, 0);

    if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = flags < 0 ? errno : EBADF;
    }
}

/**
 * @brief Perform a non-blocking read from an input stream.
 * @details Implements the `read` method on the `input-stream` resource from the
 *          `wasi:io/streams` interface. It attempts to read up to `len` bytes,
 *          but may return fewer if not all data is immediately available.
 *
 * @note The caller is responsible for freeing the returned list
 * (`ret->u.ok.buf`).
 *
 * @param stream The input stream handle (file descriptor) to read from.
 * @param len The maximum number of bytes to read.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 list of bytes read. On failure, it contains a stream error.
 */
void
wasi_input_stream_read(wasi_input_stream_t stream, uint64_t len,
                       wasi_result_list_u8_stream_error_t *ret)
{
    uint8_t *buf = NULL;
    ssize_t s;

    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));

    if (len == 0) {
        validate_zero_length_input_stream(stream, ret);
        return;
    }
    if (len > UINT32_MAX) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = EOVERFLOW;
        return;
    }

    buf = wasm_runtime_malloc((uint32_t)len);
    if (!buf) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = ENOMEM;
        return;
    }

    s = read(stream, buf, (size_t)len);
    if (s < 0) {
        int read_error = errno;

        wasm_runtime_free(buf);
        buf = NULL;
        if (read_error == EAGAIN || read_error == EWOULDBLOCK) {
            ret->is_err = false;
            ret->u.ok.buf = NULL;
            ret->u.ok.buf_len = 0;
            return;
        }
        else {
            ret->is_err = true;
            ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
            ret->u.err.payload.error = read_error;
            return;
        }
    }
    if (s == 0) {
        wasm_runtime_free(buf);
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_CLOSED;
        return;
    }

    ret->is_err = false;
    ret->u.ok.buf = buf;
    ret->u.ok.buf_len = s;
}

/**
 * @brief Perform a blocking read from an input stream.
 * @details Implements the `blocking-read` method on the `input-stream` resource
 *          from the `wasi:io/streams` interface. It blocks until data is
 *          available and then performs a read.
 *
 * @note The caller is responsible for freeing the returned list
 * (`ret->u.ok.buf`).
 *
 * @param stream The input stream handle (file descriptor) to read from.
 * @param len The maximum number of bytes to read.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 list of bytes read. On failure, it contains a stream error.
 */
void
wasi_input_stream_blocking_read(wasi_input_stream_t stream, uint64_t len,
                                wasi_result_list_u8_stream_error_t *ret)
{
    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));

    if (len == 0) {
        validate_zero_length_input_stream(stream, ret);
        return;
    }
    if (len > UINT32_MAX) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = EOVERFLOW;
        return;
    }

    struct pollfd pfd;
    pfd.fd = stream;
    pfd.events = POLLIN;
    int n = 0;

    // Block until the stream is ready for reading.
    // The loop handles cases where the poll system call is interrupted by a
    // signal.
    do {
        n = poll(&pfd, 1, -1);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = errno;
        return;
    }

    if (n == 0) {
        // This should not happen with a timeout of -1, but as a safeguard,
        // return an empty list to indicate no data was read.
        ret->is_err = false;
        ret->u.ok.buf = NULL;
        ret->u.ok.buf_len = 0;
        return;
    }

    // Now that we know there's data available, we can delegate to the
    // non-blocking read function to perform the actual read operation.
    wasi_input_stream_read(stream, len, ret);
}

/**
 * @brief Skip bytes from an input stream.
 * @details Implements the `skip` method on the `input-stream` resource from the
 *          `wasi:io/streams` interface. It attempts to efficiently skip bytes
 *          using `lseek` for seekable streams, and falls back to reading and
 *          discarding data for non-seekable streams.
 * @param stream The input stream handle (file descriptor).
 * @param len The maximum number of bytes to skip.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 number of bytes actually skipped. On failure, it contains a
 *                 stream error.
 */
void
wasi_input_stream_skip(wasi_input_stream_t stream, uint64_t len,
                       wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

    // Try to use lseek for seekable streams
    off_t current_pos = lseek(stream, 0, SEEK_CUR);
    if (current_pos != -1) {
        off_t new_pos = lseek(stream, len, SEEK_CUR);
        if (new_pos != -1) {
            ret->is_err = false;
            ret->u.ok = new_pos - current_pos;
            return;
        }
        // If lseek fails, it might be a non-seekable stream (e.g., a pipe).
        // Reset errno and fall through to the read-based approach.
        errno = 0;
    }

    // Fallback for non-seekable streams: read and discard
    char buf[4096];
    uint64_t skipped_total = 0;
    while (skipped_total < len) {
        uint64_t to_read = len - skipped_total;
        if (to_read > sizeof(buf)) {
            to_read = sizeof(buf);
        }

        ssize_t bytes_read = read(stream, buf, to_read);

        if (bytes_read < 0) {
            // Retry if the read was interrupted by a signal.
            if (errno == EINTR) {
                continue;
            }
            ret->is_err = true;
            ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
            ret->u.err.payload.error = errno;
            return;
        }

        if (bytes_read == 0) {
            // End of stream
            break;
        }

        skipped_total += bytes_read;
    }

    if (!skipped_total && len) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_CLOSED;
        return;
    }

    ret->is_err = false;
    ret->u.ok = skipped_total;
}

/**
 * @brief Skip bytes from a stream, blocking until data is available.
 * @details Implements the `blocking-skip` method on the `input-stream` resource
 *          from the `wasi:io/streams` interface. It blocks until the stream is
 *          ready and then performs a skip operation.
 * @param stream The input stream handle (file descriptor).
 * @param len The maximum number of bytes to skip.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 number of bytes actually skipped. On failure, it contains a
 *                 stream error.
 */
void
wasi_input_stream_blocking_skip(wasi_input_stream_t stream, uint64_t len,
                                wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

    struct pollfd pfd;
    pfd.fd = stream;
    pfd.events = POLLIN;
    int n = 0;

    // Block until the stream is ready for reading.
    // The loop handles cases where the poll system call is interrupted by a
    // signal.
    do {
        n = poll(&pfd, 1, -1);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = errno;
        return;
    }

    if (n == 0) {
        // This should not happen with a timeout of -1, but as a safeguard,
        // return 0 to indicate no bytes were skipped.
        ret->is_err = false;
        ret->u.ok = 0;
        return;
    }

    // Now that the stream is ready, delegate to the non-blocking skip function
    // to perform the actual skip operation.
    wasi_input_stream_skip(stream, len, ret);
}

/**
 * @brief Check readiness for writing to an output stream.
 * @details Implements the `check-write` method on the `output-stream` resource
 *          from the `wasi:io/streams` interface. This function is non-blocking
 *          and returns the number of bytes that can be written without
 * blocking.
 * @param stream The output stream handle (file descriptor).
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 number of bytes permitted for the next write. On failure, it
 *                 contains a stream error.
 */
void
wasi_output_stream_check_write(wasi_output_stream_t stream,
                               wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

    // Lock the mutex to safely check and update the flushing streams list.
    pthread_mutex_lock(&flushing_streams_list_lock);

    // If the stream is currently flushing, we must check if it's done.
    if (is_in_flushing_list(stream)) {
        int queue_size = 0;
#if defined(__APPLE__)
        // macOS/Darwin has no TIOCOUTQ to query the bytes still queued in the
        // socket/tty output buffer. Treat the output queue as already drained
        // (0 bytes queued) so the flush is reported complete and writes are not
        // blocked. This is the conservative "is the queue empty" answer.
        queue_size = 0;
#else
        // Use ioctl with TIOCOUTQ to get the number of bytes in the output
        // buffer.
        if (ioctl(stream, TIOCOUTQ, &queue_size) < 0) {
            pthread_mutex_unlock(&flushing_streams_list_lock);
            ret->is_err = true;
            ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
            ret->u.err.payload.error = errno;
            return;
        }
#endif

        if (queue_size == 0) {
            // If the output queue is empty, the flush is complete.
            remove_from_flushing_list(stream);
        }
        else {
            // If the queue is not empty, the flush is still in progress.
            // According to the spec, we must return a permit of 0 bytes.
            pthread_mutex_unlock(&flushing_streams_list_lock);
            ret->is_err = false;
            ret->u.ok = 0;
            return;
        }
    }

    pthread_mutex_unlock(&flushing_streams_list_lock);

    // Determine the type of the descriptor to provide an accurate buffer size.
    struct stat statbuf;
    if (fstat(stream, &statbuf) < 0) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = errno;
        return;
    }

    if (S_ISSOCK(statbuf.st_mode)) {
        int available_space = 0;
        socklen_t len = sizeof(available_space);
        if (getsockopt(stream, SOL_SOCKET, SO_SNDBUF, &available_space, &len)
            < 0) {
            ret->is_err = true;
            ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
            ret->u.err.payload.error = errno;
            return;
        }

#if !defined(__APPLE__)
        int used_space = 0;
        if (ioctl(stream, TIOCOUTQ, &used_space) == 0) {
            available_space -= used_space;
        }
#else
        // macOS/Darwin has no TIOCOUTQ to learn how many bytes are already
        // queued, so SO_SNDBUF cannot be turned into exact free space.
        // Reporting the full SO_SNDBUF would over-grant: the socket is
        // non-blocking and wasi_output_stream_write treats a short write as a
        // stream error, so an over-large permit turns normal TCP backpressure
        // into a write failure. Report a readiness-based permit instead — only
        // when the socket is writable (POLLOUT), and capped at SO_SNDLOWAT, the
        // send low-water mark that POLLOUT guarantees is free.
        struct pollfd wpfd = { .fd = stream, .events = POLLOUT };
        if (poll(&wpfd, 1, 0) <= 0 || !(wpfd.revents & POLLOUT)) {
            available_space = 0;
        }
        else {
            int low_water = 0;
            socklen_t lwlen = sizeof(low_water);
            if (getsockopt(stream, SOL_SOCKET, SO_SNDLOWAT, &low_water, &lwlen)
                    < 0
                || low_water <= 0) {
                low_water = 1024; // macOS default send low-water mark
            }
            if (available_space > low_water) {
                available_space = low_water;
            }
        }
#endif

        ret->is_err = false;
        ret->u.ok = available_space > 0 ? available_space : 0;
    }
    else if (S_ISFIFO(statbuf.st_mode)) {
        // For pipes, we can't easily get the available buffer space.
        // Return a reasonable chunk size as a permit.
        ret->is_err = false;
        ret->u.ok = 4096;
    }
    else {
        // For regular files, writes are usually buffered by the OS and rarely
        // block. We can permit a large write.
        ret->is_err = false;
        ret->u.ok = 65536; // 64k, a reasonable buffer size
    }
}

/**
 * @brief Perform a non-blocking write to an output stream.
 * @details Implements the `write` method on the `output-stream` resource from
 *          the `wasi:io/streams` interface. It attempts to write the entire
 *          payload in a single operation.
 * @note Per the WIT specification, this function should only be called after
 *       `check-write` has granted a permit of sufficient size. A partial write
 *       is treated as an error.
 * @param stream The output stream handle (file descriptor).
 * @param payload A list containing the data to be written.
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
void
wasi_output_stream_write(wasi_output_stream_t stream,
                         const wasi_list_u8_t *payload,
                         wasi_result_void_stream_error_t *ret)
{
    if (!payload || !ret) {
        return;
    }

    const uint8_t *buf = payload->buf;
    size_t len = payload->buf_len;

    // Perform the underlying POSIX write operation.
    ssize_t s = write(stream, buf, len);

    if (s < 0) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = errno;
        return;
    }

    if ((size_t)s < len) {
        // A partial write is considered an error, as `check-write` should
        // have guaranteed enough space for the entire write to succeed.
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = EACCES;
    }
    else {
        // The full payload was written successfully.
        ret->is_err = false;
    }
}

/**
 * @brief Perform a blocking write of a list of bytes and then a blocking flush.
 * @details Implements the `blocking-write-and-flush` method on the
 * `output-stream` resource from the `wasi:io/streams` interface. This function
 * writes the entire payload, blocking as necessary, and then blocks until the
 *          stream is fully flushed.
 * @param stream The output stream handle (file descriptor).
 * @param payload A list containing the data to be written.
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
static void
output_stream_blocking_write_and_flush_reserved(
    wasi_output_stream_t stream, const wasi_list_u8_t *payload,
    FlushingStreamReservation *reservation,
    wasi_result_void_stream_error_t *ret)
{
    uint64_t remaining = payload->buf_len;
    uint8_t *buf = payload->buf;
    wasi_pollable_context_t pollable = { .fd = stream,
                                         .own_fd = false,
                                         .type = WASI_POLLABLE_OUT,
                                         .host_context = NULL };

    // Loop until the entire payload has been written.
    while (remaining > 0) {
        wasi_pollable_block(&pollable);
        wasi_result_u64_stream_error_t check_res;
        wasi_output_stream_check_write(stream, &check_res);
        if (check_res.is_err) {
            ret->is_err = true;
            ret->u.err = check_res.u.err;
            goto cleanup;
        }

        uint64_t n = check_res.u.ok;
        uint64_t len = remaining < n ? remaining : n;
        if (len == 0) {
            // If the permit is 0, loop again to block until a non-zero
            // permit is available.
            continue;
        }

        wasi_list_u8_t chunk = { .buf = buf, .buf_len = len };
        wasi_result_void_stream_error_t write_res;
        wasi_output_stream_write(stream, &chunk, &write_res);
        if (write_res.is_err) {
            ret->is_err = true;
            ret->u.err = write_res.u.err;
            goto cleanup;
        }

        // Advance the buffer pointer and update the remaining byte count.
        buf += len;
        remaining -= len;
    }

    // After all bytes have been written, perform a blocking flush to ensure
    // all data is sent to its final destination.
    output_stream_blocking_flush_reserved(stream, reservation, ret);

cleanup:
    return;
}

void
wasi_output_stream_blocking_write_and_flush(
    wasi_output_stream_t stream, const wasi_list_u8_t *payload,
    wasi_result_void_stream_error_t *ret)
{
    FlushingStreamReservation reservation;

    if (!payload || !ret) {
        return;
    }
    if (!prepare_flushing_stream_reservation(stream, &reservation, ret)) {
        return;
    }
    output_stream_blocking_write_and_flush_reserved(stream, payload,
                                                    &reservation, ret);
    release_flushing_stream_reservation(&reservation);
}

/**
 * @brief Request to flush buffered output for a stream.
 * @details Implements the `flush` method on the `output-stream` resource from
 *          the `wasi:io/streams` interface. This function is non-blocking.
 *          It initiates a flush and returns immediately.
 * @note For regular files, this function is a no-op due to the blocking nature
 *       of `fsync`. For sockets, it initiates a flush by marking the stream,
 *       which will cause subsequent `check-write` calls to return 0 until the
 *       kernel's output buffer is empty.
 * @param stream The output stream handle (file descriptor).
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
static void
output_stream_flush_reserved(wasi_output_stream_t stream,
                             FlushingStreamReservation *reservation,
                             wasi_result_void_stream_error_t *ret)
{
    if (!reservation->track_output_queue) {
        // For regular files and pipes, flush is a no-op because writes are
        // generally buffered by the OS or immediately available. The
        // blocking-flush function is responsible for fsync on files. Darwin
        // also has no TIOCOUTQ support, so socket flushes complete here.
        ret->is_err = false;
        return;
    }

    // Lock the mutex to safely check and update the flushing streams list.
    pthread_mutex_lock(&flushing_streams_list_lock);

    int queue_size = 0;
#if defined(__APPLE__)
    // macOS/Darwin has no TIOCOUTQ to query the bytes still queued in the
    // kernel's output buffer. Treat the output queue as already drained
    // (0 bytes queued) so the stream is never marked as flushing and
    // subsequent check-write calls proceed immediately.
    queue_size = 0;
#else
    // Use ioctl with TIOCOUTQ to get the number of bytes in the kernel's output
    // buffer.
    if (ioctl(stream, TIOCOUTQ, &queue_size) < 0) {
        pthread_mutex_unlock(&flushing_streams_list_lock);
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
        ret->u.err.payload.error = errno;
        return;
    }
#endif

    // If there is data in the output queue, mark the stream as flushing.
    if (queue_size > 0) {
        if (!is_in_flushing_list(stream)) {
            if (!reservation->node) {
                pthread_mutex_unlock(&flushing_streams_list_lock);
                set_flush_error(ret, ENOMEM);
                return;
            }
            add_reserved_to_flushing_list(reservation);
        }
    }

    pthread_mutex_unlock(&flushing_streams_list_lock);

    ret->is_err = false;
}

void
wasi_output_stream_flush(wasi_output_stream_t stream,
                         wasi_result_void_stream_error_t *ret)
{
    FlushingStreamReservation reservation;

    if (!ret) {
        return;
    }
    if (!prepare_flushing_stream_reservation(stream, &reservation, ret)) {
        return;
    }
    output_stream_flush_reserved(stream, &reservation, ret);
    release_flushing_stream_reservation(&reservation);
}

/**
 * @brief Block until all buffered output has been flushed to its destination.
 * @details Implements the `blocking-flush` method on the `output-stream`
 *          resource from the `wasi:io/streams` interface. This function
 *          guarantees that all data written prior to this call is durably
 *          stored.
 * @param stream The output stream handle (file descriptor).
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
static void
output_stream_blocking_flush_reserved(wasi_output_stream_t stream,
                                      FlushingStreamReservation *reservation,
                                      wasi_result_void_stream_error_t *ret)
{
    // For regular files, fsync is the correct blocking flush operation.
    if (reservation->regular_file) {
        if (fsync(stream) != 0) {
            // fsync can fail with EINVAL or EROFS on file systems that
            // don't support it for the given descriptor. In these cases,
            // we should not report an error.
            if (errno != EINVAL && errno != EROFS) {
                ret->is_err = true;
                ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
                ret->u.err.payload.error = errno;
                return;
            }
        }
        ret->is_err = false;
        return;
    }

    // For other stream types (sockets, pipes), use the polling mechanism.
    // 1. Initiate the flush.
    wasi_result_void_stream_error_t flush_res;
    output_stream_flush_reserved(stream, reservation, &flush_res);
    if (flush_res.is_err) {
        *ret = flush_res;
        return;
    }

    // 2. Poll until the stream is ready for writing again.
    wasi_pollable_context_t pollable = { .fd = stream,
                                         .own_fd = false,
                                         .type = WASI_POLLABLE_OUT,
                                         .host_context = NULL };
    while (true) {
        wasi_pollable_block(&pollable);

        wasi_result_u64_stream_error_t check_res;
        wasi_output_stream_check_write(stream, &check_res);

        if (check_res.is_err) {
            ret->is_err = true;
            ret->u.err = check_res.u.err;
            return;
        }

        // The flush is complete when check-write grants a permit > 0,
        // which means the stream is no longer considered to be flushing.
        if (check_res.u.ok > 0) {
            break;
        }
    }

    ret->is_err = false;
}

void
wasi_output_stream_blocking_flush(wasi_output_stream_t stream,
                                  wasi_result_void_stream_error_t *ret)
{
    FlushingStreamReservation reservation;

    if (!ret) {
        return;
    }
    if (!prepare_flushing_stream_reservation(stream, &reservation, ret)) {
        return;
    }
    output_stream_blocking_flush_reserved(stream, &reservation, ret);
    release_flushing_stream_reservation(&reservation);
}

/**
 * @brief Write a sequence of zero bytes to an output stream.
 * @details Implements the `write-zeroes` method on the `output-stream` resource
 *          from the `wasi:io/streams` interface. It includes an optimization
 *          for regular files using `ftruncate` and falls back to manual
 *          writing for other stream types (e.g., sockets, pipes).
 * @param stream The output stream handle (file descriptor).
 * @param len The number of zero bytes to write.
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
void
wasi_output_stream_write_zeroes(wasi_output_stream_t stream, uint64_t len,
                                wasi_result_void_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

    struct stat statbuf;
    if (fstat(stream, &statbuf) == 0 && S_ISREG(statbuf.st_mode)) {
        off_t current_pos = lseek(stream, 0, SEEK_CUR);
        if (current_pos != -1) {
            // ftruncate is often a highly efficient way to extend a file with
            // zeroes.
            if (ftruncate(stream, current_pos + len) == 0) {
                lseek(stream, len, SEEK_CUR); // Advance the file offset.
                ret->is_err = false;
                return;
            }
            // If ftruncate fails, we ignore the error and fall through to the
            // manual write method below.
        }
    }

    char buf[4096] = { 0 };
    uint64_t remaining = len;

    while (remaining > 0) {
        uint64_t write_len = remaining < sizeof(buf) ? remaining : sizeof(buf);
        wasi_list_u8_t payload = { .buf = (uint8_t *)buf,
                                   .buf_len = write_len };
        wasi_result_void_stream_error_t write_res;

        // Reuse the existing write function to write a chunk of zeroes.
        wasi_output_stream_write(stream, &payload, &write_res);
        if (write_res.is_err) {
            ret->is_err = true;
            ret->u.err = write_res.u.err;
            return;
        }
        remaining -= write_len;
    }

    ret->is_err = false;
}

/**
 * @brief Perform a blocking write of a sequence of zero bytes, then a blocking
 * flush.
 * @details Implements the `blocking-write-zeroes-and-flush` method on the
 *          `output-stream` resource from `wasi:io/streams`. This function
 *          writes the specified number of zero bytes, blocking as necessary,
 *          and then blocks until the stream is fully flushed.
 * @param stream The output stream handle (file descriptor).
 * @param len The number of zero bytes to write.
 * @param[out] ret A pointer to the result struct. On success, `is_err` is
 * false. On failure, it contains a stream error.
 */
static void
output_stream_blocking_write_zeroes_and_flush_reserved(
    wasi_output_stream_t stream, uint64_t len,
    FlushingStreamReservation *reservation,
    wasi_result_void_stream_error_t *ret)
{
    uint64_t remaining = len;

    // Loop until the specified number of zero bytes has been written.
    wasi_pollable_context_t pollable = { .fd = stream,
                                         .own_fd = false,
                                         .type = WASI_POLLABLE_OUT,
                                         .host_context = NULL };
    while (remaining > 0) {
        wasi_pollable_block(&pollable);
        wasi_result_u64_stream_error_t check_res;
        wasi_output_stream_check_write(stream, &check_res);
        if (check_res.is_err) {
            ret->is_err = true;
            ret->u.err = check_res.u.err;
            goto cleanup;
        }

        uint64_t n = check_res.u.ok;
        uint64_t write_len = remaining < n ? remaining : n;
        if (write_len == 0) {
            // If the permit is 0, loop again to block until a non-zero
            // permit is available.
            continue;
        }

        // Perform the non-blocking write of zeroes for the permitted chunk
        // size. This reuses the optimized `write_zeroes` implementation.
        wasi_result_void_stream_error_t write_res;
        wasi_output_stream_write_zeroes(stream, write_len, &write_res);
        if (write_res.is_err) {
            ret->is_err = true;
            ret->u.err = write_res.u.err;
            goto cleanup;
        }
        remaining -= write_len;
    }

    // After all bytes have been written, perform a blocking flush.
    output_stream_blocking_flush_reserved(stream, reservation, ret);

cleanup:
    return;
}

void
wasi_output_stream_blocking_write_zeroes_and_flush(
    wasi_output_stream_t stream, uint64_t len,
    wasi_result_void_stream_error_t *ret)
{
    FlushingStreamReservation reservation;

    if (!ret) {
        return;
    }
    if (!prepare_flushing_stream_reservation(stream, &reservation, ret)) {
        return;
    }
    output_stream_blocking_write_zeroes_and_flush_reserved(stream, len,
                                                           &reservation, ret);
    release_flushing_stream_reservation(&reservation);
}

/**
 * @brief Splice data from an input stream to an output stream.
 * @details Implements the `splice` method on the `output-stream` resource from
 *          the `wasi:io/streams` interface. This function moves data from a
 *          source to a destination without copying it into the WebAssembly
 * memory. It includes a zero-copy optimization for Linux using `splice(2)`.
 * @param stream The destination output stream handle (file descriptor).
 * @param src The source input stream handle (file descriptor).
 * @param len The maximum number of bytes to splice.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 number of bytes actually spliced. On failure, it contains a
 *                 stream error.
 */
void
wasi_output_stream_splice(wasi_output_stream_t stream, wasi_input_stream_t src,
                          uint64_t len, wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

#if defined(__linux__)
    // --- Optimization for Linux using zero-copy splice(2) ---
    struct stat stat_in, stat_out;
    if (fstat(src, &stat_in) == 0 && fstat(stream, &stat_out) == 0) {
        bool in_is_pipe = S_ISFIFO(stat_in.st_mode);
        bool out_is_pipe = S_ISFIFO(stat_out.st_mode);

        // splice(2) is most effective with pipes.
        if (in_is_pipe || out_is_pipe) {
            ssize_t s = splice(src, NULL, stream, NULL, len, SPLICE_F_NONBLOCK);
            if (s >= 0) {
                ret->is_err = false;
                ret->u.ok = s;
                return;
            }
            // EINVAL can mean splice is not supported for these descriptor
            // types. In that case, we fall through to the generic read/write
            // implementation.
            if (errno != EINVAL) {
                ret->is_err = true;
                ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
                ret->u.err.payload.error = errno;
                return;
            }
        }
    }
#endif

    // --- Fallback implementation for non-Linux or non-pipe streams ---
    // This follows the logic described in the WIT specification:
    // check-write, then read, then write.

    // 1. Check how much data the destination stream can accept.
    wasi_result_u64_stream_error_t check_res;
    wasi_output_stream_check_write(stream, &check_res);
    if (check_res.is_err) {
        *ret = check_res;
        return;
    }

    uint64_t n = check_res.u.ok;
    // Determine the amount to read: the minimum of what's requested, and
    // what the destination can accept.
    uint64_t read_len = len < n ? len : n;
    if (read_len == 0) {
        ret->is_err = false;
        ret->u.ok = 0;
        return;
    }

    // 2. Read that amount from the source stream.
    wasi_result_list_u8_stream_error_t read_res;
    wasi_input_stream_read(src, read_len, &read_res);
    if (read_res.is_err) {
        ret->is_err = true;
        ret->u.err = read_res.u.err;
        return;
    }

    // 3. Write the data to the destination stream.
    if (read_res.u.ok.buf_len > 0) {
        wasi_result_void_stream_error_t write_res;
        wasi_output_stream_write(stream, &read_res.u.ok, &write_res);
        if (write_res.is_err) {
            wasm_runtime_free(read_res.u.ok.buf);
            ret->is_err = true;
            ret->u.err = write_res.u.err;
            return;
        }
        ret->is_err = false;
        ret->u.ok = read_res.u.ok.buf_len;
    }
    else {
        // Nothing was read, so nothing was spliced.
        ret->is_err = false;
        ret->u.ok = 0;
    }

    // Clean up the buffer allocated by wasi_input_stream_read.
    if (read_res.u.ok.buf) {
        wasm_runtime_free(read_res.u.ok.buf);
    }
}

/**
 * @brief Splice data from an input stream to an output stream, blocking until
 *        the requested number of bytes have been moved.
 * @details Implements the `blocking-splice` method on the `output-stream`
 * resource from the `wasi:io/streams` interface. This function moves data until
 *          `len` bytes have been spliced or the source stream ends. It includes
 *          a zero-copy optimization for Linux using a blocking `splice(2)`
 * loop.
 * @param stream The destination output stream handle (file descriptor).
 * @param src The source input stream handle (file descriptor).
 * @param len The number of bytes to splice.
 * @param[out] ret A pointer to the result struct. On success, it contains the
 *                 number of bytes actually spliced. On failure, it contains a
 *                 stream error.
 */
void
wasi_output_stream_blocking_splice(wasi_output_stream_t stream,
                                   wasi_input_stream_t src, uint64_t len,
                                   wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }

#if defined(__linux__)
    // --- Optimization for Linux using blocking zero-copy splice(2) ---
    struct stat stat_in, stat_out;
    if (fstat(src, &stat_in) == 0 && fstat(stream, &stat_out) == 0) {
        bool in_is_pipe = S_ISFIFO(stat_in.st_mode);
        bool out_is_pipe = S_ISFIFO(stat_out.st_mode);

        if (in_is_pipe || out_is_pipe) {
            uint64_t total_spliced = 0;
            while (total_spliced < len) {
                // Call splice with a flag of 0 to make it a blocking operation.
                ssize_t s =
                    splice(src, NULL, stream, NULL, len - total_spliced, 0);
                if (s > 0) {
                    total_spliced += s;
                }
                else if (s == 0) {
                    // EOF
                    break;
                }
                else {
                    // Handle errors, retrying on EINTR.
                    if (errno == EINTR) {
                        continue;
                    }
                    ret->is_err = true;
                    ret->u.err.kind =
                        WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
                    ret->u.err.payload.error = errno;
                    return;
                }
            }
            ret->is_err = false;
            ret->u.ok = total_spliced;
            return;
        }
    }
#endif

    // --- Fallback implementation using a blocking read/write loop ---
    uint64_t total_spliced = 0;
    char buf[4096];

    while (total_spliced < len) {
        uint64_t to_read = len - total_spliced;
        if (to_read > sizeof(buf)) {
            to_read = sizeof(buf);
        }

        // Perform a blocking read from the source.
        ssize_t bytes_read = read(src, buf, to_read);

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            ret->is_err = true;
            ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
            ret->u.err.payload.error = errno;
            return;
        }

        if (bytes_read == 0) {
            // End of source stream
            break;
        }

        // Perform a blocking write to the destination, handling partial writes.
        uint64_t total_written = 0;
        while (total_written < (uint64_t)bytes_read) {
            ssize_t bytes_written =
                write(stream, buf + total_written, bytes_read - total_written);

            if (bytes_written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ret->is_err = true;
                ret->u.err.kind = WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED;
                ret->u.err.payload.error = errno;
                return;
            }
            total_written += bytes_written;
        }

        total_spliced += total_written;
    }

    if (!total_spliced && len) {
        ret->is_err = true;
        ret->u.err.kind = WASI_STREAM_ERROR_KIND_CLOSED;
        return;
    }

    ret->is_err = false;
    ret->u.ok = total_spliced;
}

static void
set_resource_stream_error(wasi_stream_error_t *error,
                          wasi_stream_error_kind_t kind, int code)
{
    error->kind = kind;
    error->payload.error = code;
}

static bool
file_stream_validate_locked(StreamResourceType *stream, bool writing,
                            int *fd_out)
{
    int fd;
    int flags;
    int access_mode;

    if (!stream || stream->type != STREAM_TYPE_FILE || !stream->position_valid
        || stream->fd > INT_MAX || stream->position > INT64_MAX) {
        errno = EINVAL;
        return false;
    }

    fd = (int)stream->fd;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    access_mode = flags & O_ACCMODE;
    if ((writing && access_mode == O_RDONLY)
        || (!writing && access_mode == O_WRONLY)) {
        errno = EBADF;
        return false;
    }

    /* Linux redirects pwrite() to EOF on an O_APPEND open-file description.
     * Append streams want that behavior, but a positional stream cannot honor
     * its logical cursor without mutating shared status flags. */
    if (writing && !stream->append && (flags & O_APPEND) != 0) {
        errno = ENOTSUP;
        return false;
    }

    *fd_out = fd;
    return true;
}

static void
file_stream_read(StreamResourceType *stream, uint64_t len,
                 wasi_result_list_u8_stream_error_t *ret)
{
    uint8_t *buf = NULL;
    ssize_t bytes_read;
    int fd = -1;
    int saved_error = 0;

    memset(ret, 0, sizeof(*ret));
    if (len > UINT32_MAX || len > (uint64_t)SSIZE_MAX) {
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err,
                                  WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                                  EOVERFLOW);
        return;
    }

    wasi_p2_file_io_lock();
    if (!file_stream_validate_locked(stream, false, &fd)) {
        saved_error = errno;
        goto fail_locked;
    }
    if (len == 0) {
        wasi_p2_file_io_unlock();
        return;
    }
    if (len > (uint64_t)INT64_MAX - stream->position) {
        saved_error = EOVERFLOW;
        goto fail_locked;
    }

    buf = wasm_runtime_malloc((uint32_t)len);
    if (!buf) {
        saved_error = ENOMEM;
        goto fail_locked;
    }

    do {
        bytes_read = pread(fd, buf, (size_t)len, (off_t)stream->position);
    } while (bytes_read < 0 && errno == EINTR);
    if (bytes_read < 0) {
        saved_error = errno;
        wasm_runtime_free(buf);
        buf = NULL;
        if (saved_error == EAGAIN || saved_error == EWOULDBLOCK) {
            wasi_p2_file_io_unlock();
            return;
        }
        goto fail_locked;
    }
    if (bytes_read == 0) {
        wasm_runtime_free(buf);
        wasi_p2_file_io_unlock();
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED,
                                  0);
        return;
    }

    stream->position += (uint64_t)bytes_read;
    wasi_p2_file_io_unlock();
    ret->u.ok.buf = buf;
    ret->u.ok.buf_len = (uint64_t)bytes_read;
    return;

fail_locked:
    wasi_p2_file_io_unlock();
    ret->is_err = true;
    set_resource_stream_error(
        &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, saved_error);
}

static void
file_stream_skip(StreamResourceType *stream, uint64_t len,
                 wasi_result_u64_stream_error_t *ret)
{
    struct stat statbuf;
    uint64_t available;
    uint64_t skipped;
    int fd = -1;
    int saved_error = 0;

    memset(ret, 0, sizeof(*ret));
    wasi_p2_file_io_lock();
    if (!file_stream_validate_locked(stream, false, &fd)) {
        saved_error = errno;
        goto fail_locked;
    }
    if (len == 0) {
        wasi_p2_file_io_unlock();
        return;
    }
    if (fstat(fd, &statbuf) < 0 || statbuf.st_size < 0) {
        saved_error = errno ? errno : EOVERFLOW;
        goto fail_locked;
    }
    if (stream->position >= (uint64_t)statbuf.st_size) {
        wasi_p2_file_io_unlock();
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED,
                                  0);
        return;
    }

    available = (uint64_t)statbuf.st_size - stream->position;
    skipped = len < available ? len : available;
    stream->position += skipped;
    wasi_p2_file_io_unlock();
    ret->u.ok = skipped;
    return;

fail_locked:
    wasi_p2_file_io_unlock();
    ret->is_err = true;
    set_resource_stream_error(
        &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, saved_error);
}

void
wasi_p2_stream_resource_read(StreamResourceType *stream, uint64_t len,
                             bool blocking,
                             wasi_result_list_u8_stream_error_t *ret)
{
    wasi_input_stream_t fd;

    if (!ret) {
        return;
    }
    if (!stream) {
        memset(ret, 0, sizeof(*ret));
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (stream->type == STREAM_TYPE_FILE) {
        file_stream_read(stream, len, ret);
        return;
    }

    fd = (wasi_input_stream_t)stream->fd;
    if (blocking) {
        wasi_input_stream_blocking_read(fd, len, ret);
    }
    else {
        wasi_input_stream_read(fd, len, ret);
    }
}

void
wasi_p2_stream_resource_skip(StreamResourceType *stream, uint64_t len,
                             bool blocking, wasi_result_u64_stream_error_t *ret)
{
    wasi_input_stream_t fd;

    if (!ret) {
        return;
    }
    if (!stream) {
        memset(ret, 0, sizeof(*ret));
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (stream->type == STREAM_TYPE_FILE) {
        file_stream_skip(stream, len, ret);
        return;
    }

    fd = (wasi_input_stream_t)stream->fd;
    if (blocking) {
        wasi_input_stream_blocking_skip(fd, len, ret);
    }
    else {
        wasi_input_stream_skip(fd, len, ret);
    }
}

static bool
file_stream_native_range(uint64_t position, uint64_t len,
                         off_t *native_position)
{
    off_t start;
    off_t end;

    if (position > (uint64_t)INT64_MAX
        || len > (uint64_t)INT64_MAX - position) {
        errno = EOVERFLOW;
        return false;
    }
    start = (off_t)position;
    end = (off_t)(position + len);
    if (start < 0 || end < 0 || (uint64_t)start != position
        || (uint64_t)end != position + len) {
        errno = EOVERFLOW;
        return false;
    }
    *native_position = start;
    return true;
}

static bool
file_stream_write_position_locked(StreamResourceType *stream, int fd,
                                  uint64_t len, uint64_t *position,
                                  off_t *native_position)
{
    struct stat statbuf;
    uint64_t resolved_position = stream->position;

    if (stream->append) {
        if (fstat(fd, &statbuf) < 0) {
            return false;
        }
        if (statbuf.st_size < 0
            || (uint64_t)statbuf.st_size > (uint64_t)INT64_MAX) {
            errno = EOVERFLOW;
            return false;
        }
        resolved_position = (uint64_t)statbuf.st_size;
    }
    if (!file_stream_native_range(resolved_position, len, native_position)) {
        return false;
    }
    *position = resolved_position;
    return true;
}

static void
file_stream_write(StreamResourceType *stream, const wasi_list_u8_t *payload,
                  uint64_t *written_out, wasi_result_void_stream_error_t *ret)
{
    uint64_t write_position;
    uint64_t len;
    ssize_t bytes_written;
    off_t native_write_position;
    int fd = -1;
    int saved_error = 0;

    memset(ret, 0, sizeof(*ret));
    if (written_out) {
        *written_out = 0;
    }
    if (!payload || (payload->buf_len > 0 && !payload->buf)) {
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }

    len = payload->buf_len;
    if (len > (uint64_t)SSIZE_MAX) {
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err,
                                  WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                                  EOVERFLOW);
        return;
    }

    wasi_p2_file_io_lock();
    if (!file_stream_validate_locked(stream, true, &fd)) {
        saved_error = errno;
        goto fail_locked;
    }
    if (len == 0) {
        wasi_p2_file_io_unlock();
        return;
    }

    if (!file_stream_write_position_locked(stream, fd, len, &write_position,
                                           &native_write_position)) {
        saved_error = errno;
        goto fail_locked;
    }

    do {
        bytes_written =
            pwrite(fd, payload->buf, (size_t)len, native_write_position);
    } while (bytes_written < 0 && errno == EINTR);
    if (bytes_written < 0) {
        saved_error = errno;
        goto fail_locked;
    }

    if (bytes_written > 0) {
        stream->position = write_position + (uint64_t)bytes_written;
        if (written_out) {
            *written_out = (uint64_t)bytes_written;
        }
    }
    if ((uint64_t)bytes_written != len) {
        saved_error = EACCES;
        goto fail_locked;
    }

    wasi_p2_file_io_unlock();
    return;

fail_locked:
    wasi_p2_file_io_unlock();
    ret->is_err = true;
    set_resource_stream_error(
        &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, saved_error);
}

void
wasi_p2_stream_resource_write(StreamResourceType *stream,
                              const wasi_list_u8_t *payload, bool blocking,
                              bool flush, wasi_result_void_stream_error_t *ret)
{
    wasi_output_stream_t fd;
    FlushingStreamReservation reservation = { 0 };

    if (!ret) {
        return;
    }
    if (!stream) {
        memset(ret, 0, sizeof(*ret));
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    fd = (wasi_output_stream_t)stream->fd;
    if (flush && !prepare_flushing_stream_reservation(fd, &reservation, ret)) {
        return;
    }
    if (stream->type == STREAM_TYPE_FILE) {
        file_stream_write(stream, payload, NULL, ret);
        if (!ret->is_err && flush) {
            if (blocking) {
                output_stream_blocking_flush_reserved(fd, &reservation, ret);
            }
            else {
                output_stream_flush_reserved(fd, &reservation, ret);
            }
        }
        goto cleanup;
    }

    if (blocking && flush) {
        output_stream_blocking_write_and_flush_reserved(fd, payload,
                                                        &reservation, ret);
        goto cleanup;
    }

    wasi_output_stream_write(fd, payload, ret);
    if (!ret->is_err && flush) {
        if (blocking) {
            output_stream_blocking_flush_reserved(fd, &reservation, ret);
        }
        else {
            output_stream_flush_reserved(fd, &reservation, ret);
        }
    }

cleanup:
    release_flushing_stream_reservation(&reservation);
}

static void
file_stream_write_zeroes(StreamResourceType *stream, uint64_t len,
                         wasi_result_void_stream_error_t *ret)
{
    const uint8_t zeroes[4096] = { 0 };
    uint64_t write_position;
    uint64_t remaining = len;
    off_t native_write_position;
    int fd = -1;
    int saved_error = 0;

    memset(ret, 0, sizeof(*ret));
    wasi_p2_file_io_lock();
    if (!file_stream_validate_locked(stream, true, &fd)) {
        saved_error = errno;
        goto fail_locked;
    }
    if (len == 0) {
        wasi_p2_file_io_unlock();
        return;
    }
    if (!file_stream_write_position_locked(stream, fd, len, &write_position,
                                           &native_write_position)) {
        saved_error = errno;
        goto fail_locked;
    }

    while (remaining > 0) {
        size_t write_len =
            remaining < sizeof(zeroes) ? (size_t)remaining : sizeof(zeroes);
        ssize_t bytes_written;

        do {
            bytes_written =
                pwrite(fd, zeroes, write_len, native_write_position);
        } while (bytes_written < 0 && errno == EINTR);
        if (bytes_written < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (bytes_written > 0) {
            uint64_t advanced = (uint64_t)bytes_written;
            write_position += advanced;
            native_write_position += (off_t)bytes_written;
            stream->position = write_position;
            remaining -= advanced;
        }
        if ((size_t)bytes_written != write_len) {
            saved_error = EACCES;
            goto fail_locked;
        }
    }

    wasi_p2_file_io_unlock();
    return;

fail_locked:
    wasi_p2_file_io_unlock();
    ret->is_err = true;
    set_resource_stream_error(
        &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, saved_error);
}

void
wasi_p2_stream_resource_write_zeroes(StreamResourceType *stream, uint64_t len,
                                     bool blocking, bool flush,
                                     wasi_result_void_stream_error_t *ret)
{
    wasi_output_stream_t fd;
    FlushingStreamReservation reservation = { 0 };

    if (!ret) {
        return;
    }
    if (!stream) {
        memset(ret, 0, sizeof(*ret));
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    fd = (wasi_output_stream_t)stream->fd;
    if (flush && !prepare_flushing_stream_reservation(fd, &reservation, ret)) {
        return;
    }
    if (stream->type == STREAM_TYPE_FILE) {
        file_stream_write_zeroes(stream, len, ret);
        if (!ret->is_err && flush) {
            if (blocking) {
                output_stream_blocking_flush_reserved(fd, &reservation, ret);
            }
            else {
                output_stream_flush_reserved(fd, &reservation, ret);
            }
        }
        goto cleanup;
    }

    if (blocking && flush) {
        output_stream_blocking_write_zeroes_and_flush_reserved(
            fd, len, &reservation, ret);
        goto cleanup;
    }
    wasi_output_stream_write_zeroes(fd, len, ret);
    if (!ret->is_err && flush) {
        if (blocking) {
            output_stream_blocking_flush_reserved(fd, &reservation, ret);
        }
        else {
            output_stream_flush_reserved(fd, &reservation, ret);
        }
    }

cleanup:
    release_flushing_stream_reservation(&reservation);
}

static void
file_streams_splice(StreamResourceType *stream, StreamResourceType *src,
                    uint64_t len, bool blocking,
                    wasi_result_u64_stream_error_t *ret)
{
    uint8_t buf[4096];
    struct stat source_stat;
    uint64_t source_position;
    uint64_t write_position;
    uint64_t transfer_len;
    uint64_t total_spliced = 0;
    off_t native_source_position;
    off_t native_write_position;
    int source_fd = -1;
    int stream_fd = -1;
    int saved_error = 0;

    memset(ret, 0, sizeof(*ret));
    wasi_p2_file_io_lock();
    if (stream == src) {
        saved_error = EINVAL;
        goto fail_locked;
    }
    if (!file_stream_validate_locked(src, false, &source_fd)
        || !file_stream_validate_locked(stream, true, &stream_fd)) {
        saved_error = errno;
        goto fail_locked;
    }
    if (len == 0) {
        wasi_p2_file_io_unlock();
        return;
    }
    if (fstat(source_fd, &source_stat) < 0) {
        saved_error = errno;
        goto fail_locked;
    }
    if (source_stat.st_size < 0
        || (uint64_t)source_stat.st_size > (uint64_t)INT64_MAX) {
        saved_error = EOVERFLOW;
        goto fail_locked;
    }
    source_position = src->position;
    if (source_position >= (uint64_t)source_stat.st_size) {
        wasi_p2_file_io_unlock();
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED,
                                  0);
        return;
    }

    transfer_len = (uint64_t)source_stat.st_size - source_position;
    if (transfer_len > len) {
        transfer_len = len;
    }
    if (!blocking && transfer_len > sizeof(buf)) {
        transfer_len = sizeof(buf);
    }
    if (!file_stream_native_range(source_position, transfer_len,
                                  &native_source_position)
        || !file_stream_write_position_locked(stream, stream_fd, transfer_len,
                                              &write_position,
                                              &native_write_position)) {
        saved_error = errno;
        goto fail_locked;
    }

    while (total_spliced < transfer_len) {
        uint64_t remaining = transfer_len - total_spliced;
        size_t read_len =
            remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        ssize_t bytes_read;
        ssize_t bytes_written;

        do {
            bytes_read =
                pread(source_fd, buf, read_len, native_source_position);
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (bytes_read == 0) {
            break;
        }

        do {
            bytes_written = pwrite(stream_fd, buf, (size_t)bytes_read,
                                   native_write_position);
        } while (bytes_written < 0 && errno == EINTR);
        if (bytes_written < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (bytes_written > 0) {
            uint64_t advanced = (uint64_t)bytes_written;
            source_position += advanced;
            write_position += advanced;
            native_source_position += (off_t)bytes_written;
            native_write_position += (off_t)bytes_written;
            src->position = source_position;
            stream->position = write_position;
            total_spliced += advanced;
        }
        if (bytes_written != bytes_read) {
            saved_error = EACCES;
            goto fail_locked;
        }
    }

    wasi_p2_file_io_unlock();
    if (total_spliced == 0 && len > 0) {
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err, WASI_STREAM_ERROR_KIND_CLOSED,
                                  0);
        return;
    }
    ret->u.ok = total_spliced;
    return;

fail_locked:
    wasi_p2_file_io_unlock();
    ret->is_err = true;
    set_resource_stream_error(
        &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, saved_error);
}

static void
file_stream_to_fd_splice(wasi_output_stream_t stream, StreamResourceType *src,
                         uint64_t len, bool blocking,
                         wasi_result_u64_stream_error_t *ret)
{
    uint8_t buf[4096];
    uint64_t total_spliced = 0;
    int source_fd = -1;

    memset(ret, 0, sizeof(*ret));
    wasi_p2_file_io_lock();
    if (!file_stream_validate_locked(src, false, &source_fd)) {
        int saved_error = errno;
        wasi_p2_file_io_unlock();
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err,
                                  WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                                  saved_error);
        return;
    }
    wasi_p2_file_io_unlock();

    while (total_spliced < len) {
        wasi_result_u64_stream_error_t check_result = { 0 };
        uint64_t remaining = len - total_spliced;
        uint64_t permitted;
        size_t read_len;
        struct stat source_stat;
        off_t native_source_position;
        ssize_t bytes_read;
        ssize_t bytes_written;
        int saved_error = 0;

        do {
            if (blocking) {
                wasi_pollable_context_t pollable = {
                    .fd = stream,
                    .own_fd = false,
                    .type = WASI_POLLABLE_OUT,
                    .host_context = NULL,
                };
                wasi_pollable_block(&pollable);
            }
            else {
                struct pollfd pfd = { .fd = stream, .events = POLLOUT };
                int poll_result;

                do {
                    poll_result = poll(&pfd, 1, 0);
                } while (poll_result < 0 && errno == EINTR);
                if (poll_result < 0) {
                    ret->is_err = true;
                    set_resource_stream_error(
                        &ret->u.err,
                        WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, errno);
                    return;
                }
                if (poll_result == 0 || !(pfd.revents & POLLOUT)) {
                    ret->u.ok = total_spliced;
                    return;
                }
            }
            wasi_output_stream_check_write(stream, &check_result);
            if (check_result.is_err) {
                ret->is_err = true;
                ret->u.err = check_result.u.err;
                return;
            }
        } while (blocking && check_result.u.ok == 0);
        if (check_result.u.ok == 0) {
            break;
        }

        permitted = check_result.u.ok;
        if (permitted > remaining) {
            permitted = remaining;
        }
        if (permitted > sizeof(buf)) {
            permitted = sizeof(buf);
        }
        read_len = (size_t)permitted;

        wasi_p2_file_io_lock();
        if (!file_stream_validate_locked(src, false, &source_fd)) {
            saved_error = errno;
            goto fail_locked;
        }
        if (fstat(source_fd, &source_stat) < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (source_stat.st_size < 0
            || (uint64_t)source_stat.st_size > (uint64_t)INT64_MAX) {
            saved_error = EOVERFLOW;
            goto fail_locked;
        }
        if (src->position >= (uint64_t)source_stat.st_size) {
            wasi_p2_file_io_unlock();
            if (total_spliced == 0 && len > 0) {
                ret->is_err = true;
                set_resource_stream_error(&ret->u.err,
                                          WASI_STREAM_ERROR_KIND_CLOSED, 0);
            }
            break;
        }
        if (read_len > (uint64_t)source_stat.st_size - src->position) {
            read_len = (size_t)((uint64_t)source_stat.st_size - src->position);
        }
        if (!file_stream_native_range(src->position, read_len,
                                      &native_source_position)) {
            saved_error = errno;
            goto fail_locked;
        }

        do {
            bytes_read =
                pread(source_fd, buf, read_len, native_source_position);
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (bytes_read == 0) {
            wasi_p2_file_io_unlock();
            if (total_spliced == 0 && len > 0) {
                ret->is_err = true;
                set_resource_stream_error(&ret->u.err,
                                          WASI_STREAM_ERROR_KIND_CLOSED, 0);
            }
            break;
        }

        do {
            bytes_written = write(stream, buf, (size_t)bytes_read);
        } while (bytes_written < 0 && errno == EINTR);
        if (bytes_written < 0) {
            saved_error = errno;
            goto fail_locked;
        }
        if (bytes_written > 0) {
            uint64_t advanced = (uint64_t)bytes_written;
            src->position += advanced;
            total_spliced += advanced;
        }
        if (bytes_written != bytes_read) {
            wasi_p2_file_io_unlock();
            ret->is_err = true;
            set_resource_stream_error(
                &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                EACCES);
            return;
        }
        wasi_p2_file_io_unlock();

        if (!blocking) {
            break;
        }
        continue;

    fail_locked:
        wasi_p2_file_io_unlock();
        ret->is_err = true;
        set_resource_stream_error(&ret->u.err,
                                  WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                                  saved_error);
        return;
    }

    if (!ret->is_err) {
        ret->u.ok = total_spliced;
    }
}

static void
fd_to_file_stream_splice(StreamResourceType *stream, wasi_input_stream_t src,
                         uint64_t len, bool blocking,
                         wasi_result_u64_stream_error_t *ret)
{
    uint8_t buf[4096];
    wasi_list_u8_t empty_payload = { 0 };
    wasi_result_void_stream_error_t write_result = { 0 };
    uint64_t total_spliced = 0;

    memset(ret, 0, sizeof(*ret));
    file_stream_write(stream, &empty_payload, NULL, &write_result);
    if (write_result.is_err) {
        ret->is_err = true;
        ret->u.err = write_result.u.err;
        return;
    }

    while (total_spliced < len) {
        uint64_t remaining = len - total_spliced;
        size_t read_len =
            remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        ssize_t bytes_read;
        uint64_t bytes_written = 0;

        {
            struct pollfd pfd = { .fd = src, .events = POLLIN };
            int poll_result;
            int timeout = blocking ? -1 : 0;

            do {
                poll_result = poll(&pfd, 1, timeout);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result < 0) {
                ret->is_err = true;
                set_resource_stream_error(
                    &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                    errno);
                return;
            }
            if (poll_result == 0 || !(pfd.revents & (POLLIN | POLLHUP))) {
                if (!blocking) {
                    break;
                }
                continue;
            }
        }

        do {
            bytes_read = read(src, buf, read_len);
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read < 0) {
            if (!blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            ret->is_err = true;
            set_resource_stream_error(
                &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                errno);
            return;
        }
        if (bytes_read == 0) {
            if (total_spliced == 0 && len > 0) {
                ret->is_err = true;
                set_resource_stream_error(&ret->u.err,
                                          WASI_STREAM_ERROR_KIND_CLOSED, 0);
            }
            break;
        }

        wasi_list_u8_t payload = {
            .buf = buf,
            .buf_len = (uint64_t)bytes_read,
        };
        file_stream_write(stream, &payload, &bytes_written, &write_result);
        total_spliced += bytes_written;
        if (write_result.is_err) {
            ret->is_err = true;
            ret->u.err = write_result.u.err;
            return;
        }
        if (!blocking) {
            break;
        }
    }

    if (!ret->is_err) {
        ret->u.ok = total_spliced;
    }
}

void
wasi_p2_stream_resources_splice(StreamResourceType *stream,
                                StreamResourceType *src, uint64_t len,
                                bool blocking,
                                wasi_result_u64_stream_error_t *ret)
{
    if (!ret) {
        return;
    }
    if (!stream || !src) {
        memset(ret, 0, sizeof(*ret));
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (stream->type == STREAM_TYPE_FILE && src->type == STREAM_TYPE_FILE) {
        file_streams_splice(stream, src, len, blocking, ret);
        return;
    }
    if (stream->type == STREAM_TYPE_FILE) {
        if (src->fd > INT_MAX) {
            memset(ret, 0, sizeof(*ret));
            ret->is_err = true;
            set_resource_stream_error(
                &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                EINVAL);
            return;
        }
        fd_to_file_stream_splice(stream, (wasi_input_stream_t)src->fd, len,
                                 blocking, ret);
        return;
    }
    if (src->type == STREAM_TYPE_FILE) {
        if (stream->fd > INT_MAX) {
            memset(ret, 0, sizeof(*ret));
            ret->is_err = true;
            set_resource_stream_error(
                &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                EINVAL);
            return;
        }
        file_stream_to_fd_splice((wasi_output_stream_t)stream->fd, src, len,
                                 blocking, ret);
        return;
    }

    if (blocking) {
        wasi_output_stream_blocking_splice((wasi_output_stream_t)stream->fd,
                                           (wasi_input_stream_t)src->fd, len,
                                           ret);
    }
    else {
        wasi_output_stream_splice((wasi_output_stream_t)stream->fd,
                                  (wasi_input_stream_t)src->fd, len, ret);
    }
}

void
wasi_p2_callback_input_stream_to_resource_splice(
    HostResource *src, StreamResourceType *stream, uint64_t len, bool blocking,
    wasi_result_u64_stream_error_t *ret)
{
    wasi_list_u8_t empty_payload = { 0 };
    wasi_result_void_stream_error_t write_result = { 0 };
    uint64_t total_spliced = 0;
    uint64_t bytes_written = 0;
    uint32_t scratch_capacity = 0;
    uint8_t *scratch = NULL;

    if (!ret) {
        return;
    }
    memset(ret, 0, sizeof(*ret));
    if (!wasi_p2_is_callback_input_stream(src) || !stream
        || stream->type != STREAM_TYPE_FILE) {
        ret->is_err = true;
        set_resource_stream_error(
            &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED, EINVAL);
        return;
    }
    if (len > 0) {
        scratch_capacity =
            len < WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES
                ? (uint32_t)len
                : WASM_COMPONENT_WASI_INPUT_STREAM_MAX_CALLBACK_BYTES;
        scratch = wasm_runtime_malloc(scratch_capacity);
        if (!scratch) {
            ret->is_err = true;
            set_resource_stream_error(
                &ret->u.err, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED,
                ENOMEM);
            return;
        }
    }

    file_stream_write(stream, &empty_payload, NULL, &write_result);
    if (write_result.is_err) {
        ret->is_err = true;
        ret->u.err = write_result.u.err;
        goto done;
    }

    while (total_spliced < len) {
        wasi_result_list_u8_stream_error_t read_result = { 0 };
        uint64_t remaining = len - total_spliced;
        uint64_t read_len = remaining < 4096 ? remaining : 4096;

        if (read_len > scratch_capacity) {
            read_len = scratch_capacity;
        }
        wasi_p2_callback_input_stream_read_into(src, read_len, scratch,
                                                scratch_capacity, &read_result);
        if (read_result.is_err) {
            if (read_result.u.err.kind == WASI_STREAM_ERROR_KIND_CLOSED
                && total_spliced > 0) {
                break;
            }
            ret->is_err = true;
            ret->u.err = read_result.u.err;
            goto done;
        }

        bytes_written = 0;
        file_stream_write(stream, &read_result.u.ok, &bytes_written,
                          &write_result);
        total_spliced += bytes_written;
        if (write_result.is_err) {
            ret->is_err = true;
            ret->u.err = write_result.u.err;
            goto done;
        }
        if (!blocking) {
            break;
        }
    }

    ret->u.ok = total_spliced;

done:
    wasm_runtime_free(scratch);
}
