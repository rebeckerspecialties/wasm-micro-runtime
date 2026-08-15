/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "component-model/wasm_component_host_resource.h"
#include "component-model/wasm_component_resource.h"
#include "component-model/wasm_component_resource_table.h"
#include "component-model/wasm_component_runtime.h"
#include "wasi_p2_filesystem_quota.h"
#include "wasm_export.h"
}

namespace {

struct NativeFdQuotaState {
    uint32_t limit = 0;
    uint32_t active = 0;
    uint32_t peak = 0;
    uint32_t reserve_calls = 0;
    uint32_t release_calls = 0;
    uint32_t denied_calls = 0;
    int baseline_process_fds = 0;
    int maximum_process_fds = 0;
};

int
count_process_fds()
{
    DIR *directory = opendir("/dev/fd");
    int count = 0;

    if (!directory) {
        return -1;
    }
    while (dirent *entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0
            && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(directory);
    /* Do not count the descriptor used to enumerate /dev/fd itself. */
    return count - 1;
}

bool
reserve_native_fds(void *attachment, uint32_t fd_count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);
    int process_fds = count_process_fds();

    state->reserve_calls++;
    if (process_fds > state->maximum_process_fds) {
        state->maximum_process_fds = process_fds;
    }
    if (state->active > state->limit
        || fd_count > state->limit - state->active) {
        state->denied_calls++;
        return false;
    }
    state->active += fd_count;
    if (state->active > state->peak) {
        state->peak = state->active;
    }
    return true;
}

void
release_native_fds(void *attachment, uint32_t fd_count)
{
    auto *state = static_cast<NativeFdQuotaState *>(attachment);

    ASSERT_GE(state->active, fd_count);
    state->active -= fd_count;
    state->release_calls++;
}

class WasiP2FilesystemFdQuotaTest : public testing::Test
{
  protected:
    NativeFdQuotaState quota_;
    WASMComponentInstance component_ = {};
    WASMExecEnv exec_env_ = {};
    char test_dir_[PATH_MAX] = {};
    int root_fd_ = -1;

    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        ASSERT_TRUE(instantiate_host_resource_table());

        strcpy(test_dir_, "/tmp/wasi_p2_fd_quota.XXXXXX");
        ASSERT_NE(mkdtemp(test_dir_), nullptr);
        root_fd_ = open(test_dir_, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ASSERT_GE(root_fd_, 0);

        component_.instantiation_args.native_fd_quota_attachment = &quota_;
        component_.instantiation_args.native_fd_quota_reserve =
            reserve_native_fds;
        component_.instantiation_args.native_fd_quota_release =
            release_native_fds;
        exec_env_.component_inst = &component_;

        quota_.baseline_process_fds = count_process_fds();
        ASSERT_GE(quota_.baseline_process_fds, 0);
        quota_.maximum_process_fds = quota_.baseline_process_fds;
    }

    void TearDown() override
    {
        EXPECT_EQ(quota_.active, 0u);
        if (root_fd_ >= 0) {
            close(root_fd_);
            root_fd_ = -1;
        }
        destroy_host_resource_table();
        wasm_runtime_destroy();
        if (test_dir_[0] != '\0') {
            std::filesystem::remove_all(test_dir_);
        }
    }

    void ResetQuota(uint32_t limit)
    {
        ASSERT_EQ(quota_.active, 0u);
        quota_.limit = limit;
        quota_.peak = 0;
        quota_.reserve_calls = 0;
        quota_.release_calls = 0;
        quota_.denied_calls = 0;
        quota_.baseline_process_fds = count_process_fds();
        ASSERT_GE(quota_.baseline_process_fds, 0);
        quota_.maximum_process_fds = quota_.baseline_process_fds;
    }

    void CreateFile(const char *path)
    {
        int fd = openat(root_fd_, path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        ASSERT_GE(fd, 0) << strerror(errno);
        ASSERT_EQ(close(fd), 0);
    }

    void StatPath(const char *path, int *error)
    {
        wasi_descriptor_stat_t stat = {};
        wasi_filesystem_stat_at_with_fd_quota(&exec_env_, root_fd_, 0, path,
                                              &stat, error);
    }

    void CloseMetered(wasi_descriptor_t *fd, WasiP2NativeFdQuotaLease *lease)
    {
        ASSERT_NE(fd, nullptr);
        ASSERT_NE(lease, nullptr);
        if (*fd >= 0) {
            EXPECT_EQ(wasi_filesystem_close(*fd), WASI_ERROR_CODE_SUCCESS);
            *fd = -1;
        }
        wasi_p2_native_fd_quota_release(lease);
    }
};

TEST_F(WasiP2FilesystemFdQuotaTest, CapOneAllowsDotButDeniesNestedWalk)
{
    ASSERT_EQ(mkdirat(root_fd_, "nested", 0755), 0);
    CreateFile("nested/file");

    ResetQuota(1);
    int error = -1;
    StatPath(".", &error);
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.peak, 1u);
    EXPECT_EQ(quota_.active, 0u);

    ResetQuota(1);
    StatPath("nested/file", &error);
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(quota_.peak, 1u);
    EXPECT_EQ(quota_.denied_calls, 1u);
    EXPECT_EQ(quota_.active, 0u);
}

TEST_F(WasiP2FilesystemFdQuotaTest, NestedPopAndFailureRefundForReopen)
{
    ASSERT_EQ(mkdirat(root_fd_, "nested", 0755), 0);
    CreateFile("root-file");
    CreateFile("nested/file");

    ResetQuota(2);
    int error = -1;
    StatPath("nested/../root-file", &error);
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.peak, 2u);
    EXPECT_EQ(quota_.active, 0u);

    StatPath("nested/file", &error);
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 0u);

    ResetQuota(1);
    StatPath("nested/file", &error);
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(quota_.active, 0u);

    ResetQuota(2);
    StatPath("nested/file", &error);
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 0u);
}

TEST_F(WasiP2FilesystemFdQuotaTest,
       PersistentOpenAtDeniesNPlusOneThenDropAllowsReopen)
{
    CreateFile("file");
    ResetQuota(3);

    wasi_descriptor_t first = -1;
    wasi_descriptor_t second = -1;
    wasi_descriptor_t denied = -1;
    WasiP2NativeFdQuotaLease first_lease = {};
    WasiP2NativeFdQuotaLease second_lease = {};
    WasiP2NativeFdQuotaLease denied_lease = {};
    int error = -1;

    wasi_filesystem_open_at_with_fd_quota(&exec_env_, root_fd_, 0, "file", 0,
                                          WASI_DESCRIPTOR_FLAGS_READ, 0, &first,
                                          &first_lease, &error);
    ASSERT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 1u);

    wasi_filesystem_open_at_with_fd_quota(&exec_env_, root_fd_, 0, "file", 0,
                                          WASI_DESCRIPTOR_FLAGS_READ, 0,
                                          &second, &second_lease, &error);
    ASSERT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 2u);

    wasi_filesystem_open_at_with_fd_quota(&exec_env_, root_fd_, 0, "file", 0,
                                          WASI_DESCRIPTOR_FLAGS_READ, 0,
                                          &denied, &denied_lease, &error);
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(denied, -1);
    EXPECT_EQ(quota_.active, 2u);
    EXPECT_EQ(quota_.denied_calls, 1u);

    CloseMetered(&first, &first_lease);
    EXPECT_EQ(quota_.active, 1u);
    wasi_filesystem_open_at_with_fd_quota(&exec_env_, root_fd_, 0, "file", 0,
                                          WASI_DESCRIPTOR_FLAGS_READ, 0,
                                          &denied, &denied_lease, &error);
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_GE(denied, 0);
    EXPECT_EQ(quota_.active, 2u);

    CloseMetered(&denied, &denied_lease);
    CloseMetered(&second, &second_lease);
}

TEST_F(WasiP2FilesystemFdQuotaTest, TwoPathOperationsRollbackBothReservations)
{
    CreateFile("source");
    ResetQuota(1);
    struct stat stat = {};

    int error = wasi_filesystem_link_at_with_fd_quota(
        &exec_env_, root_fd_, 0, "source", root_fd_, "linked");
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(quota_.denied_calls, 1u);
    int stat_result = fstatat(root_fd_, "linked", &stat, AT_SYMLINK_NOFOLLOW);
    int stat_error = errno;
    EXPECT_EQ(stat_result, -1);
    EXPECT_EQ(stat_error, ENOENT);

    error = wasi_filesystem_rename_at_with_fd_quota(
        &exec_env_, root_fd_, "source", root_fd_, "renamed");
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(quota_.denied_calls, 2u);
    EXPECT_EQ(fstatat(root_fd_, "source", &stat, AT_SYMLINK_NOFOLLOW), 0);
    stat_result = fstatat(root_fd_, "renamed", &stat, AT_SYMLINK_NOFOLLOW);
    stat_error = errno;
    EXPECT_EQ(stat_result, -1);
    EXPECT_EQ(stat_error, ENOENT);

    ResetQuota(2);
    error = wasi_filesystem_link_at_with_fd_quota(&exec_env_, root_fd_, 0,
                                                  "source", root_fd_, "linked");
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 0u);
    error = wasi_filesystem_rename_at_with_fd_quota(
        &exec_env_, root_fd_, "linked", root_fd_, "renamed");
    EXPECT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    EXPECT_EQ(quota_.active, 0u);
}

TEST_F(WasiP2FilesystemFdQuotaTest, DeepPathKeepsKernelFdDeltaWithinQuota)
{
    std::string path;
    int parent = dup(root_fd_);
    ASSERT_GE(parent, 0);
    for (int depth = 0; depth < 32; depth++) {
        std::string segment = "d" + std::to_string(depth);
        ASSERT_EQ(mkdirat(parent, segment.c_str(), 0755), 0);
        int child =
            openat(parent, segment.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ASSERT_GE(child, 0);
        close(parent);
        parent = child;
        if (!path.empty()) {
            path += '/';
        }
        path += segment;
    }
    int leaf = openat(parent, "file", O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    ASSERT_GE(leaf, 0);
    close(leaf);
    close(parent);
    path += "/file";

    ResetQuota(3);
    int error = -1;
    StatPath(path.c_str(), &error);
    EXPECT_EQ(error, EDQUOT);
    EXPECT_EQ(quota_.peak, 3u);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_LE(quota_.maximum_process_fds - quota_.baseline_process_fds, 3);
    EXPECT_EQ(count_process_fds(), quota_.baseline_process_fds);
}

TEST_F(WasiP2FilesystemFdQuotaTest,
       ComponentTeardownClosesFinalDescriptorAndRefundsQuota)
{
    CreateFile("file");
    ResetQuota(2);
    int process_fds_before = count_process_fds();

    wasi_descriptor_t fd = -1;
    WasiP2NativeFdQuotaLease fd_lease = {};
    int error = -1;
    wasi_filesystem_open_at_with_fd_quota(&exec_env_, root_fd_, 0, "file", 0,
                                          WASI_DESCRIPTOR_FLAGS_READ, 0, &fd,
                                          &fd_lease, &error);
    ASSERT_EQ(error, WASI_ERROR_CODE_SUCCESS);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(quota_.active, 1u);
    ASSERT_EQ(count_process_fds(), process_fds_before + 1);

    HostResource *resource = host_resource_create(WASI_P2_FILESYSTEM_DESCRIPTOR,
                                                  sizeof(wasi_descriptor_t));
    ASSERT_NE(resource, nullptr);
    *static_cast<wasi_descriptor_t *>(resource->data) = fd;
    host_resource_set_dtor(resource, filesystem_descriptor_dtor);
    wasi_p2_native_fd_quota_transfer_to_host_resource(resource, &fd_lease);
    uint32_t representation =
        host_resource_table_add(get_global_host_resource_table(), resource);
    ASSERT_NE(representation, 0u);

    auto *instance = static_cast<WASMComponentInstance *>(
        wasm_runtime_malloc(sizeof(WASMComponentInstance)));
    ASSERT_NE(instance, nullptr);
    memset(instance, 0, sizeof(*instance));
    ASSERT_EQ(os_mutex_init(&instance->lifecycle_lock), BHT_OK);
    instance->lifecycle_lock_initialized = true;
    ASSERT_EQ(os_recursive_mutex_init(&instance->operation_lock), BHT_OK);
    instance->operation_lock_initialized = true;
    instance->lifecycle_root = instance;
    instance->table = wasm_component_table_init(4, 100);
    ASSERT_NE(instance->table, nullptr);

    WASMComponentResourceInstance resource_type = {};
    resource_type.is_builtin_wasi = true;
    WASMResourceHandle *handle =
        wasm_create_resource_handle(&resource_type, representation, true);
    ASSERT_NE(handle, nullptr);
    uint32_t table_index = 0;
    ASSERT_TRUE(wasm_component_table_add(instance->table, handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &table_index));
    ASSERT_NE(table_index, 0u);

    wasm_component_deinstantiate(instance);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(count_process_fds(), process_fds_before);
    EXPECT_EQ(host_resource_table_get(get_global_host_resource_table(),
                                      representation),
              nullptr);
}

} // namespace
