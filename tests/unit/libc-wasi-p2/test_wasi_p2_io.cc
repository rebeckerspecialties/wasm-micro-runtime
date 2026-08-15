/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <chrono>
#include <future>
#include <thread>
#include <sys/socket.h>

extern "C" {
#include "wasi_p2_io.h"
#include "wasi_p2_common.h"
#include "wasi_p2_error.h"
#include "component-model/wasm_component_host_resource.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
}

struct IoQuotaState {
    uint64_t live_bytes = 0;
    uint32_t live_allocations = 0;
    uint32_t attachment_retain_calls = 0;
    uint32_t attachment_release_calls = 0;
};

class FileSizeLimitGuard
{
  public:
    bool Activate(rlim_t limit)
    {
        if (getrlimit(RLIMIT_FSIZE, &old_limit_) != 0
            || (old_limit_.rlim_max != RLIM_INFINITY
                && old_limit_.rlim_max < limit)) {
            return false;
        }
        previous_handler_ = signal(SIGXFSZ, SIG_IGN);
        if (previous_handler_ == SIG_ERR) {
            return false;
        }
        struct rlimit partial_limit = old_limit_;
        partial_limit.rlim_cur = limit;
        if (setrlimit(RLIMIT_FSIZE, &partial_limit) != 0) {
            signal(SIGXFSZ, previous_handler_);
            previous_handler_ = SIG_ERR;
            return false;
        }
        active_ = true;
        return true;
    }

    bool Restore()
    {
        bool restored = true;

        if (!active_) {
            return true;
        }
        restored = setrlimit(RLIMIT_FSIZE, &old_limit_) == 0;
        restored = signal(SIGXFSZ, previous_handler_) != SIG_ERR && restored;
        active_ = !restored;
        return restored;
    }

    ~FileSizeLimitGuard() { Restore(); }

  private:
    struct rlimit old_limit_ = {};
    void (*previous_handler_)(int) = SIG_ERR;
    bool active_ = false;
};

struct IoCallbackSpliceState {
    const uint8_t *bytes = nullptr;
    uint32_t length = 0;
    uint32_t position = 0;
    uint32_t max_per_read = UINT32_MAX;
    uint32_t successful_reads = 0;
    bool fail_after_first_read = false;
};

/* Mirrors the private callback-stream payload prefix used by wasi_p2_host_io.c
 * so this unit can exercise the HostResource-aware splice seam directly. */
struct IoCallbackStreamData {
    IOStreamType type = STREAM_TYPE_HOST_CALLBACK;
    wasm_component_wasi_input_stream_callbacks_t callbacks = {};
    void *attachment = nullptr;
    bool dropped = false;
};

static wasm_component_wasi_input_stream_status_t
io_callback_splice_read(void *attachment, uint8_t *buffer, uint32_t capacity,
                        uint32_t *out_bytes_read)
{
    auto *state = static_cast<IoCallbackSpliceState *>(attachment);

    *out_bytes_read = 0;
    if (state->fail_after_first_read && state->successful_reads > 0) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
    }
    if (state->position >= state->length) {
        return WASM_COMPONENT_WASI_INPUT_STREAM_CLOSED;
    }

    uint32_t remaining = state->length - state->position;
    uint32_t count = remaining < capacity ? remaining : capacity;
    if (count > state->max_per_read) {
        count = state->max_per_read;
    }
    memcpy(buffer, state->bytes + state->position, count);
    state->position += count;
    state->successful_reads++;
    *out_bytes_read = count;
    return WASM_COMPONENT_WASI_INPUT_STREAM_OK;
}

static wasm_component_wasi_input_stream_status_t
io_callback_splice_skip(void *, uint64_t, uint64_t *out_bytes_skipped)
{
    *out_bytes_skipped = 0;
    return WASM_COMPONENT_WASI_INPUT_STREAM_FAILED;
}

static void
io_callback_splice_drop(void *)
{
}

static bool
io_quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<IoQuotaState *>(attachment);

    if (!state || bytes > UINT64_MAX - state->live_bytes
        || allocations > UINT32_MAX - state->live_allocations) {
        return false;
    }
    state->live_bytes += bytes;
    state->live_allocations += allocations;
    return true;
}

static void
io_quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<IoQuotaState *>(attachment);

    if (!state || bytes > state->live_bytes
        || allocations > state->live_allocations) {
        return;
    }
    state->live_bytes -= bytes;
    state->live_allocations -= allocations;
}

static bool
io_quota_attachment_retain(void *attachment)
{
    auto *state = static_cast<IoQuotaState *>(attachment);

    if (!state) {
        return false;
    }
    state->attachment_retain_calls++;
    return true;
}

static void
io_quota_attachment_release(void *attachment)
{
    auto *state = static_cast<IoQuotaState *>(attachment);

    if (state) {
        state->attachment_release_calls++;
    }
}

// Test fixture for wasi:io tests.
class WasiP2IoTest : public testing::Test {
protected:
    int pipe_fds[2] = {-1, -1};
    wasi_pollable_context_t in_pollable, out_pollable;

    void SetUp() override {
        wasm_runtime_init();
        if (pipe(pipe_fds) == -1) {
            FAIL() << "Failed to create pipe";
        }
        SET_INPUT_POLLABLE(&in_pollable, pipe_fds[0], true);
        SET_OUTPUT_POLLABLE(&out_pollable, pipe_fds[1], true);

        // Instantiate the global host resource table
        bool success = instantiate_host_resource_table();
        ASSERT_TRUE(success);
    }

    void TearDown() override {
        if (pipe_fds[0] != -1) {
            close(pipe_fds[0]);
        }
        if (pipe_fds[1] != -1) {
            close(pipe_fds[1]);
        }
        // Destroy the global host resource table
        destroy_host_resource_table();
        wasm_runtime_destroy();
    }
};

// Test `pollable.ready` and `pollable.block`.
TEST_F(WasiP2IoTest, wasi_pollable_ready_and_block) {
    ASSERT_FALSE(wasi_pollable_ready(&in_pollable));

    char buf[1] = {'a'};
    write(pipe_fds[1], buf, 1);

    ASSERT_TRUE(wasi_pollable_ready(&in_pollable));

    wasi_pollable_block(&in_pollable);
    read(pipe_fds[0], buf, 1);
}

// Test `poll.poll`.
TEST_F(WasiP2IoTest, wasi_poll) {
    int pipe2_fds[2];
    ASSERT_NE(pipe(pipe2_fds), -1);
    wasi_pollable_context_t in_pollable2;
    SET_INPUT_POLLABLE(&in_pollable2, pipe2_fds[0], true);

    const wasi_pollable_context_t* pollables[] = { &in_pollable, &in_pollable2 };
    wasi_list_u32_t ret;

    char buf[1] = {'a'};
    write(pipe2_fds[1], buf, 1);

    wasi_poll(pollables, 2, &ret);

    ASSERT_EQ(ret.len, 1);
    ASSERT_EQ(ret.buf[0], 1);

    wasm_runtime_free(ret.buf);
    close(pipe2_fds[0]);
    close(pipe2_fds[1]);
}

// Test non-blocking read from an input stream.
TEST_F(WasiP2IoTest, wasi_input_stream_read) {
    char write_buf[] = "hello";
    write(pipe_fds[1], write_buf, sizeof(write_buf));

    wasi_result_list_u8_stream_error_t ret;
    wasi_input_stream_read(pipe_fds[0], sizeof(write_buf), &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok.buf_len, sizeof(write_buf));
    ASSERT_EQ(memcmp(ret.u.ok.buf, write_buf, sizeof(write_buf)), 0);

    wasm_runtime_free(ret.u.ok.buf);
}

TEST_F(WasiP2IoTest, wasi_input_stream_read_rejects_oversized_length)
{
    char byte = 'x';
    ASSERT_EQ(write(pipe_fds[1], &byte, 1), 1);

    wasi_result_list_u8_stream_error_t ret = {};
    wasi_input_stream_read(pipe_fds[0], (uint64_t)UINT32_MAX + 1, &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EOVERFLOW);

    char read_byte = 0;
    EXPECT_EQ(read(pipe_fds[0], &read_byte, 1), 1);
    EXPECT_EQ(read_byte, byte);
}

TEST_F(WasiP2IoTest, wasi_input_stream_zero_length_does_not_consume_input)
{
    char byte = 'z';
    ASSERT_EQ(write(pipe_fds[1], &byte, 1), 1);

    wasi_result_list_u8_stream_error_t ret = {};
    wasi_input_stream_read(pipe_fds[0], 0, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok.buf, nullptr);
    EXPECT_EQ(ret.u.ok.buf_len, 0);

    char read_byte = 0;
    EXPECT_EQ(read(pipe_fds[0], &read_byte, 1), 1);
    EXPECT_EQ(read_byte, byte);
}

TEST_F(WasiP2IoTest, wasi_input_stream_zero_length_rejects_write_only_stream)
{
    wasi_result_list_u8_stream_error_t ret = {};

    wasi_input_stream_read(pipe_fds[1], 0, &ret);
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EBADF);

    ret = {};
    wasi_input_stream_blocking_read(pipe_fds[1], 0, &ret);
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EBADF);
}

TEST_F(WasiP2IoTest, wasi_input_stream_would_block_releases_buffer)
{
    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK), 0);

    IoQuotaState quota;
    LoadArgs2 args;
    wasm_runtime_load_args2_init(&args);
    wasm_runtime_load_args2_set_allocation_quota(
        &args, &quota, io_quota_reserve, io_quota_release,
        io_quota_attachment_retain, io_quota_attachment_release);
    WASMAllocationQuotaToken *token = wasm_allocation_quota_token_create(&args);
    ASSERT_NE(token, nullptr);

    uint64_t baseline_bytes = quota.live_bytes;
    uint32_t baseline_allocations = quota.live_allocations;
    WASMAllocationQuotaToken *previous =
        wasm_allocation_quota_set_current(token);
    EXPECT_EQ(previous, nullptr);

    wasi_result_list_u8_stream_error_t ret = {};
    wasi_input_stream_read(pipe_fds[0], 64, &ret);
    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok.buf, nullptr);
    EXPECT_EQ(ret.u.ok.buf_len, 0);
    EXPECT_EQ(quota.live_bytes, baseline_bytes);
    EXPECT_EQ(quota.live_allocations, baseline_allocations);

    EXPECT_EQ(wasm_allocation_quota_set_current(previous), token);
    wasm_allocation_quota_token_release(token);
    EXPECT_EQ(quota.live_bytes, 0);
    EXPECT_EQ(quota.live_allocations, 0);
    EXPECT_EQ(quota.attachment_retain_calls, 1);
    EXPECT_EQ(quota.attachment_release_calls, 1);
}

// Test blocking read from an input stream.
TEST_F(WasiP2IoTest, wasi_input_stream_blocking_read) {
    char write_buf[] = "hello";
    write(pipe_fds[1], write_buf, sizeof(write_buf));

    wasi_result_list_u8_stream_error_t ret;
    wasi_input_stream_blocking_read(pipe_fds[0], sizeof(write_buf), &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok.buf_len, sizeof(write_buf));
    ASSERT_EQ(memcmp(ret.u.ok.buf, write_buf, sizeof(write_buf)), 0);

    wasm_runtime_free(ret.u.ok.buf);
}

TEST_F(WasiP2IoTest, wasi_input_stream_blocking_read_rejects_oversized_length)
{
    char byte = 'b';
    ASSERT_EQ(write(pipe_fds[1], &byte, 1), 1);

    wasi_result_list_u8_stream_error_t ret = {};
    wasi_input_stream_blocking_read(pipe_fds[0], (uint64_t)UINT32_MAX + 1,
                                    &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EOVERFLOW);

    char read_byte = 0;
    EXPECT_EQ(read(pipe_fds[0], &read_byte, 1), 1);
    EXPECT_EQ(read_byte, byte);
}

TEST_F(WasiP2IoTest,
       wasi_input_stream_blocking_zero_length_does_not_consume_input)
{
    char byte = 'q';
    ASSERT_EQ(write(pipe_fds[1], &byte, 1), 1);

    wasi_result_list_u8_stream_error_t ret = {};
    wasi_input_stream_blocking_read(pipe_fds[0], 0, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok.buf, nullptr);
    EXPECT_EQ(ret.u.ok.buf_len, 0);

    char read_byte = 0;
    EXPECT_EQ(read(pipe_fds[0], &read_byte, 1), 1);
    EXPECT_EQ(read_byte, byte);
}

// Test skipping bytes on a seekable input stream.
TEST_F(WasiP2IoTest, wasi_input_stream_skip) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    char write_buf[] = "hello world";
    write(fd, write_buf, sizeof(write_buf));
    lseek(fd, 0, SEEK_SET);

    wasi_result_u64_stream_error_t ret;
    wasi_input_stream_skip(fd, 6, &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, 6);

    char read_buf[6];
    read(fd, read_buf, 5);
    read_buf[5] = 0;
    ASSERT_STREQ(read_buf, "world");

    fclose(tmpf);
}

// Test that `check-write` correctly reports a full socket buffer after a non-blocking flush.
TEST_F(WasiP2IoTest, wasi_output_stream_non_blocking_flush_socket) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    char write_buf[] = "hello";
    wasi_list_u8_t payload = { .buf = (uint8_t *)write_buf, .buf_len = sizeof(write_buf) };
    wasi_result_void_stream_error_t write_ret;
    wasi_output_stream_write(fds[0], &payload, &write_ret);
    ASSERT_FALSE(write_ret.is_err);

    wasi_result_void_stream_error_t flush_ret;
    wasi_output_stream_flush(fds[0], &flush_ret);
    ASSERT_FALSE(flush_ret.is_err);

    wasi_result_u64_stream_error_t check_ret;
    wasi_output_stream_check_write(fds[0], &check_ret);
    ASSERT_FALSE(check_ret.is_err);
    ASSERT_EQ(check_ret.u.ok, 0);

    char read_buf[sizeof(write_buf)];
    read(fds[1], read_buf, sizeof(write_buf));

    // After reading, the socket buffer should be empty and check_write should work again
    // It might take a moment for the kernel to update the buffer status
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    wasi_output_stream_check_write(fds[0], &check_ret);
    ASSERT_FALSE(check_ret.is_err);
    ASSERT_GT(check_ret.u.ok, 0);

    close(fds[0]);
    close(fds[1]);
}

// Test `check-write` on a writable output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_check_write) {
    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_check_write(pipe_fds[1], &ret);
    ASSERT_FALSE(ret.is_err);
    ASSERT_GT(ret.u.ok, 0);
}

// Test the accuracy of `check-write` on a socket.
TEST_F(WasiP2IoTest, wasi_output_stream_check_write_socket_accuracy) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    wasi_result_u64_stream_error_t ret1;
    wasi_output_stream_check_write(fds[0], &ret1);
    ASSERT_FALSE(ret1.is_err);
    ASSERT_GT(ret1.u.ok, 0);

    char write_buf[] = "hello";
    write(fds[0], write_buf, sizeof(write_buf));

    wasi_result_u64_stream_error_t ret2;
    wasi_output_stream_check_write(fds[0], &ret2);
    ASSERT_FALSE(ret2.is_err);
    ASSERT_LT(ret2.u.ok, ret1.u.ok);

    // Set a specific buffer size
    int sndbuf_size = 8192;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));

    wasi_result_u64_stream_error_t ret3;
    wasi_output_stream_check_write(fds[0], &ret3);
    ASSERT_FALSE(ret3.is_err);
    // The kernel usually doubles the value set by setsockopt
    ASSERT_LE(ret3.u.ok, sndbuf_size * 2);

    close(fds[0]);
    close(fds[1]);
}

// Test writing to an output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_write) {
    wasi_list_u8_t payload = { .buf = (uint8_t *)"hello", .buf_len = 5 };
    wasi_result_void_stream_error_t ret;
    wasi_output_stream_write(pipe_fds[1], &payload, &ret);
    ASSERT_FALSE(ret.is_err);

    char read_buf[6];
    read(pipe_fds[0], read_buf, 5);
    read_buf[5] = 0;
    ASSERT_STREQ(read_buf, "hello");
}

TEST_F(WasiP2IoTest, wasi_file_stream_write_is_positional)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    const char initial[] = "0123456789";
    ASSERT_EQ(pwrite(fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    ASSERT_EQ(lseek(fd, 7, SEEK_SET), 7);

    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position = 2;
    stream.position_valid = true;
    uint8_t replacement[] = { 'A', 'B', 'C' };
    wasi_list_u8_t payload = { .buf = replacement,
                               .buf_len = sizeof(replacement) };
    wasi_result_void_stream_error_t ret = {};

    wasi_p2_stream_resource_write(&stream, &payload, false, false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(stream.position, 5);
    EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 7);
    char actual[sizeof(initial)] = {};
    ASSERT_EQ(pread(fd, actual, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    EXPECT_EQ(memcmp(actual, "01ABC56789", sizeof(initial) - 1), 0);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_writes_have_independent_cursors)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    int second_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(second_fd, 0);
    const char initial[] = "........";
    ASSERT_EQ(pwrite(fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);

    StreamResourceType first = {};
    first.type = STREAM_TYPE_FILE;
    first.fd = (uint32_t)fd;
    first.position_valid = true;
    StreamResourceType second = first;
    second.fd = (uint32_t)second_fd;
    second.position = 4;
    uint8_t first_bytes[] = { 'a', 'b' };
    uint8_t second_bytes[] = { 'X', 'Y' };
    wasi_list_u8_t first_payload = { .buf = first_bytes,
                                     .buf_len = sizeof(first_bytes) };
    wasi_list_u8_t second_payload = { .buf = second_bytes,
                                      .buf_len = sizeof(second_bytes) };
    wasi_result_void_stream_error_t first_ret = {};
    wasi_result_void_stream_error_t second_ret = {};

    wasi_p2_stream_resource_write(&first, &first_payload, false, false,
                                  &first_ret);
    wasi_p2_stream_resource_write(&second, &second_payload, false, false,
                                  &second_ret);

    EXPECT_FALSE(first_ret.is_err);
    EXPECT_FALSE(second_ret.is_err);
    EXPECT_EQ(first.position, 2);
    EXPECT_EQ(second.position, 6);
    char actual[sizeof(initial)] = {};
    ASSERT_EQ(pread(fd, actual, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    EXPECT_EQ(memcmp(actual, "ab..XY..", sizeof(initial) - 1), 0);
    close(second_fd);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_append_uses_current_eof)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    const char initial[] = "base";
    ASSERT_EQ(pwrite(fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);

    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position = 1;
    stream.position_valid = true;
    stream.append = true;
    uint8_t first_bytes[] = { '-', 'a' };
    wasi_list_u8_t first_payload = { .buf = first_bytes,
                                     .buf_len = sizeof(first_bytes) };
    wasi_result_void_stream_error_t ret = {};
    wasi_p2_stream_resource_write(&stream, &first_payload, false, false, &ret);
    ASSERT_FALSE(ret.is_err);
    EXPECT_EQ(stream.position, 6);

    const char intervening[] = "gap";
    ASSERT_EQ(pwrite(fd, intervening, sizeof(intervening) - 1, 6),
              (ssize_t)sizeof(intervening) - 1);
    uint8_t second_bytes[] = { '-', 'b' };
    wasi_list_u8_t second_payload = { .buf = second_bytes,
                                      .buf_len = sizeof(second_bytes) };
    ret = {};
    wasi_p2_stream_resource_write(&stream, &second_payload, false, false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(stream.position, 11);
    char actual[12] = {};
    ASSERT_EQ(pread(fd, actual, 11, 0), 11);
    EXPECT_EQ(memcmp(actual, "base-agap-b", 11), 0);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_short_write_advances_cursor)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    FileSizeLimitGuard file_limit;
    if (!file_limit.Activate(4)) {
        fclose(tmpf);
        GTEST_SKIP() << "RLIMIT_FSIZE cannot be adjusted";
    }

    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position_valid = true;
    uint8_t bytes[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    wasi_list_u8_t payload = { .buf = bytes, .buf_len = sizeof(bytes) };
    wasi_result_void_stream_error_t ret = {};
    wasi_p2_stream_resource_write(&stream, &payload, false, false, &ret);

    ASSERT_TRUE(file_limit.Restore());

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EACCES);
    EXPECT_EQ(stream.position, 4);
    struct stat statbuf;
    ASSERT_EQ(fstat(fd, &statbuf), 0);
    EXPECT_EQ(statbuf.st_size, 4);
    char actual[4] = {};
    ASSERT_EQ(pread(fd, actual, sizeof(actual), 0), (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, "abcd", sizeof(actual)), 0);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_write_rejects_offset_overflow)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position = INT64_MAX;
    stream.position_valid = true;
    uint8_t byte = 'x';
    wasi_list_u8_t payload = { .buf = &byte, .buf_len = 1 };
    wasi_result_void_stream_error_t ret = {};

    wasi_p2_stream_resource_write(&stream, &payload, false, false, &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EOVERFLOW);
    EXPECT_EQ(stream.position, (uint64_t)INT64_MAX);
    struct stat statbuf;
    ASSERT_EQ(fstat(fd, &statbuf), 0);
    EXPECT_EQ(statbuf.st_size, 0);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_write_zeroes_is_positional)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    int second_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(second_fd, 0);
    const char initial[] = "ABCDEFGH";
    ASSERT_EQ(pwrite(fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    ASSERT_EQ(lseek(fd, 4, SEEK_SET), 4);

    StreamResourceType first = {};
    first.type = STREAM_TYPE_FILE;
    first.fd = (uint32_t)fd;
    first.position = 1;
    first.position_valid = true;
    StreamResourceType second = first;
    second.fd = (uint32_t)second_fd;
    second.position = 5;
    wasi_result_void_stream_error_t first_ret = {};
    wasi_result_void_stream_error_t second_ret = {};

    wasi_p2_stream_resource_write_zeroes(&first, 2, false, false, &first_ret);
    wasi_p2_stream_resource_write_zeroes(&second, 2, false, false, &second_ret);

    EXPECT_FALSE(first_ret.is_err);
    EXPECT_FALSE(second_ret.is_err);
    EXPECT_EQ(first.position, 3);
    EXPECT_EQ(second.position, 7);
    EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 4);
    const uint8_t expected[] = { 'A', 0, 0, 'D', 'E', 0, 0, 'H' };
    uint8_t actual[sizeof(expected)] = {};
    ASSERT_EQ(pread(fd, actual, sizeof(actual), 0), (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, expected, sizeof(expected)), 0);
    close(second_fd);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_write_zeroes_partial_advances_cursor)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    FileSizeLimitGuard file_limit;
    if (!file_limit.Activate(4)) {
        fclose(tmpf);
        GTEST_SKIP() << "RLIMIT_FSIZE cannot be adjusted";
    }

    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position_valid = true;
    wasi_result_void_stream_error_t ret = {};
    wasi_p2_stream_resource_write_zeroes(&stream, 8, false, false, &ret);

    ASSERT_TRUE(file_limit.Restore());
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EACCES);
    EXPECT_EQ(stream.position, 4);
    struct stat statbuf;
    ASSERT_EQ(fstat(fd, &statbuf), 0);
    EXPECT_EQ(statbuf.st_size, 4);
    uint8_t actual[4] = { 1, 1, 1, 1 };
    ASSERT_EQ(pread(fd, actual, sizeof(actual), 0), (ssize_t)sizeof(actual));
    const uint8_t expected[4] = {};
    EXPECT_EQ(memcmp(actual, expected, sizeof(expected)), 0);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_write_zeroes_rejects_overflow)
{
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);
    StreamResourceType stream = {};
    stream.type = STREAM_TYPE_FILE;
    stream.fd = (uint32_t)fd;
    stream.position = INT64_MAX;
    stream.position_valid = true;
    wasi_result_void_stream_error_t ret = {};

    wasi_p2_stream_resource_write_zeroes(&stream, 1, false, false, &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EOVERFLOW);
    EXPECT_EQ(stream.position, (uint64_t)INT64_MAX);
    fclose(tmpf);
}

TEST_F(WasiP2IoTest, wasi_file_stream_splice_tracks_logical_cursors)
{
    FILE *source = tmpfile();
    FILE *destination = tmpfile();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);
    int source_fd = fileno(source);
    int destination_fd = fileno(destination);
    const char source_bytes[] = "abcdefgh";
    const char destination_bytes[] = "0123456789";
    ASSERT_EQ(pwrite(source_fd, source_bytes, sizeof(source_bytes) - 1, 0),
              (ssize_t)sizeof(source_bytes) - 1);
    ASSERT_EQ(pwrite(destination_fd, destination_bytes,
                     sizeof(destination_bytes) - 1, 0),
              (ssize_t)sizeof(destination_bytes) - 1);
    ASSERT_EQ(lseek(source_fd, 6, SEEK_SET), 6);
    ASSERT_EQ(lseek(destination_fd, 8, SEEK_SET), 8);

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = (uint32_t)source_fd;
    source_stream.position = 2;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position = 3;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};

    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 4,
                                    false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 4);
    EXPECT_EQ(source_stream.position, 6);
    EXPECT_EQ(destination_stream.position, 7);
    EXPECT_EQ(lseek(source_fd, 0, SEEK_CUR), 6);
    EXPECT_EQ(lseek(destination_fd, 0, SEEK_CUR), 8);
    char actual[sizeof(destination_bytes)] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(destination_bytes) - 1, 0),
              (ssize_t)sizeof(destination_bytes) - 1);
    EXPECT_EQ(memcmp(actual, "012cdef789", sizeof(destination_bytes) - 1), 0);
    fclose(source);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_nonblocking_splice_is_bounded)
{
    FILE *source = tmpfile();
    FILE *destination = tmpfile();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);
    int source_fd = fileno(source);
    int destination_fd = fileno(destination);
    uint8_t source_bytes[12288];
    memset(source_bytes, 's', sizeof(source_bytes));
    ASSERT_EQ(pwrite(source_fd, source_bytes, sizeof(source_bytes), 0),
              (ssize_t)sizeof(source_bytes));

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = (uint32_t)source_fd;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};

    wasi_p2_stream_resources_splice(&destination_stream, &source_stream,
                                    sizeof(source_bytes), false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 4096);
    EXPECT_EQ(source_stream.position, 4096);
    EXPECT_EQ(destination_stream.position, 4096);
    struct stat statbuf;
    ASSERT_EQ(fstat(destination_fd, &statbuf), 0);
    EXPECT_EQ(statbuf.st_size, 4096);

    ret = {};
    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 8192,
                                    true, &ret);
    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 8192);
    EXPECT_EQ(source_stream.position, sizeof(source_bytes));
    EXPECT_EQ(destination_stream.position, sizeof(source_bytes));
    ASSERT_EQ(fstat(destination_fd, &statbuf), 0);
    EXPECT_EQ(statbuf.st_size, (off_t)sizeof(source_bytes));
    fclose(source);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_to_generic_splice_stops_at_eof)
{
    FILE *source = tmpfile();
    ASSERT_NE(source, nullptr);
    int source_fd = fileno(source);
    const char source_bytes[] = "abc";
    ASSERT_EQ(pwrite(source_fd, source_bytes, sizeof(source_bytes) - 1, 0),
              (ssize_t)sizeof(source_bytes) - 1);
    ASSERT_EQ(lseek(source_fd, 2, SEEK_SET), 2);

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = (uint32_t)source_fd;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_GENERIC;
    destination_stream.fd = (uint32_t)pipe_fds[1];
    wasi_result_u64_stream_error_t ret = {};

    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 8,
                                    false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 3);
    EXPECT_EQ(source_stream.position, 3);
    EXPECT_EQ(lseek(source_fd, 0, SEEK_CUR), 2);
    char actual[3] = {};
    ASSERT_EQ(read(pipe_fds[0], actual, sizeof(actual)),
              (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, "abc", sizeof(actual)), 0);
    fclose(source);
}

TEST_F(WasiP2IoTest,
       wasi_file_stream_to_full_blocking_pipe_nonblocking_splice_returns_zero)
{
    FILE *source = tmpfile();
    ASSERT_NE(source, nullptr);
    int source_fd = fileno(source);
    const char source_bytes[] = "abcdef";
    ASSERT_EQ(pwrite(source_fd, source_bytes, sizeof(source_bytes) - 1, 0),
              (ssize_t)sizeof(source_bytes) - 1);
    ASSERT_EQ(lseek(source_fd, 4, SEEK_SET), 4);

    int pipe_flags = fcntl(pipe_fds[1], F_GETFL, 0);
    ASSERT_GE(pipe_flags, 0);
    ASSERT_EQ(fcntl(pipe_fds[1], F_SETFL, pipe_flags | O_NONBLOCK), 0);
    uint8_t fill_bytes[4096] = {};
    for (;;) {
        ssize_t written = write(pipe_fds[1], fill_bytes, sizeof(fill_bytes));
        if (written > 0 || (written < 0 && errno == EINTR)) {
            continue;
        }
        ASSERT_EQ(written, -1);
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    for (;;) {
        uint8_t fill_byte = 0;
        ssize_t written = write(pipe_fds[1], &fill_byte, sizeof(fill_byte));
        if (written > 0 || (written < 0 && errno == EINTR)) {
            continue;
        }
        ASSERT_EQ(written, -1);
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    ASSERT_EQ(fcntl(pipe_fds[1], F_SETFL, pipe_flags), 0);
    ASSERT_EQ(fcntl(pipe_fds[1], F_GETFL, 0) & O_NONBLOCK, 0);

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = (uint32_t)source_fd;
    source_stream.position = 1;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_GENERIC;
    destination_stream.fd = (uint32_t)pipe_fds[1];
    wasi_result_u64_stream_error_t ret = {};

    auto splice = std::async(std::launch::async, [&] {
        wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 4,
                                        false, &ret);
    });
    std::future_status status = splice.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready) {
        uint8_t drained[4096];
        EXPECT_GT(read(pipe_fds[0], drained, sizeof(drained)), 0);
    }
    splice.get();

    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 0);
    EXPECT_EQ(source_stream.position, 1);
    EXPECT_EQ(lseek(source_fd, 0, SEEK_CUR), 4);
    fclose(source);
}

TEST_F(WasiP2IoTest,
       wasi_empty_blocking_pipe_to_file_nonblocking_splice_returns_zero)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    int destination_fd = fileno(destination);
    const char initial[] = "........";
    ASSERT_EQ(pwrite(destination_fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    ASSERT_EQ(lseek(destination_fd, 6, SEEK_SET), 6);
    int pipe_flags = fcntl(pipe_fds[0], F_GETFL, 0);
    ASSERT_GE(pipe_flags, 0);
    ASSERT_EQ(pipe_flags & O_NONBLOCK, 0);

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_GENERIC;
    source_stream.fd = (uint32_t)pipe_fds[0];
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position = 2;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};

    auto splice = std::async(std::launch::async, [&] {
        wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 4,
                                        false, &ret);
    });
    std::future_status status = splice.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready) {
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }
    splice.get();

    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 0);
    EXPECT_EQ(destination_stream.position, 2);
    EXPECT_EQ(lseek(destination_fd, 0, SEEK_CUR), 6);
    char actual[sizeof(initial)] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    EXPECT_EQ(memcmp(actual, initial, sizeof(initial) - 1), 0);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_generic_source_splice_is_positional)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    int destination_fd = fileno(destination);
    const char initial[] = "........";
    ASSERT_EQ(pwrite(destination_fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    const char source_bytes[] = "xyz";
    ASSERT_EQ(write(pipe_fds[1], source_bytes, sizeof(source_bytes) - 1),
              (ssize_t)sizeof(source_bytes) - 1);

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_GENERIC;
    source_stream.fd = (uint32_t)pipe_fds[0];
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position = 2;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};

    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 3,
                                    false, &ret);

    EXPECT_FALSE(ret.is_err);
    EXPECT_EQ(ret.u.ok, 3);
    EXPECT_EQ(destination_stream.position, 5);
    char actual[sizeof(initial)] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    EXPECT_EQ(memcmp(actual, "..xyz...", sizeof(initial) - 1), 0);
    fclose(destination);
}

TEST_F(WasiP2IoTest,
       wasi_file_stream_generic_source_partial_write_tracks_consumption)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    int destination_fd = fileno(destination);
    const char source_bytes[] = "abcdefgh";
    ASSERT_EQ(write(pipe_fds[1], source_bytes, sizeof(source_bytes) - 1),
              (ssize_t)sizeof(source_bytes) - 1);
    FileSizeLimitGuard file_limit;
    if (!file_limit.Activate(4)) {
        fclose(destination);
        GTEST_SKIP() << "RLIMIT_FSIZE cannot be adjusted";
    }

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_GENERIC;
    source_stream.fd = (uint32_t)pipe_fds[0];
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};
    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 8,
                                    false, &ret);

    ASSERT_TRUE(file_limit.Restore());
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EACCES);
    EXPECT_EQ(destination_stream.position, 4);
    char actual[4] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(actual), 0),
              (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, "abcd", sizeof(actual)), 0);

    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK), 0);
    char leftover = 0;
    ssize_t leftover_read = read(pipe_fds[0], &leftover, 1);
    int read_error = errno;
    EXPECT_EQ(leftover_read, -1);
    EXPECT_TRUE(read_error == EAGAIN || read_error == EWOULDBLOCK);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_splice_partial_advances_both_cursors)
{
    FILE *source = tmpfile();
    FILE *destination = tmpfile();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);
    int source_fd = fileno(source);
    int destination_fd = fileno(destination);
    const char source_bytes[] = "abcdefgh";
    ASSERT_EQ(pwrite(source_fd, source_bytes, sizeof(source_bytes) - 1, 0),
              (ssize_t)sizeof(source_bytes) - 1);
    FileSizeLimitGuard file_limit;
    if (!file_limit.Activate(4)) {
        fclose(source);
        fclose(destination);
        GTEST_SKIP() << "RLIMIT_FSIZE cannot be adjusted";
    }

    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = (uint32_t)source_fd;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};
    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 8,
                                    false, &ret);

    ASSERT_TRUE(file_limit.Restore());
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EACCES);
    EXPECT_EQ(source_stream.position, 4);
    EXPECT_EQ(destination_stream.position, 4);
    char actual[4] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(actual), 0),
              (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, "abcd", sizeof(actual)), 0);
    fclose(source);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_splice_error_preserves_cursors)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    StreamResourceType source_stream = {};
    source_stream.type = STREAM_TYPE_FILE;
    source_stream.fd = UINT32_MAX;
    source_stream.position = 3;
    source_stream.position_valid = true;
    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)fileno(destination);
    destination_stream.position = 2;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};

    wasi_p2_stream_resources_splice(&destination_stream, &source_stream, 4,
                                    false, &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EINVAL);
    EXPECT_EQ(source_stream.position, 3);
    EXPECT_EQ(destination_stream.position, 2);
    fclose(destination);
}

TEST_F(WasiP2IoTest, wasi_file_stream_callback_splice_keeps_prior_progress)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    int destination_fd = fileno(destination);
    const char initial[] = "........";
    ASSERT_EQ(pwrite(destination_fd, initial, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);

    const uint8_t callback_bytes[] = { 'a', 'b', 'c' };
    IoCallbackSpliceState callback_state;
    callback_state.bytes = callback_bytes;
    callback_state.length = sizeof(callback_bytes);
    callback_state.max_per_read = sizeof(callback_bytes);
    callback_state.fail_after_first_read = true;
    IoCallbackStreamData callback_data;
    callback_data.callbacks.struct_size = sizeof(callback_data.callbacks);
    callback_data.callbacks.read = io_callback_splice_read;
    callback_data.callbacks.skip = io_callback_splice_skip;
    callback_data.callbacks.drop = io_callback_splice_drop;
    callback_data.attachment = &callback_state;
    HostResource callback_resource = {};
    callback_resource.type = WASI_P2_IO_INPUT_STREAM;
    callback_resource.data = &callback_data;

    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position = 2;
    destination_stream.position_valid = true;

    IoQuotaState quota;
    LoadArgs2 args;
    wasm_runtime_load_args2_init(&args);
    wasm_runtime_load_args2_set_allocation_quota(
        &args, &quota, io_quota_reserve, io_quota_release,
        io_quota_attachment_retain, io_quota_attachment_release);
    WASMAllocationQuotaToken *token = wasm_allocation_quota_token_create(&args);
    ASSERT_NE(token, nullptr);
    uint64_t baseline_bytes = quota.live_bytes;
    uint32_t baseline_allocations = quota.live_allocations;
    WASMAllocationQuotaToken *previous =
        wasm_allocation_quota_set_current(token);
    EXPECT_EQ(previous, nullptr);

    wasi_result_u64_stream_error_t ret = {};
    wasi_p2_callback_input_stream_to_resource_splice(
        &callback_resource, &destination_stream, 6, true, &ret);

    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EIO);
    EXPECT_EQ(callback_state.position, 3);
    EXPECT_EQ(callback_state.successful_reads, 1);
    EXPECT_EQ(destination_stream.position, 5);
    EXPECT_EQ(quota.live_bytes, baseline_bytes);
    EXPECT_EQ(quota.live_allocations, baseline_allocations);
    EXPECT_EQ(wasm_allocation_quota_set_current(previous), token);
    wasm_allocation_quota_token_release(token);
    EXPECT_EQ(quota.live_bytes, 0);
    EXPECT_EQ(quota.live_allocations, 0);

    char actual[sizeof(initial)] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(initial) - 1, 0),
              (ssize_t)sizeof(initial) - 1);
    EXPECT_EQ(memcmp(actual, "..abc...", sizeof(initial) - 1), 0);
    fclose(destination);
}

TEST_F(WasiP2IoTest,
       wasi_file_stream_callback_splice_partial_tracks_consumption)
{
    FILE *destination = tmpfile();
    ASSERT_NE(destination, nullptr);
    int destination_fd = fileno(destination);
    FileSizeLimitGuard file_limit;
    if (!file_limit.Activate(4)) {
        fclose(destination);
        GTEST_SKIP() << "RLIMIT_FSIZE cannot be adjusted";
    }

    const uint8_t callback_bytes[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    IoCallbackSpliceState callback_state;
    callback_state.bytes = callback_bytes;
    callback_state.length = sizeof(callback_bytes);
    IoCallbackStreamData callback_data;
    callback_data.callbacks.struct_size = sizeof(callback_data.callbacks);
    callback_data.callbacks.read = io_callback_splice_read;
    callback_data.callbacks.skip = io_callback_splice_skip;
    callback_data.callbacks.drop = io_callback_splice_drop;
    callback_data.attachment = &callback_state;
    HostResource callback_resource = {};
    callback_resource.type = WASI_P2_IO_INPUT_STREAM;
    callback_resource.data = &callback_data;

    StreamResourceType destination_stream = {};
    destination_stream.type = STREAM_TYPE_FILE;
    destination_stream.fd = (uint32_t)destination_fd;
    destination_stream.position_valid = true;
    wasi_result_u64_stream_error_t ret = {};
    wasi_p2_callback_input_stream_to_resource_splice(
        &callback_resource, &destination_stream, sizeof(callback_bytes), false,
        &ret);

    ASSERT_TRUE(file_limit.Restore());
    EXPECT_TRUE(ret.is_err);
    EXPECT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
    EXPECT_EQ(ret.u.err.payload.error, EACCES);
    EXPECT_EQ(callback_state.position, sizeof(callback_bytes));
    EXPECT_EQ(callback_state.successful_reads, 1);
    EXPECT_EQ(destination_stream.position, 4);
    char actual[4] = {};
    ASSERT_EQ(pread(destination_fd, actual, sizeof(actual), 0),
              (ssize_t)sizeof(actual));
    EXPECT_EQ(memcmp(actual, "abcd", sizeof(actual)), 0);
    fclose(destination);
}

// Test that a partial write to a full non-blocking stream fails correctly.
TEST_F(WasiP2IoTest, wasi_output_stream_write_partial)
{
    // Make the pipe non-blocking
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    // Fill the pipe buffer
    const int buf_size = 1024 * 1024;
    char *buf = (char *)wasm_runtime_malloc(buf_size);
    ASSERT_NE(buf, nullptr);
    memset(buf, 'a', buf_size);
    while (write(pipe_fds[1], buf, 1024) > 0)
        ;
    wasm_runtime_free(buf);

    // Try to write more data
    wasi_list_u8_t payload = { .buf = (uint8_t *)"hello", .buf_len = 5 };
    wasi_result_void_stream_error_t ret;
    wasi_output_stream_write(pipe_fds[1], &payload, &ret);

    ASSERT_TRUE(ret.is_err);
    ASSERT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
}

// Test blocking write and flush to an output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_blocking_write_and_flush) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    wasi_list_u8_t payload = { .buf = (uint8_t *)"hello", .buf_len = 5 };
    wasi_result_void_stream_error_t ret;
    wasi_output_stream_blocking_write_and_flush(fd, &payload, &ret);
    ASSERT_FALSE(ret.is_err);

    lseek(fd, 0, SEEK_SET);
    char read_buf[6];
    read(fd, read_buf, 5);
    read_buf[5] = 0;
    ASSERT_STREQ(read_buf, "hello");

    fclose(tmpf);
}

// Test flushing an output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_flush) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    wasi_result_void_stream_error_t ret;
    wasi_output_stream_flush(fd, &ret);
    ASSERT_FALSE(ret.is_err);

    fclose(tmpf);
}

// Test blocking flush on an output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_blocking_flush) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    wasi_result_void_stream_error_t ret;
    wasi_output_stream_blocking_flush(fd, &ret);
    ASSERT_FALSE(ret.is_err);

    fclose(tmpf);
}

// Test writing zeroes to an output stream.
TEST_F(WasiP2IoTest, wasi_output_stream_write_zeroes) {
    wasi_result_void_stream_error_t ret;
    wasi_output_stream_write_zeroes(pipe_fds[1], 5, &ret);
    ASSERT_FALSE(ret.is_err);

    char read_buf[6];
    read(pipe_fds[0], read_buf, 5);
    read_buf[5] = 0;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(read_buf[i], 0);
    }
}

// Test writing zeroes to a file.
TEST_F(WasiP2IoTest, wasi_output_stream_write_zeroes_file) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    char write_buf[] = "hello";
    write(fd, write_buf, sizeof(write_buf));

    wasi_result_void_stream_error_t ret;
    wasi_output_stream_write_zeroes(fd, 10, &ret);
    ASSERT_FALSE(ret.is_err);

    struct stat statbuf;
    fstat(fd, &statbuf);
    ASSERT_EQ(statbuf.st_size, sizeof(write_buf) + 10);

    lseek(fd, sizeof(write_buf), SEEK_SET);
    char read_buf[11];
    read(fd, read_buf, 10);
    read_buf[10] = 0;
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(read_buf[i], 0);
    }

    fclose(tmpf);
}

// Test blocking write zeroes and flush.
TEST_F(WasiP2IoTest, wasi_output_stream_blocking_write_zeroes_and_flush) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int fd = fileno(tmpf);

    wasi_result_void_stream_error_t ret;
    wasi_output_stream_blocking_write_zeroes_and_flush(fd, 5, &ret);
    ASSERT_FALSE(ret.is_err);

    lseek(fd, 0, SEEK_SET);
    char read_buf[6];
    read(fd, read_buf, 5);
    read_buf[5] = 0;
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(read_buf[i], 0);
    }

    fclose(tmpf);
}

// Test reading from an input stream at EOF.
TEST_F(WasiP2IoTest, wasi_input_stream_read_eof)
{
    close(pipe_fds[1]); // Close the write end to simulate EOF

    wasi_result_list_u8_stream_error_t ret;
    wasi_input_stream_read(pipe_fds[0], 10, &ret);

    ASSERT_TRUE(ret.is_err);
    ASSERT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_CLOSED);

    // Second read on a closed stream should return an empty list as well
    wasi_input_stream_read(pipe_fds[0], 10, &ret);
    ASSERT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_CLOSED);
}

// Test that `splice` fails when the destination stream is closed.
TEST_F(WasiP2IoTest, wasi_output_stream_splice_write_error)
{
    int src_pipe_fds[2];
    ASSERT_NE(pipe(src_pipe_fds), -1);

    char write_buf[] = "hello";
    write(src_pipe_fds[1], write_buf, sizeof(write_buf));

    close(pipe_fds[1]); // Close the write end of the destination pipe

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_splice(
        pipe_fds[1], src_pipe_fds[0], sizeof(write_buf), &ret);

    ASSERT_TRUE(ret.is_err);

    close(src_pipe_fds[0]);
    close(src_pipe_fds[1]);
}

// Test splicing a large amount of data.
TEST_F(WasiP2IoTest, wasi_output_stream_splice_large_data)
{
    int fds[2];
    ASSERT_NE(pipe(fds), -1);

    const int large_size = 65536;
    char *large_buf = (char *)wasm_runtime_malloc(large_size);
    ASSERT_NE(large_buf, nullptr);
    for (int i = 0; i < large_size; i++) {
        large_buf[i] = i % 256;
    }

    // Make pipe non-blocking for writing
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    // Write all data to pipe
    int written = 0;
    while (written < large_size) {
        int res = write(fds[1], large_buf + written, large_size - written);
        if (res > 0) {
            written += res;
        }
    }

    // Splice from pipe to a temporary file
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int tmp_fd = fileno(tmpf);

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_blocking_splice(tmp_fd, fds[0], large_size, &ret);
    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, large_size);

    // Verify the content of the temporary file
    lseek(tmp_fd, 0, SEEK_SET);
    char *read_buf = (char *)wasm_runtime_malloc(large_size);
    ASSERT_NE(read_buf, nullptr);
    ASSERT_EQ(read(tmp_fd, read_buf, large_size), large_size);
    ASSERT_EQ(memcmp(large_buf, read_buf, large_size), 0);

    wasm_runtime_free(large_buf);
    wasm_runtime_free(read_buf);
    fclose(tmpf);
    close(fds[0]);
    close(fds[1]);
}

void *
splice_to_pipe_after_delay(void *arg)
{
    int *pipe_fds = (int *)arg;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char write_buf[] = "hello";
    write(pipe_fds[1], write_buf, sizeof(write_buf));
    return NULL;
}

// Test that `blocking-splice` actually blocks.
TEST_F(WasiP2IoTest, wasi_output_stream_blocking_splice_blocks)
{
    pthread_t thread;
    int src_pipe_fds[2];
    ASSERT_NE(pipe(src_pipe_fds), -1);

    pthread_create(&thread, NULL, splice_to_pipe_after_delay, src_pipe_fds);

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_blocking_splice(
        pipe_fds[1], src_pipe_fds[0], 5, &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, 5);

    char read_buf[6];
    read(pipe_fds[0], read_buf, 5);
    read_buf[5] = 0;
    ASSERT_STREQ(read_buf, "hello");

    pthread_join(thread, NULL);
    close(src_pipe_fds[0]);
    close(src_pipe_fds[1]);
}

void *
write_to_pipe_after_delay(void *arg)
{
    int *pipe_fds = (int *)arg;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char write_buf[] = "hello";
    write(pipe_fds[1], write_buf, sizeof(write_buf));
    return NULL;
}

// Test that `blocking-read` actually blocks.
TEST_F(WasiP2IoTest, wasi_input_stream_blocking_read_blocks)
{
    pthread_t thread;
    pthread_create(&thread, NULL, write_to_pipe_after_delay, pipe_fds);

    wasi_result_list_u8_stream_error_t ret;
    wasi_input_stream_blocking_read(pipe_fds[0], 5, &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok.buf_len, 5);
    ASSERT_EQ(memcmp(ret.u.ok.buf, "hello", 5), 0);

    wasm_runtime_free(ret.u.ok.buf);
    pthread_join(thread, NULL);
}

// Test skipping bytes on a non-seekable stream (a pipe).
TEST_F(WasiP2IoTest, wasi_input_stream_skip_non_seekable)
{
    char write_buf[] = "hello world";
    write(pipe_fds[1], write_buf, sizeof(write_buf));

    wasi_result_u64_stream_error_t ret;
    wasi_input_stream_skip(pipe_fds[0], 6, &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, 6);

    char read_buf[6];
    read(pipe_fds[0], read_buf, 5);
    read_buf[5] = 0;
    ASSERT_STREQ(read_buf, "world");
}

// Test that writing to a closed stream fails correctly.
TEST_F(WasiP2IoTest, wasi_output_stream_write_to_closed)
{
    signal(SIGPIPE, SIG_IGN);
    close(pipe_fds[0]);

    wasi_list_u8_t payload = { .buf = (uint8_t *)"hello", .buf_len = 5 };
    wasi_result_void_stream_error_t ret;
    wasi_output_stream_write(pipe_fds[1], &payload, &ret);
    ASSERT_TRUE(ret.is_err);
    ASSERT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);

    // Second write should also fail
    wasi_output_stream_write(pipe_fds[1], &payload, &ret);
    ASSERT_TRUE(ret.is_err);
    ASSERT_EQ(ret.u.err.kind, WASI_STREAM_ERROR_KIND_LAST_OPERATION_FAILED);
}

void *
skip_from_pipe_after_delay(void *arg)
{
    int *pipe_fds = (int *)arg;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char write_buf[] = "hello";
    write(pipe_fds[1], write_buf, sizeof(write_buf));
    return NULL;
}

// Test that `blocking-skip` actually blocks.
TEST_F(WasiP2IoTest, wasi_input_stream_blocking_skip_blocks)
{
    pthread_t thread;
    pthread_create(&thread, NULL, skip_from_pipe_after_delay, pipe_fds);

    wasi_result_u64_stream_error_t ret;
    wasi_input_stream_blocking_skip(pipe_fds[0], 5, &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, 5);

    pthread_join(thread, NULL);
}

// Test splicing data from a file to a pipe.
TEST_F(WasiP2IoTest, wasi_output_stream_splice_file_to_pipe) {
    FILE *tmpf = tmpfile();
    ASSERT_NE(tmpf, nullptr);
    int src_fd = fileno(tmpf);

    char write_buf[] = "hello world";
    write(src_fd, write_buf, sizeof(write_buf));
    lseek(src_fd, 0, SEEK_SET);

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_splice(pipe_fds[1], src_fd, sizeof(write_buf), &ret);

    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, sizeof(write_buf));

    char read_buf[sizeof(write_buf) + 1];
    read(pipe_fds[0], read_buf, sizeof(write_buf));
    read_buf[sizeof(write_buf)] = 0;
    ASSERT_STREQ(read_buf, write_buf);

    fclose(tmpf);
}

// Test splicing data from one file to another.
TEST_F(WasiP2IoTest, wasi_output_stream_blocking_splice_file_to_file) {
    FILE *src_tmpf = tmpfile();
    ASSERT_NE(src_tmpf, nullptr);
    int src_fd = fileno(src_tmpf);

    FILE *dst_tmpf = tmpfile();
    ASSERT_NE(dst_tmpf, nullptr);
    int dst_fd = fileno(dst_tmpf);

    char write_buf[] = "splice file to file";
    write(src_fd, write_buf, sizeof(write_buf));
    lseek(src_fd, 0, SEEK_SET);

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_blocking_splice(dst_fd, src_fd, sizeof(write_buf), &ret);
    ASSERT_FALSE(ret.is_err);
    ASSERT_EQ(ret.u.ok, sizeof(write_buf));

    char read_buf[sizeof(write_buf) + 1];
    lseek(dst_fd, 0, SEEK_SET);
    read(dst_fd, read_buf, sizeof(write_buf));
    read_buf[sizeof(write_buf)] = 0;
    ASSERT_STREQ(read_buf, write_buf);

    fclose(src_tmpf);
    fclose(dst_tmpf);
}

// Test that `splice` fails when the source stream is unreadable.
TEST_F(WasiP2IoTest, wasi_output_stream_splice_read_error) {
    int src_pipe_fds[2];
    ASSERT_NE(pipe(src_pipe_fds), -1);

    // Close the read end of the source pipe to trigger an error
    close(src_pipe_fds[0]);

    wasi_result_u64_stream_error_t ret;
    wasi_output_stream_splice(pipe_fds[1], src_pipe_fds[0], 10, &ret);

    ASSERT_TRUE(ret.is_err);

    close(src_pipe_fds[1]);
}

extern "C" const char *wasi_error_to_debug_string(HostResource *hr_err);

// Test the `wasi_error_to_debug_string` function.
TEST_F(WasiP2IoTest, wasi_error_to_debug_string_test) {
    uint32_t err = wasi_error_new(STREAM_TYPE_SOCKET, WASI_NETWORK_ERROR_CODE_UNKNOWN);
    HostResourceTable *hr_table = get_global_host_resource_table();
    HostResource *hr = host_resource_table_get(hr_table, err);
    const char *debug_str = wasi_error_to_debug_string(hr);
    ASSERT_NE(debug_str, nullptr);
    ASSERT_STREQ(debug_str, "unknown");

    err = wasi_error_new(STREAM_TYPE_SOCKET, WASI_NETWORK_ERROR_CODE_ACCESS_DENIED);
    hr = host_resource_table_get(hr_table, err);
    debug_str = wasi_error_to_debug_string(hr);
    ASSERT_NE(debug_str, nullptr);
    ASSERT_STREQ(debug_str, "access-denied");
}
