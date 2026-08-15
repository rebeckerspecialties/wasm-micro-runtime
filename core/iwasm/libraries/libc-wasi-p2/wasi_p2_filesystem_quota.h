/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASI_P2_FILESYSTEM_QUOTA_H
#define WASI_P2_FILESYSTEM_QUOTA_H

#include "wasi_p2_common.h"
#include "wasi_p2_filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

void
wasi_filesystem_stat_at_with_fd_quota(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t fd,
                                      wasi_path_flags_t path_flags,
                                      const char *path,
                                      wasi_descriptor_stat_t *ret, int *err);

int
wasi_filesystem_set_times_at_with_fd_quota(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd,
    wasi_path_flags_t path_flags, const char *path,
    wasi_new_timestamp_t data_access_timestamp,
    wasi_new_timestamp_t data_modification_timestamp);

int
wasi_filesystem_link_at_with_fd_quota(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t old_fd,
                                      wasi_path_flags_t old_path_flags,
                                      const char *old_path,
                                      wasi_descriptor_t new_fd,
                                      const char *new_path);

void
wasi_filesystem_open_at_with_fd_quota(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd,
    wasi_path_flags_t path_flags, const char *path,
    wasi_open_flags_t open_flags, wasi_descriptor_flags_t flags, mode_t mode,
    wasi_descriptor_t *ret, WasiP2NativeFdQuotaLease *out_fd_lease, int *err);

void
wasi_filesystem_readlink_at_with_fd_quota(wasm_exec_env_t exec_env,
                                          wasi_descriptor_t fd,
                                          const char *path, char **ret,
                                          int *err);

void
wasi_filesystem_metadata_hash_at_with_fd_quota(wasm_exec_env_t exec_env,
                                               wasi_descriptor_t fd,
                                               wasi_path_flags_t path_flags,
                                               const char *path,
                                               wasi_metadata_hash_value_t *ret,
                                               int *err);

int
wasi_filesystem_create_directory_at_with_fd_quota(wasm_exec_env_t exec_env,
                                                  wasi_descriptor_t fd,
                                                  const char *path);

int
wasi_filesystem_remove_directory_at_with_fd_quota(wasm_exec_env_t exec_env,
                                                  wasi_descriptor_t fd,
                                                  const char *path);

int
wasi_filesystem_unlink_file_at_with_fd_quota(wasm_exec_env_t exec_env,
                                             wasi_descriptor_t fd,
                                             const char *path);

int
wasi_filesystem_rename_at_with_fd_quota(wasm_exec_env_t exec_env,
                                        wasi_descriptor_t old_fd,
                                        const char *old_path,
                                        wasi_descriptor_t new_fd,
                                        const char *new_path);

int
wasi_filesystem_symlink_at_with_fd_quota(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd,
                                         const char *old_path,
                                         const char *new_path);

#ifdef __cplusplus
}
#endif

#endif /* WASI_P2_FILESYSTEM_QUOTA_H */
