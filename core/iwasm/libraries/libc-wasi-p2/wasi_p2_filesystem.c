/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_filesystem_quota.h"

#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <bh_common.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#include <linux/openat2.h>
#endif

#include "wasi_p2_common.h"

#if defined(__APPLE__)
/* Darwin/BSD names the nanosecond-resolution stat timestamp fields
 * st_*timespec rather than POSIX-2008's st_*tim. Both are 'struct timespec',
 * so a textual rename keeps every st_atim.tv_nsec etc. site working unchanged.
 */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
/* macOS has no fdatasync(); fsync() flushes both data and metadata, which is a
 * conformant (stronger-than-required) superset of fdatasync semantics. */
#define fdatasync fsync
#endif

/*
 * Path-taking filesystem methods are capability operations: a descriptor is
 * the root of the namespace visible to the guest.  POSIX *at() calls do not
 * provide that guarantee on their own because absolute paths ignore the
 * descriptor, ".." can walk above it, and an intermediate symlink can redirect
 * resolution elsewhere.
 *
 * Keep path policy in one place.  Linux opens a parent with openat2(2)'s
 * RESOLVE_BENEATH/RESOLVE_NO_SYMLINKS policy.  Apple platforms provide the
 * equivalent O_NOFOLLOW_ANY flag.  Once the parent directory is held by file
 * descriptor, the final *at() operation cannot be redirected by renaming an
 * ancestor.  Operations that intentionally inspect a final symlink (lstat,
 * readlink, unlink, or renaming the link itself) remain possible without ever
 * following that link.
 */
typedef struct wasi_beneath_path_ref_t {
    int parent_fd;
    WasiP2NativeFdQuotaLease parent_fd_lease;
    char *storage;
    const char *leaf;
    int parent_depth;
    bool trailing_slash;
} wasi_beneath_path_ref_t;

static int
wasi_sandbox_error(int error)
{
    /* openat2 reports a beneath escape as EXDEV, while both openat2 and
     * O_NOFOLLOW_ANY report prohibited symlink traversal as ELOOP.  Expose a
     * stable capability error to WASI callers. */
    return error == EXDEV || error == ELOOP ? EPERM : error;
}

static int
wasi_validate_beneath_path(const char *path, int initial_depth,
                           int *final_depth)
{
    int depth = initial_depth;
    size_t path_len;

    if (!path) {
        return EINVAL;
    }
    path_len = strnlen(path, (size_t)PATH_MAX + 1);
    if (path_len == 0) {
        return EINVAL;
    }
    if (path_len > PATH_MAX) {
        return ENAMETOOLONG;
    }
    if (path[0] == '/') {
        return EPERM;
    }

    for (const char *segment = path; segment && *segment;) {
        const char *slash = strchr(segment, '/');
        size_t segment_len =
            slash ? (size_t)(slash - segment) : strlen(segment);

        if (segment_len == 2 && segment[0] == '.' && segment[1] == '.') {
            if (depth == 0) {
                return EPERM;
            }
            depth--;
        }
        else if (!(segment_len == 0
                   || (segment_len == 1 && segment[0] == '.'))) {
            depth++;
        }
        segment = slash ? slash + 1 : NULL;
    }

    if (final_depth) {
        *final_depth = depth;
    }
    return WASI_ERROR_CODE_SUCCESS;
}

static int
wasi_open_beneath_directory(int parent_fd, const char *path)
{
#if defined(__APPLE__)
    return openat(parent_fd, path,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY);
#else
    struct open_how how = { 0 };
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    return syscall(SYS_openat2, parent_fd, path, &how, sizeof(how));
#endif
}

static void
wasi_beneath_path_ref_destroy(wasi_beneath_path_ref_t *ref)
{
    if (!ref) {
        return;
    }
    if (ref->parent_fd >= 0) {
        close(ref->parent_fd);
    }
    wasi_p2_native_fd_quota_release(&ref->parent_fd_lease);
    wasm_runtime_free(ref->storage);
    memset(ref, 0, sizeof(*ref));
    ref->parent_fd = -1;
}

static int
wasi_beneath_path_ref_init(wasm_exec_env_t exec_env, wasi_descriptor_t root_fd,
                           const char *path, wasi_beneath_path_ref_t *ref)
{
    int *directory_fds = NULL;
    WasiP2NativeFdQuotaLease *directory_fd_leases = NULL;
    size_t directory_fd_count = 0;
    size_t path_len;
    char *cursor;
    int error;

    if (!ref) {
        return EINVAL;
    }
    memset(ref, 0, sizeof(*ref));
    ref->parent_fd = -1;

    error = wasi_validate_beneath_path(path, 0, NULL);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }

    path_len = strlen(path);
    if (path_len >= UINT_MAX) {
        return ENAMETOOLONG;
    }
    ref->storage = wasm_runtime_malloc(path_len + 1);
    if (!ref->storage) {
        return ENOMEM;
    }
    memcpy(ref->storage, path, path_len + 1);
    ref->trailing_slash = path[path_len - 1] == '/';

    directory_fds =
        wasm_runtime_malloc((uint32_t)((path_len + 1) * sizeof(int)));
    if (!directory_fds) {
        error = ENOMEM;
        goto fail;
    }
    directory_fd_leases = wasm_runtime_malloc(
        (uint32_t)((path_len + 1) * sizeof(WasiP2NativeFdQuotaLease)));
    if (!directory_fd_leases) {
        error = ENOMEM;
        goto fail;
    }
    memset(directory_fd_leases, 0,
           (path_len + 1) * sizeof(WasiP2NativeFdQuotaLease));

    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1,
                                         &directory_fd_leases[0])) {
        error = EDQUOT;
        goto fail;
    }
    directory_fds[0] = wasi_open_beneath_directory(root_fd, ".");
    if (directory_fds[0] < 0) {
        error = wasi_sandbox_error(errno);
        wasi_p2_native_fd_quota_release(&directory_fd_leases[0]);
        goto fail;
    }
    directory_fd_count = 1;

    /* Keep each ancestor descriptor until the path is resolved.  A contained
     * ".." pops this stack instead of asking the kernel to traverse a mutable
     * parent relationship, so a concurrent rename cannot redirect it above
     * root_fd.  Real components are opened before they can be canceled by a
     * later "..", preserving openat2's existence/type/no-symlink semantics. */
    cursor = ref->storage;
    while (*cursor) {
        char *segment, *segment_end, *next_segment;
        bool is_last;

        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        segment = cursor;
        segment_end = segment;
        while (*segment_end && *segment_end != '/') {
            segment_end++;
        }
        next_segment = segment_end;
        while (*next_segment == '/') {
            next_segment++;
        }
        is_last = *next_segment == '\0';
        if (*segment_end) {
            *segment_end = '\0';
        }

        if (is_last) {
            if (strcmp(segment, "..") == 0) {
                directory_fd_count--;
                close(directory_fds[directory_fd_count]);
                wasi_p2_native_fd_quota_release(
                    &directory_fd_leases[directory_fd_count]);
                segment[0] = '.';
                segment[1] = '\0';
            }
            ref->leaf = segment;
            break;
        }

        if (strcmp(segment, ".") == 0) {
            cursor = next_segment;
            continue;
        }
        if (strcmp(segment, "..") == 0) {
            directory_fd_count--;
            close(directory_fds[directory_fd_count]);
            wasi_p2_native_fd_quota_release(
                &directory_fd_leases[directory_fd_count]);
            cursor = next_segment;
            continue;
        }

        if (!wasi_p2_native_fd_quota_reserve(
                exec_env, 1, &directory_fd_leases[directory_fd_count])) {
            error = EDQUOT;
            goto fail;
        }
        int child_fd = wasi_open_beneath_directory(
            directory_fds[directory_fd_count - 1], segment);
        if (child_fd < 0) {
            error = wasi_sandbox_error(errno);
            wasi_p2_native_fd_quota_release(
                &directory_fd_leases[directory_fd_count]);
            goto fail;
        }
        directory_fds[directory_fd_count++] = child_fd;
        cursor = next_segment;
    }

    if (!ref->leaf || directory_fd_count == 0) {
        error = EINVAL;
        goto fail;
    }

    ref->parent_fd = directory_fds[--directory_fd_count];
    ref->parent_fd_lease = directory_fd_leases[directory_fd_count];
    memset(&directory_fd_leases[directory_fd_count], 0,
           sizeof(directory_fd_leases[directory_fd_count]));
    ref->parent_depth = (int)directory_fd_count;
    while (directory_fd_count > 0) {
        directory_fd_count--;
        close(directory_fds[directory_fd_count]);
        wasi_p2_native_fd_quota_release(
            &directory_fd_leases[directory_fd_count]);
    }
    wasm_runtime_free(directory_fds);
    wasm_runtime_free(directory_fd_leases);
    return WASI_ERROR_CODE_SUCCESS;

fail:
    while (directory_fd_count > 0) {
        directory_fd_count--;
        close(directory_fds[directory_fd_count]);
        wasi_p2_native_fd_quota_release(
            &directory_fd_leases[directory_fd_count]);
    }
    wasm_runtime_free(directory_fds);
    wasm_runtime_free(directory_fd_leases);
    wasi_beneath_path_ref_destroy(ref);
    return error;
}

static bool
wasi_beneath_path_ref_leaf_is_dot(const wasi_beneath_path_ref_t *ref)
{
    return ref && (strcmp(ref->leaf, ".") == 0 || strcmp(ref->leaf, "..") == 0);
}

static int
wasi_beneath_path_ref_lstat(const wasi_beneath_path_ref_t *ref, struct stat *st)
{
    if (fstatat(ref->parent_fd, ref->leaf, st, AT_SYMLINK_NOFOLLOW) < 0) {
        return wasi_sandbox_error(errno);
    }
    return WASI_ERROR_CODE_SUCCESS;
}

void
filesystem_descriptor_dtor(void *data)
{
    wasi_filesystem_close(*(wasi_descriptor_t *)data);
}

void
file_stream_dtor(void *data)
{
    close(((StreamResourceType *)data)->fd);
}

void
directory_entry_stream_dtor(void *data)
{
    DIR *dir = *(DIR **)data;
    if (dir) {
        closedir(dir);
    }
}

/**
 * @brief Static helper to create a new stream handle by duplicating a
 *        descriptor and seeking it to a specific offset.
 * @details This is not a direct WIT export, but provides the core POSIX
 *          logic for implementing `read-via-stream` and `write-via-stream`
 *          from the `wasi:filesystem/types` interface
 * @param fd The base file descriptor to duplicate.
 * @param offset The absolute offset to seek the new descriptor to.
 * @param[out] ret Pointer to store the new file descriptor (stream handle).
 * @param[out] err Pointer to store the resulting WASI error code.
 */
static void
wasi_filesystem_seek_via_stream(wasi_descriptor_t fd, wasi_filesize_t offset,
                                int32_t *ret, int *err)
{
    if (!ret || !err) {
        return;
    }

    // Duplicate the descriptor to get a new one with an independent seek
    // position. The new descriptor shares the same underlying file description.
    int new_fd = dup(fd);
    if (new_fd < 0) {
        *err = errno;
        *ret = -1;
        return;
    }

    // Seek the *new* descriptor to the specified absolute offset.
    if (lseek(new_fd, offset, SEEK_SET) < 0) {
        *err = errno;
        *ret = -1;
        // Important: clean up the duplicated descriptor on failure.
        close(new_fd);
        return;
    }

    *err = 0;
    *ret = new_fd;
}

/**
 * @brief Return a stream for reading from a file at a given offset.
 * @details Implements the `read-via-stream` function from the
 *          `wasi:filesystem/types` interface. This creates a new,
 *          independent stream that does not interfere with other operations
 *          on the original descriptor.
 * @param fd The file descriptor to read from.
 * @param offset The absolute offset in the file where the new stream should
 * start.
 * @param[out] ret Pointer to store the new input stream handle (file
 * descriptor).
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_read_via_stream(wasi_descriptor_t fd, wasi_filesize_t offset,
                                wasi_input_stream_t *ret, int *err)
{
    if (!ret || !err) {
        return;
    }
    // Delegate to the helper function to perform the descriptor duplication
    // and seek operation.
    wasi_filesystem_seek_via_stream(fd, offset, ret, err);
}

/**
 * @brief Return a stream for writing to a file at a given offset.
 * @details Implements the `write-via-stream` function from the
 *          `wasi:filesystem/types` interface. This creates a new,
 *          independent stream that does not interfere with other operations
 *          on the original descriptor.
 * @param fd The file descriptor to write to.
 * @param offset The absolute offset in the file where the new stream should
 * start.
 * @param[out] ret Pointer to store the new output stream handle (file
 * descriptor).
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_write_via_stream(wasi_descriptor_t fd, wasi_filesize_t offset,
                                 wasi_output_stream_t *ret, int *err)
{
    if (!ret || !err) {
        return;
    }
    // Delegate to the helper function to perform the descriptor duplication
    // and seek operation.
    wasi_filesystem_seek_via_stream(fd, offset, ret, err);
}

/**
 * @brief Return a stream for appending to a file.
 * @details Implements the `append-via-stream` function from the
 *          `wasi:filesystem/types` interface. This creates a new,
 *          independent stream that is permanently set to append mode.
 * @param fd The file descriptor to append to.
 * @param[out] ret Pointer to store the new output stream handle (file
 * descriptor).
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_append_via_stream(wasi_descriptor_t fd,
                                  wasi_output_stream_t *ret, int *err)
{
    if (!ret || !err) {
        return;
    }

    // First, get the flags of the original descriptor to check permissions.
    int file_flags = fcntl(fd, F_GETFL);
    if (file_flags < 0) {
        *err = errno;
        *ret = -1;
        return;
    }

    // Ensure the original descriptor is writable. It's an error to create an
    // append stream from a read-only descriptor.
    if ((file_flags & O_ACCMODE) == O_RDONLY) {
        *err = EACCES;
        *ret = -1;
        return;
    }

    // Duplicate the descriptor to create an independent stream handle.
    int new_fd = dup(fd);
    if (new_fd < 0) {
        *err = errno;
        *ret = -1;
        return;
    }

    // Set the O_APPEND flag on the *new* descriptor. All subsequent writes
    // to this stream will automatically go to the end of the file.
    if (fcntl(new_fd, F_SETFL, file_flags | O_APPEND) < 0) {
        close(new_fd); // Clean up the new descriptor on failure.
        *err = errno;
        *ret = -1;
        return;
    }

    // On success, return the new descriptor.
    *err = 0;
    *ret = new_fd;
}

/**
 * @brief Static helper to convert a POSIX file type mode to a WASI descriptor
 * type.
 * @details This utility function maps the file type constants from a POSIX
 *          `stat` struct (`mode_t`) to the corresponding enum values defined in
 *          the `wasi:filesystem/types` interface.
 * @param mode The `st_mode` field from a POSIX `stat` struct.
 * @return The equivalent `wasi_descriptor_type_t`.
 */
static wasi_descriptor_type_t
convert_file_type(mode_t mode)
{
    // Use a bitmask to isolate the file type bits from the mode's permission
    // bits.
    switch (mode & S_IFMT) {
        case S_IFBLK:
            return WASI_DESCRIPTOR_TYPE_BLOCK_DEVICE;
        case S_IFCHR:
            return WASI_DESCRIPTOR_TYPE_CHARACTER_DEVICE;
        case S_IFDIR:
            return WASI_DESCRIPTOR_TYPE_DIRECTORY;
        case S_IFIFO:
            return WASI_DESCRIPTOR_TYPE_FIFO;
        case S_IFLNK:
            return WASI_DESCRIPTOR_TYPE_SYMBOLIC_LINK;
        case S_IFREG:
            return WASI_DESCRIPTOR_TYPE_REGULAR_FILE;
        case S_IFSOCK:
            return WASI_DESCRIPTOR_TYPE_SOCKET;
        default:
            // If the POSIX type is not recognized, return UNKNOWN.
            return WASI_DESCRIPTOR_TYPE_UNKNOWN;
    }
}

/**
 * @brief Static helper to convert a POSIX `stat` struct to a WASI
 * `descriptor_stat_t`.
 * @details This function translates the fields from the OS-specific `struct
 * stat` into the abstract `descriptor-stat` record defined in the
 *          `wasi:filesystem/types` interface.
 * @param st The source POSIX stat struct.
 * @param wasi_stat The destination WASI descriptor_stat_t struct to be
 * populated.
 */
static void
stat_to_wasi(const struct stat *st, wasi_descriptor_stat_t *wasi_stat)
{
    if (!st || !wasi_stat) {
        return;
    }
    *wasi_stat = (wasi_descriptor_stat_t){
        // Convert the file type using the dedicated helper function.
        .type = convert_file_type(st->st_mode),
        // Directly map the link count and size.
        .link_count = st->st_nlink,
        .size = st->st_size,
        // Map POSIX access, modification, and status change timestamps.
        // The `has_value = true` correctly represents the `some(datetime)`
        // variant for the `option<datetime>` WIT type.
        .data_access_timestamp = { .has_value = true,
                                   .datetime = { .seconds = st->st_atim.tv_sec,
                                                 .nanoseconds =
                                                     st->st_atim.tv_nsec } },
        .data_modification_timestamp = { .has_value = true,
                                         .datetime = { .seconds =
                                                           st->st_mtim.tv_sec,
                                                       .nanoseconds =
                                                           st->st_mtim
                                                               .tv_nsec } },
        .status_change_timestamp = { .has_value = true,
                                     .datetime = { .seconds =
                                                       st->st_ctim.tv_sec,
                                                   .nanoseconds =
                                                       st->st_ctim.tv_nsec } },
    };
}

/**
 * @brief Get metadata for an open descriptor.
 * @details Implements the `stat` function on the `descriptor` resource from the
 *          `wasi:filesystem/types` interface.
 * @param fd The file descriptor to get metadata for.
 * @param[out] ret Pointer to a `wasi_descriptor_stat_t` struct to store the
 * result.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_stat(wasi_descriptor_t fd, wasi_descriptor_stat_t *ret,
                     int *err)
{
    if (!ret || !err) {
        return;
    }
    struct stat st;

    // Use fstat to get file status information for the open file descriptor.
    if (fstat(fd, &st) < 0) {
        *err = errno;
        return;
    }

    // Use the helper function to convert the POSIX stat struct to the WASI
    // format.
    stat_to_wasi(&st, ret);
    *err = 0;
}

/**
 * @brief Get metadata for an object at a path relative to a descriptor.
 * @details Implements the `stat-at` function on the `descriptor` resource from
 *          the `wasi:filesystem/types` interface.
 * @param fd The base directory file descriptor.
 * @param path_flags Flags controlling path resolution, such as symlink
 * following.
 * @param path The path of the object to stat, relative to `fd`.
 * @param[out] ret Pointer to a `wasi_descriptor_stat_t` struct to store the
 * result.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
static void
wasi_filesystem_stat_at_impl(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                             wasi_path_flags_t path_flags, const char *path,
                             wasi_descriptor_stat_t *ret, int *err)
{
    wasi_beneath_path_ref_t path_ref;
    if (!err || !ret || !path) {
        if (err)
            *err = EINVAL;
        return;
    }
    struct stat st;

    *err = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }

    *err = wasi_beneath_path_ref_lstat(&path_ref, &st);
    wasi_beneath_path_ref_destroy(&path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }
    if ((path_flags & WASI_PATH_FLAGS_SYMLINK_FOLLOW) && S_ISLNK(st.st_mode)) {
        *err = EPERM;
        return;
    }

    // Use the helper function to convert the POSIX stat struct to the WASI
    // format.
    stat_to_wasi(&st, ret);
    *err = 0;
}

void
wasi_filesystem_stat_at(wasi_descriptor_t fd, wasi_path_flags_t path_flags,
                        const char *path, wasi_descriptor_stat_t *ret, int *err)
{
    wasi_filesystem_stat_at_impl(NULL, fd, path_flags, path, ret, err);
}

void
wasi_filesystem_stat_at_with_fd_quota(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t fd,
                                      wasi_path_flags_t path_flags,
                                      const char *path,
                                      wasi_descriptor_stat_t *ret, int *err)
{
    wasi_filesystem_stat_at_impl(exec_env, fd, path_flags, path, ret, err);
}

/**
 * @brief Unwraps a WASI error resource to get the underlying error code.
 * @details This function translates a handle to a `wasi:io/error` resource
 *          into an `option<error-code>`, which is a common pattern in WASI.
 *          A success state is mapped to `none`, while a failure state is
 *          mapped to `some(error-code)`.
 * @param err The error resource handle to inspect.
 * @param[out] is_some Set to `true` if an error code is present (`some`),
 *                     or `false` if there is no error (`none`).
 * @return The `errno` if an error is present, otherwise 0.
 */
int
wasi_filesystem_error_code(int err, bool *is_some)
{
    if (!is_some) {
        return EINVAL;
    }

    // A zero value for the error resource represents success, or the `none`
    // case.
    if (err == 0) {
        *is_some = false;
        return WASI_ERROR_CODE_SUCCESS;
    }

    // A non-zero value represents a specific error, or the `some(error-code)`
    // case.
    *is_some = true;
    return err;
}

/**
 * @brief Close a file descriptor.
 * @details Implements the resource destructor (`drop-descriptor`) for the
 *          `descriptor` resource in the `wasi:filesystem/types` interface.
 * @param fd The file descriptor to close.
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_close(wasi_descriptor_t fd)
{
    // Call the underlying POSIX close function.
    if (close(fd) < 0) {
        // If it fails, convert the OS-specific errno to a WASI error code.
        return errno;
    }
    return WASI_ERROR_CODE_SUCCESS;
}

/**
 * @brief Provide file advisory information on a descriptor.
 * @details Implements the `advise` function on the `descriptor` resource from
 *          the `wasi:filesystem/types` interface. This is a performance hint
 *          to the OS and is similar to `posix_fadvise` in POSIX.
 * @param fd The file descriptor to advise.
 * @param offset The starting offset of the file region.
 * @param length The length of the file region.
 * @param advice The advice to give (e.g., sequential, random access).
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_advise(wasi_descriptor_t fd, wasi_filesize_t offset,
                       wasi_filesize_t length, wasi_advice_t advice)
{
#if defined(__APPLE__)
    /* macOS/iOS have no posix_fadvise. The advice is purely an optional
     * performance hint to the kernel, so ignoring it is conformant behavior.
     * (F_RDADVISE/F_NOCACHE exist but do not map cleanly onto the WASI advice
     * enum, so we treat advise as a successful no-op.) */
    (void)fd;
    (void)offset;
    (void)length;
    (void)advice;
    return WASI_ERROR_CODE_SUCCESS;
#else
    int advice_posix = 0;

    // Translate the abstract WASI advice enum to the corresponding POSIX
    // constant.
    switch (advice) {
        case WASI_ADVICE_NORMAL:
            advice_posix = POSIX_FADV_NORMAL;
            break;
        case WASI_ADVICE_SEQUENTIAL:
            advice_posix = POSIX_FADV_SEQUENTIAL;
            break;
        case WASI_ADVICE_RANDOM:
            advice_posix = POSIX_FADV_RANDOM;
            break;
        case WASI_ADVICE_WILL_NEED:
            advice_posix = POSIX_FADV_WILLNEED;
            break;
        case WASI_ADVICE_DONT_NEED:
            advice_posix = POSIX_FADV_DONTNEED;
            break;
        case WASI_ADVICE_NO_REUSE:
            advice_posix = POSIX_FADV_NOREUSE;
            break;
    }

    // Call the underlying POSIX function with the translated advice.
    int ret = posix_fadvise(fd, offset, length, advice_posix);
    if (ret != 0) {
        // On failure, posix_fadvise returns the error number directly.
        return ret;
    }

    return WASI_ERROR_CODE_SUCCESS;
#endif
}

/**
 * @brief Synchronize the data of a file to disk.
 * @details Implements the `sync-data` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `fdatasync` in POSIX, ensuring that file data is physically
 *          written to the storage device.
 * @param fd The file descriptor to synchronize.
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_sync_data(wasi_descriptor_t fd)
{
    // Call the underlying POSIX fdatasync function.
    if (fdatasync(fd) < 0) {
        // If it fails, convert the OS-specific errno to a WASI error code.
        return errno;
    }
    return WASI_ERROR_CODE_SUCCESS;
}

/**
 * @brief Static helper to convert a WASI `new-timestamp` variant to a POSIX
 * `timespec`.
 * @details This function translates the `new-timestamp` variant from the
 *          `wasi:filesystem/types` interface into a `struct timespec` suitable
 *          for use with POSIX functions like `utimensat`.
 * @param wasi_ts The source WASI timestamp variant.
 * @param ts The destination POSIX timespec struct to be populated.
 */
static void
wasi_to_timespec(wasi_new_timestamp_t wasi_ts, struct timespec *ts)
{
    if (!ts) {
        return;
    }
    if (wasi_ts.tag == WASI_NEW_TIMESTAMP_TAG_NO_CHANGE) {
        // `no-change` in WIT maps to `UTIME_OMIT` in POSIX, which tells the
        // system call to leave this specific timestamp unmodified.
        ts->tv_nsec = UTIME_OMIT;
    }
    else if (wasi_ts.tag == WASI_NEW_TIMESTAMP_TAG_NOW) {
        // `now` in WIT maps to `UTIME_NOW` in POSIX, which tells the
        // system call to set the timestamp to the current time.
        ts->tv_nsec = UTIME_NOW;
    }
    else {
        // `timestamp(datetime)` in WIT maps to a specific time value.
        ts->tv_sec = wasi_ts.timestamp.seconds;
        ts->tv_nsec = wasi_ts.timestamp.nanoseconds;
    }
}

/**
 * @brief Set timestamps for an open descriptor.
 * @details Implements the `set-times` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `futimens` in POSIX.
 * @param fd The file descriptor to modify.
 * @param data_access_timestamp The new data access timestamp.
 * @param data_modification_timestamp The new data modification timestamp.
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_set_times(wasi_descriptor_t fd,
                          wasi_new_timestamp_t data_access_timestamp,
                          wasi_new_timestamp_t data_modification_timestamp)
{
    struct timespec times[2];

    // Use the helper function to convert the WASI timestamp variants into
    // the format required by the POSIX `futimens` call. The first element
    // is for access time, the second is for modification time.
    wasi_to_timespec(data_access_timestamp, &times[0]);
    wasi_to_timespec(data_modification_timestamp, &times[1]);

    // Call futimens to set the access and modification times on the open
    // descriptor.
    if (futimens(fd, times) < 0) {
        return errno;
    }

    return WASI_ERROR_CODE_SUCCESS;
}

/**
 * @brief Read data from a descriptor at a given offset.
 * @details Implements the `read` function on the `descriptor` resource from
 *          the `wasi:filesystem/types` interface. It performs a stateless
 *          read from a specific offset using `pread`, which does not affect
 *          the descriptor's internal seek position.
 *
 * @note The caller is responsible for freeing the returned buffer (`ret->buf`).
 *
 * @param fd The file descriptor to read from.
 * @param length The maximum number of bytes to read.
 * @param offset The absolute offset in the file to start reading from.
 * @param[out] ret Pointer to a list struct to store the resulting buffer and
 * length.
 * @param[out] end_of_stream Pointer to a boolean that will be set to `true` if
 *                           the end of the file was reached.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_read(wasi_descriptor_t fd, wasi_filesize_t length,
                     wasi_filesize_t offset, wasi_list_u8_t *ret,
                     bool *end_of_stream, int *err)
{
    if (!ret || !end_of_stream || !err) {
        if (err)
            *err = EINVAL;
        return;
    }

    ret->buf = NULL;
    ret->buf_len = 0;

    if (length == 0) {
        *end_of_stream = false;
        *err = 0;
        return;
    }

    uint8_t *buf = wasm_runtime_malloc(length);
    if (!buf) {
        *err = ENOMEM;
        return;
    }

    ssize_t s = pread(fd, buf, length, offset);
    if (s < 0) {
        wasm_runtime_free(buf);
        *err = errno;
        return;
    }

    if ((uint64_t)s < length) {
        *end_of_stream = true;
        if (s > 0) {
            uint8_t *new_buf = wasm_runtime_realloc(buf, s);
            if (!new_buf) {
                wasm_runtime_free(buf);
                *err = ENOMEM;
                return;
            }
            buf = new_buf;
        }
        else {
            wasm_runtime_free(buf);
            buf = NULL;
        }
    }
    else {
        *end_of_stream = false;
    }

    // On success, populate the return list.
    ret->buf = buf;
    ret->buf_len = s;
    *err = 0;
}

/**
 * @brief Write data to a descriptor at a given offset.
 * @details Implements the `write` function on the `descriptor` resource from
 *          the `wasi:filesystem/types` interface. It performs a stateless
 *          write to a specific offset using `pwrite`, which does not affect
 *          the descriptor's internal seek position.
 * @param fd The file descriptor to write to.
 * @param buf A pointer to the buffer containing the data to be written.
 * @param buf_len The number of bytes to write from the buffer.
 * @param offset The absolute offset in the file to start writing to.
 * @param[out] ret Pointer to store the number of bytes actually written.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_write(wasi_descriptor_t fd, const uint8_t *buf,
                      uint64_t buf_len, wasi_filesize_t offset,
                      wasi_filesize_t *ret, int *err)
{
    if (!buf || !ret || !err) {
        if (err) {
            *err = EINVAL;
        }
        return;
    }

    // Use pwrite to write to a specific offset without changing the
    // file descriptor's own cursor.
    ssize_t s = pwrite(fd, buf, buf_len, offset);
    if (s < 0) {
        *err = errno;
        *ret = 0;
        return;
    }

    // On success, return the number of bytes written.
    *err = 0;
    *ret = s;
}

/**
 * @brief Create a stream for reading entries from a directory.
 * @details Implements the `read-directory` function on the `descriptor`
 * resource from the `wasi:filesystem/types` interface. This creates a new,
 *          independent stream for iterating over directory entries.
 * @param fd The directory descriptor to read from.
 * @param[out] ret Pointer to store the new directory entry stream handle (`DIR
 * *`).
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_read_directory(wasi_descriptor_t fd,
                               wasi_directory_entry_stream_t *ret, int *err)
{
    if (!ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }

    // Duplicate the file descriptor to create an independent handle for the new
    // directory stream. This prevents iteration on the new stream from
    // interfering with the original descriptor's state.
    int new_fd = dup(fd);
    if (new_fd < 0) {
        *err = errno;
        *ret = NULL;
        return;
    }

    // Create a DIR* stream from the new file descriptor. This is the standard
    // POSIX object used for reading directory entries.
    DIR *dir = fdopendir(new_fd);
    if (!dir) {
        close(new_fd); // Clean up the duplicated descriptor on failure.
        *err = errno;
        *ret = NULL;
        return;
    }

    // On success, return the new DIR* stream as the resource handle.
    *err = 0;
    *ret = dir;
}

/**
 * @brief Synchronize the data and metadata of a file to disk.
 * @details Implements the `sync` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `fsync` in POSIX, ensuring that both file data and metadata are
 *          physically written to the storage device.
 * @param fd The file descriptor to synchronize.
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_sync(wasi_descriptor_t fd)
{
    // Call the underlying POSIX fsync function.
    if (fsync(fd) < 0) {
        // If it fails, convert the OS-specific errno to a WASI error code.
        return errno;
    }
    return WASI_ERROR_CODE_SUCCESS;
}

/**
 * @brief Set timestamps for a file or directory at a path relative to a
 * descriptor.
 * @details Implements the `set-times-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `utimensat` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path_flags Flags controlling path resolution, such as symlink
 * following.
 * @param path The path of the object to modify, relative to `fd`.
 * @param data_access_timestamp The new data access timestamp.
 * @param data_modification_timestamp The new data modification timestamp.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_set_times_at_impl(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd,
    wasi_path_flags_t path_flags, const char *path,
    wasi_new_timestamp_t data_access_timestamp,
    wasi_new_timestamp_t data_modification_timestamp)
{
    wasi_beneath_path_ref_t path_ref;
    struct stat st;
    int error;

    if (!path) {
        return EINVAL;
    }
    struct timespec times[2];

    // Use the helper to convert WASI timestamps to the POSIX timespec format.
    wasi_to_timespec(data_access_timestamp, &times[0]);
    wasi_to_timespec(data_modification_timestamp, &times[1]);

    error = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    error = wasi_beneath_path_ref_lstat(&path_ref, &st);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        wasi_beneath_path_ref_destroy(&path_ref);
        return error;
    }
    if ((path_flags & WASI_PATH_FLAGS_SYMLINK_FOLLOW) && S_ISLNK(st.st_mode)) {
        wasi_beneath_path_ref_destroy(&path_ref);
        return EPERM;
    }

    /* Never ask the kernel to follow the final component.  When the caller
     * requested follow semantics, the lstat above accepted only a non-link;
     * keeping AT_SYMLINK_NOFOLLOW also closes a link-swap race. */
    if (utimensat(path_ref.parent_fd, path_ref.leaf, times, AT_SYMLINK_NOFOLLOW)
        < 0) {
        error = wasi_sandbox_error(errno);
        wasi_beneath_path_ref_destroy(&path_ref);
        return error;
    }
    wasi_beneath_path_ref_destroy(&path_ref);
    return WASI_ERROR_CODE_SUCCESS;
}

int
wasi_filesystem_set_times_at(wasi_descriptor_t fd, wasi_path_flags_t path_flags,
                             const char *path,
                             wasi_new_timestamp_t data_access_timestamp,
                             wasi_new_timestamp_t data_modification_timestamp)
{
    return wasi_filesystem_set_times_at_impl(NULL, fd, path_flags, path,
                                             data_access_timestamp,
                                             data_modification_timestamp);
}

int
wasi_filesystem_set_times_at_with_fd_quota(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd,
    wasi_path_flags_t path_flags, const char *path,
    wasi_new_timestamp_t data_access_timestamp,
    wasi_new_timestamp_t data_modification_timestamp)
{
    return wasi_filesystem_set_times_at_impl(exec_env, fd, path_flags, path,
                                             data_access_timestamp,
                                             data_modification_timestamp);
}

/**
 * @brief Create a hard link.
 * @details Implements the `link-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `linkat` in POSIX.
 * @param old_fd The source directory descriptor.
 * @param old_path_flags Flags controlling resolution of the source path.
 * @param old_path The path of the existing file to link, relative to `old_fd`.
 * @param new_fd The destination directory descriptor.
 * @param new_path The path where the new link will be created, relative to
 * `new_fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_link_at_impl(wasm_exec_env_t exec_env, wasi_descriptor_t old_fd,
                             wasi_path_flags_t old_path_flags,
                             const char *old_path, wasi_descriptor_t new_fd,
                             const char *new_path)
{
    wasi_beneath_path_ref_t old_ref, new_ref;
    struct stat old_stat;
    int error;

    if (!old_path || !new_path) {
        return EINVAL;
    }
    error = wasi_beneath_path_ref_init(exec_env, old_fd, old_path, &old_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    error = wasi_beneath_path_ref_init(exec_env, new_fd, new_path, &new_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        wasi_beneath_path_ref_destroy(&old_ref);
        return error;
    }

    if (wasi_beneath_path_ref_leaf_is_dot(&old_ref)
        || wasi_beneath_path_ref_leaf_is_dot(&new_ref) || old_ref.trailing_slash
        || new_ref.trailing_slash) {
        error = EINVAL;
        goto done;
    }
    error = wasi_beneath_path_ref_lstat(&old_ref, &old_stat);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        goto done;
    }
    if ((old_path_flags & WASI_PATH_FLAGS_SYMLINK_FOLLOW)
        && S_ISLNK(old_stat.st_mode)) {
        error = EPERM;
        goto done;
    }

    /* flags=0 links a final symlink itself when follow was not requested.  It
     * never traverses the link, including if an attacker swaps it after the
     * lstat above. */
    if (linkat(old_ref.parent_fd, old_ref.leaf, new_ref.parent_fd, new_ref.leaf,
               0)
        < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&new_ref);
    wasi_beneath_path_ref_destroy(&old_ref);
    return error;
}

int
wasi_filesystem_link_at(wasi_descriptor_t old_fd,
                        wasi_path_flags_t old_path_flags, const char *old_path,
                        wasi_descriptor_t new_fd, const char *new_path)
{
    return wasi_filesystem_link_at_impl(NULL, old_fd, old_path_flags, old_path,
                                        new_fd, new_path);
}

int
wasi_filesystem_link_at_with_fd_quota(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t old_fd,
                                      wasi_path_flags_t old_path_flags,
                                      const char *old_path,
                                      wasi_descriptor_t new_fd,
                                      const char *new_path)
{
    return wasi_filesystem_link_at_impl(exec_env, old_fd, old_path_flags,
                                        old_path, new_fd, new_path);
}

/**
 * @brief Open a file or directory.
 * @details Implements the `open-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `openat` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path_flags Flags controlling path resolution.
 * @param path The path of the object to open, relative to `fd`.
 * @param open_flags Flags controlling how the file is opened (e.g., create,
 * truncate).
 * @param flags Flags representing the desired access rights (read, write).
 * @param[out] ret Pointer to store the new file descriptor.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
static void
wasi_filesystem_open_at_impl(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                             wasi_path_flags_t path_flags, const char *path,
                             wasi_open_flags_t open_flags,
                             wasi_descriptor_flags_t flags, mode_t mode,
                             wasi_descriptor_t *ret,
                             WasiP2NativeFdQuotaLease *out_fd_lease, int *err)
{
    wasi_beneath_path_ref_t path_ref;
    WasiP2NativeFdQuotaLease fd_lease = { 0 };

    if (ret)
        *ret = -1;
    if (out_fd_lease)
        memset(out_fd_lease, 0, sizeof(*out_fd_lease));

    if (!path || !ret || !err || (exec_env && !out_fd_lease)) {
        if (err)
            *err = EINVAL;
        if (ret)
            *ret = -1;
        return;
    }

    *ret = -1;

    *err = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }
    if (!wasi_p2_native_fd_quota_reserve(exec_env, 1, &fd_lease)) {
        *err = EDQUOT;
        wasi_beneath_path_ref_destroy(&path_ref);
        return;
    }

    int internal_flags = O_CLOEXEC;

#if defined(__APPLE__)
    /* O_NOFOLLOW_ANY below already covers the final component.  Darwin
     * rejects combining O_NOFOLLOW_ANY with O_NOFOLLOW as EINVAL. */
    (void)path_flags;
#else
    if (!(path_flags & WASI_PATH_FLAGS_SYMLINK_FOLLOW)) {
        internal_flags |= O_NOFOLLOW;
    }
#endif

    // Translate WASI open-flags to POSIX O_ flags.
    if (open_flags & WASI_OPEN_FLAGS_CREATE) {
        internal_flags |= O_CREAT;
    }
    if (open_flags & WASI_OPEN_FLAGS_DIRECTORY) {
        internal_flags |= O_DIRECTORY;
    }
    if (open_flags & WASI_OPEN_FLAGS_EXCLUSIVE) {
        internal_flags |= O_EXCL;
    }
    if (open_flags & WASI_OPEN_FLAGS_TRUNCATE) {
        internal_flags |= O_TRUNC;
    }
    if (path_ref.trailing_slash) {
        internal_flags |= O_DIRECTORY;
    }

    // Translate WASI descriptor-flags (access rights) to POSIX O_ flags.
    int accmode =
        flags & (WASI_DESCRIPTOR_FLAGS_READ | WASI_DESCRIPTOR_FLAGS_WRITE);
    if (accmode == (WASI_DESCRIPTOR_FLAGS_READ | WASI_DESCRIPTOR_FLAGS_WRITE)) {
        internal_flags |= O_RDWR;
    }
    else if (accmode == WASI_DESCRIPTOR_FLAGS_READ) {
        internal_flags |= O_RDONLY;
    }
    else if (accmode == WASI_DESCRIPTOR_FLAGS_WRITE) {
        internal_flags |= O_WRONLY;
    }

    // Translate WASI synchronization flags to POSIX O_ flags.
    if (flags & WASI_DESCRIPTOR_FLAGS_FILE_INTEGRITY_SYNC) {
        internal_flags |= O_SYNC;
    }
    if (flags & WASI_DESCRIPTOR_FLAGS_DATA_INTEGRITY_SYNC) {
        internal_flags |= O_DSYNC;
    }
#ifdef O_RSYNC
    if (flags & WASI_DESCRIPTOR_FLAGS_REQUESTED_WRITE_SYNC) {
        internal_flags |= O_RSYNC;
    }
#endif

#if defined(__APPLE__)
    /* The held parent was reached component-by-component without following
     * symlinks.  O_NOFOLLOW_ANY protects the remaining final component. */
    int r = openat(path_ref.parent_fd, path_ref.leaf,
                   internal_flags | O_NOFOLLOW_ANY,
                   (internal_flags & O_CREAT) ? mode : 0);
    if (r < 0) {
        /* Preserve ELOOP for the final-link error required by open-at while
         * still normalizing a beneath escape to the WASI capability error. */
        *err = errno == EXDEV ? EPERM : errno;
        *ret = -1;
        wasi_p2_native_fd_quota_release(&fd_lease);
        wasi_beneath_path_ref_destroy(&path_ref);
        return;
    }
    *err = 0;
    *ret = r;
    if (out_fd_lease) {
        *out_fd_lease = fd_lease;
        memset(&fd_lease, 0, sizeof(fd_lease));
    }
    wasi_beneath_path_ref_destroy(&path_ref);
#else
    struct open_how how = { 0 };
    how.flags = internal_flags;
    how.mode = (how.flags & (O_CREAT)) ? mode : 0;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    int r = syscall(SYS_openat2, path_ref.parent_fd, path_ref.leaf, &how,
                    sizeof(how));
    if (r < 0) {
        *err = wasi_sandbox_error(errno);
        *ret = -1;
        wasi_p2_native_fd_quota_release(&fd_lease);
        wasi_beneath_path_ref_destroy(&path_ref);
        return;
    }
    *err = 0;
    *ret = r;
    if (out_fd_lease) {
        *out_fd_lease = fd_lease;
        memset(&fd_lease, 0, sizeof(fd_lease));
    }
    wasi_beneath_path_ref_destroy(&path_ref);
#endif
}

void
wasi_filesystem_open_at(wasi_descriptor_t fd, wasi_path_flags_t path_flags,
                        const char *path, wasi_open_flags_t open_flags,
                        wasi_descriptor_flags_t flags, mode_t mode,
                        wasi_descriptor_t *ret, int *err)
{
    wasi_filesystem_open_at_impl(NULL, fd, path_flags, path, open_flags, flags,
                                 mode, ret, NULL, err);
}

void
wasi_filesystem_open_at_with_fd_quota(
    wasm_exec_env_t exec_env, wasi_descriptor_t fd,
    wasi_path_flags_t path_flags, const char *path,
    wasi_open_flags_t open_flags, wasi_descriptor_flags_t flags, mode_t mode,
    wasi_descriptor_t *ret, WasiP2NativeFdQuotaLease *out_fd_lease, int *err)
{
    wasi_filesystem_open_at_impl(exec_env, fd, path_flags, path, open_flags,
                                 flags, mode, ret, out_fd_lease, err);
}

/**
 * @brief Read the target of a symbolic link.
 * @details Implements the `readlink-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `readlinkat` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path The path of the symbolic link to read, relative to `fd`.
 * @param[out] ret Pointer to a char* to store the null-terminated path string.
 *               The caller is responsible for freeing this buffer.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
static void
wasi_filesystem_readlink_at_impl(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                 const char *path, char **ret, int *err)
{
    wasi_beneath_path_ref_t path_ref;

    if (!path || !ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }
    *ret = NULL;
    *err = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&path_ref)
        || path_ref.trailing_slash) {
        wasi_beneath_path_ref_destroy(&path_ref);
        *err = EINVAL;
        return;
    }

    char *buf = NULL;
    // Start with a reasonable initial buffer size.
    size_t buf_size = 128;
    ssize_t s;

    // Loop to handle cases where the buffer is too small for the link target.
    while (true) {
        char *new_buf = wasm_runtime_realloc(buf, buf_size);
        if (!new_buf) {
            wasm_runtime_free(buf);
            *err = ENOMEM;
            *ret = NULL;
            wasi_beneath_path_ref_destroy(&path_ref);
            return;
        }
        buf = new_buf;

        // Attempt to read the symbolic link's target path.
        s = readlinkat(path_ref.parent_fd, path_ref.leaf, buf, buf_size);
        if (s < 0) {
            wasm_runtime_free(buf);
            *err = wasi_sandbox_error(errno);
            *ret = NULL;
            wasi_beneath_path_ref_destroy(&path_ref);
            return;
        }

        // If the number of bytes written is less than the buffer size,
        // the entire path was read successfully.
        if ((size_t)s < buf_size) {
            break;
        }

        // Otherwise, the buffer might have been too small. Double it and retry.
        buf_size *= 2;
    }

    // `readlinkat` does not null-terminate the string, so we must do it
    // manually.
    buf[s] = '\0';
    *err = 0;
    *ret = buf;
    wasi_beneath_path_ref_destroy(&path_ref);
}

void
wasi_filesystem_readlink_at(wasi_descriptor_t fd, const char *path, char **ret,
                            int *err)
{
    wasi_filesystem_readlink_at_impl(NULL, fd, path, ret, err);
}

void
wasi_filesystem_readlink_at_with_fd_quota(wasm_exec_env_t exec_env,
                                          wasi_descriptor_t fd,
                                          const char *path, char **ret,
                                          int *err)
{
    wasi_filesystem_readlink_at_impl(exec_env, fd, path, ret, err);
}

/**
 * @brief Compare two descriptors to see if they refer to the same object.
 * @details Implements the `is-same-object` function on the `descriptor`
 * resource from the `wasi:filesystem/types` interface. In POSIX, this is
 *          determined by comparing the device ID and the inode number.
 * @param fd1 The first file descriptor.
 * @param fd2 The second file descriptor.
 * @return `true` if the descriptors refer to the same object, `false`
 * otherwise.
 */
bool
wasi_filesystem_is_same_object(wasi_descriptor_t fd1, wasi_descriptor_t fd2)
{
    struct stat st1, st2;

    // Get the stat structures for both file descriptors.
    fstat(fd1, &st1);
    fstat(fd2, &st2);

    // Two descriptors refer to the same object if and only if they are on the
    // same device (st_dev) and have the same inode number (st_ino).
    return st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino;
}

#define FNV_PRIME_64 1099511628211ULL
#define FNV_OFFSET_BASIS_64 14695981039346656037ULL

/**
 * @brief Static helper to compute a 64-bit FNV-1a hash.
 * @details This is a standard, non-cryptographic hashing algorithm used to
 *          generate a hash value from a block of data. It requires the
 *          predefined constants FNV_PRIME_64 and FNV_OFFSET_BASIS_64.
 * @param data A pointer to the data to be hashed.
 * @param size The size of the data in bytes.
 * @param h The initial hash value (the offset basis for the first call).
 * @return The computed 64-bit hash value.
 */
static uint64_t
fnv1a_hash(const void *data, size_t size, uint64_t h)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= FNV_PRIME_64;
    }
    return h;
}

/**
 * @brief Static helper to convert a POSIX `stat` struct to a WASI
 * `metadata-hash-value`.
 * @details This function provides the logic for the `metadata-hash` function
 * from the `wasi:filesystem/types` interface. It generates a 128-bit value by
 * creating two 64-bit FNV-1a hashes from various metadata fields, making it
 * effective at detecting file changes.
 * @param st The source POSIX stat struct.
 * @param ret The destination WASI metadata_hash_value_t struct to be populated.
 */
static void
stat_to_metadata_hash(const struct stat *st, wasi_metadata_hash_value_t *ret)
{
    if (!st || !ret) {
        return;
    }

    // Calculate the lower 64 bits of the hash from the primary metadata.
    uint64_t h1 = FNV_OFFSET_BASIS_64;
    h1 = fnv1a_hash(&st->st_dev, sizeof(st->st_dev), h1);
    h1 = fnv1a_hash(&st->st_ino, sizeof(st->st_ino), h1);
    h1 = fnv1a_hash(&st->st_size, sizeof(st->st_size), h1);
    h1 = fnv1a_hash(&st->st_mtim, sizeof(st->st_mtim), h1);

    // Calculate the upper 64 bits of the hash from secondary metadata.
    uint64_t h2 = FNV_OFFSET_BASIS_64;
    h2 = fnv1a_hash(&st->st_ctim, sizeof(st->st_ctim), h2);
    h2 = fnv1a_hash(&st->st_nlink, sizeof(st->st_nlink), h2);
    h2 = fnv1a_hash(&st->st_mode, sizeof(st->st_mode), h2);
    h2 = fnv1a_hash(&st->st_uid, sizeof(st->st_uid), h2);
    h2 = fnv1a_hash(&st->st_gid, sizeof(st->st_gid), h2);

    ret->lower = h1;
    ret->upper = h2;
}

/**
 * @brief Get a 128-bit hash of a descriptor's metadata.
 * @details Implements the `metadata-hash` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This value can be used
 *          to quickly check for file changes.
 * @param fd The file descriptor to get the hash for.
 * @param[out] ret Pointer to a `wasi_metadata_hash_value_t` struct to store the
 * result.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_metadata_hash(wasi_descriptor_t fd,
                              wasi_metadata_hash_value_t *ret, int *err)
{
    if (!ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }
    struct stat st;

    // Use fstat to get the file status information for the open descriptor.
    if (fstat(fd, &st) < 0) {
        *err = errno;
        return;
    }

    // Use the helper function to convert the POSIX stat struct into the
    // 128-bit WASI metadata hash.
    stat_to_metadata_hash(&st, ret);
    *err = 0;
}

/**
 * @brief Get a 128-bit hash of a file's metadata at a path relative to a
 * descriptor.
 * @details Implements the `metadata-hash-at` function on the `descriptor`
 * resource from the `wasi:filesystem/types` interface.
 * @param fd The base directory file descriptor.
 * @param path_flags Flags controlling path resolution, such as symlink
 * following.
 * @param path The path of the object to get the hash for, relative to `fd`.
 * @param[out] ret Pointer to a `wasi_metadata_hash_value_t` struct to store the
 * result.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
static void
wasi_filesystem_metadata_hash_at_impl(wasm_exec_env_t exec_env,
                                      wasi_descriptor_t fd,
                                      wasi_path_flags_t path_flags,
                                      const char *path,
                                      wasi_metadata_hash_value_t *ret, int *err)
{
    wasi_beneath_path_ref_t path_ref;

    if (!path || !ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }
    struct stat st;

    *err = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }
    *err = wasi_beneath_path_ref_lstat(&path_ref, &st);
    wasi_beneath_path_ref_destroy(&path_ref);
    if (*err != WASI_ERROR_CODE_SUCCESS) {
        return;
    }
    if ((path_flags & WASI_PATH_FLAGS_SYMLINK_FOLLOW) && S_ISLNK(st.st_mode)) {
        *err = EPERM;
        return;
    }

    // Use the helper function to convert the POSIX stat struct into the
    // 128-bit WASI metadata hash.
    stat_to_metadata_hash(&st, ret);
    *err = 0;
}

void
wasi_filesystem_metadata_hash_at(wasi_descriptor_t fd,
                                 wasi_path_flags_t path_flags, const char *path,
                                 wasi_metadata_hash_value_t *ret, int *err)
{
    wasi_filesystem_metadata_hash_at_impl(NULL, fd, path_flags, path, ret, err);
}

void
wasi_filesystem_metadata_hash_at_with_fd_quota(wasm_exec_env_t exec_env,
                                               wasi_descriptor_t fd,
                                               wasi_path_flags_t path_flags,
                                               const char *path,
                                               wasi_metadata_hash_value_t *ret,
                                               int *err)
{
    wasi_filesystem_metadata_hash_at_impl(exec_env, fd, path_flags, path, ret,
                                          err);
}

/**
 * @brief Read a single directory entry from a directory stream.
 * @details Implements the `read-directory-entry` method on the
 *          `directory-entry-stream` resource from `wasi:filesystem/types`.
 * @param stream The directory entry stream handle (`DIR *`) to read from.
 * @param[out] ret Pointer to a `wasi_directory_entry_t` struct to store the
 * result.
 * @param[out] is_some Set to `true` if an entry was read, or `false` if the
 *                     end of the stream has been reached.
 * @param[out] error_code Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_read_directory_entry(wasi_directory_entry_stream_t stream,
                                     wasi_directory_entry_t *ret, bool *is_some,
                                     int *error_code)
{
    if (!stream || !ret || !is_some || !error_code) {
        if (error_code) {
            *error_code = EINVAL;
        }
        return;
    }

    while (true) {
        errno = 0;
        struct dirent *entry = readdir(stream);

        if (!entry) {
            *error_code = errno;
            *is_some = false;
            return;
        }

        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        wasi_descriptor_type_t type;

        // Translate the POSIX d_type to the WASI descriptor_type.
        switch (entry->d_type) {
            case DT_BLK:
                type = WASI_DESCRIPTOR_TYPE_BLOCK_DEVICE;
                break;
            case DT_CHR:
                type = WASI_DESCRIPTOR_TYPE_CHARACTER_DEVICE;
                break;
            case DT_DIR:
                type = WASI_DESCRIPTOR_TYPE_DIRECTORY;
                break;
            case DT_FIFO:
                type = WASI_DESCRIPTOR_TYPE_FIFO;
                break;
            case DT_LNK:
                type = WASI_DESCRIPTOR_TYPE_SYMBOLIC_LINK;
                break;
            case DT_REG:
                type = WASI_DESCRIPTOR_TYPE_REGULAR_FILE;
                break;
            case DT_SOCK:
                type = WASI_DESCRIPTOR_TYPE_SOCKET;
                break;
            default:
            {
                // Fallback for filesystems that don't support d_type
                // (DT_UNKNOWN). We get the directory's fd and use fstatat to
                // get the full info.
                struct stat st;
                int dir_fd = dirfd(stream);
                if (dir_fd == -1) {
                    *error_code = errno;
                    *is_some = false;
                    return;
                }
                if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)
                    != 0) {
                    // If fstatat fails for an entry (e.g. permission denied),
                    // skip it and continue to the next one.
                    continue;
                }
                type = convert_file_type(st.st_mode);
                break;
            }
        }

        /* The caller is responsible for freeing ret->name. */
        ret->name = wa_strdup(entry->d_name);
        if (!ret->name) {
            *error_code = ENOMEM;
            *is_some = false;
            return;
        }

        ret->type = type;
        *error_code = 0;
        *is_some = true;
        return;
    }
}

/**
 * @brief Get the type of an open descriptor.
 * @details Implements the `get-type` method on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          checking the S_IFMT bits of the `st_mode` field from `fstat` in
 * POSIX.
 * @param fd The file descriptor to inspect.
 * @param[out] ret Pointer to a `wasi_descriptor_type_t` to store the result.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_get_type(wasi_descriptor_t fd, wasi_descriptor_type_t *ret,
                         int *err)
{
    if (!ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }
    struct stat st;

    // Use fstat to get file status information for the open file descriptor.
    if (fstat(fd, &st) < 0) {
        *err = errno;
        return;
    }

    *err = 0;
    // Use the helper function to convert the POSIX mode to a WASI descriptor
    // type.
    *ret = convert_file_type(st.st_mode);
}

/**
 * @brief Get the flags associated with a descriptor.
 * @details Implements the `get-flags` method on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `fcntl(fd, F_GETFL)` in POSIX.
 * @param fd The file descriptor to inspect.
 * @param[out] ret Pointer to a `wasi_descriptor_flags_t` to store the resulting
 * flags.
 * @param[out] err Pointer to store the resulting WASI error code.
 */
void
wasi_filesystem_get_flags(wasi_descriptor_t fd, wasi_descriptor_flags_t *ret,
                          int *err)
{
    if (!ret || !err) {
        if (err)
            *err = EINVAL;
        return;
    }
    // Use fcntl with F_GETFL to get the file status flags from the OS.
    int flags = fcntl(fd, F_GETFL);
    wasi_descriptor_flags_t f = 0;

    bool is_dir = false;
    // Use fstat to check if fd is a directory
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            is_dir = true;
        }
    }

    // Translate the POSIX access mode flags (O_RDONLY, O_WRONLY, O_RDWR)
    // to the corresponding WASI read/write flags.
    if ((flags & O_ACCMODE) == O_RDWR) {
        f |= WASI_DESCRIPTOR_FLAGS_READ | WASI_DESCRIPTOR_FLAGS_WRITE;
        if (is_dir) {
            f |= WASI_DESCRIPTOR_FLAGS_MUTATE_DIRECTORY;
        }
    }
    else if ((flags & O_ACCMODE) == O_RDONLY) {
        f |= WASI_DESCRIPTOR_FLAGS_READ;
    }
    else if ((flags & O_ACCMODE) == O_WRONLY) {
        f |= WASI_DESCRIPTOR_FLAGS_WRITE;
        if (is_dir) {
            f |= WASI_DESCRIPTOR_FLAGS_MUTATE_DIRECTORY;
        }
    }

    // Translate POSIX synchronization flags to their WASI equivalents.
    if (flags & O_DSYNC)
        f |= WASI_DESCRIPTOR_FLAGS_DATA_INTEGRITY_SYNC;
#ifdef O_RSYNC
    if (flags & O_RSYNC)
        f |= WASI_DESCRIPTOR_FLAGS_REQUESTED_WRITE_SYNC;
#endif
    if (flags & O_SYNC)
        f |= WASI_DESCRIPTOR_FLAGS_FILE_INTEGRITY_SYNC;

    *err = 0;
    *ret = f;
}

/**
 * @brief Set the size of an open file descriptor.
 * @details Implements the `set-size` method on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `ftruncate` in POSIX.
 * @param fd The file descriptor to modify.
 * @param size The new size of the file in bytes.
 * @return A WASI error code, `SUCCESS` on success.
 */
int
wasi_filesystem_set_size(wasi_descriptor_t fd, wasi_filesize_t size)
{
    // Call the underlying POSIX ftruncate function to set the file size.
    if (ftruncate(fd, size) < 0) {
        // If it fails, convert the OS-specific errno to a WASI error code.
        return errno;
    }
    return WASI_ERROR_CODE_SUCCESS;
}

/**
 * @brief Create a directory at a path relative to a descriptor.
 * @details Implements the `create-directory-at` function on the `descriptor`
 *          resource from the `wasi:filesystem/types` interface. This is
 *          similar to `mkdirat` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path The path where the new directory will be created, relative to
 * `fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_create_directory_at_impl(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd, const char *path)
{
    wasi_beneath_path_ref_t path_ref;
    int error;

    if (!path) {
        return EINVAL;
    }

    error = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&path_ref)) {
        error = EINVAL;
        goto done;
    }

    // The mode is hardcoded because the WIT interface has no mode parameter.
    if (mkdirat(path_ref.parent_fd, path_ref.leaf, 0777) < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&path_ref);
    return error;
}

int
wasi_filesystem_create_directory_at(wasi_descriptor_t fd, const char *path)
{
    return wasi_filesystem_create_directory_at_impl(NULL, fd, path);
}

int
wasi_filesystem_create_directory_at_with_fd_quota(wasm_exec_env_t exec_env,
                                                  wasi_descriptor_t fd,
                                                  const char *path)
{
    return wasi_filesystem_create_directory_at_impl(exec_env, fd, path);
}

/**
 * @brief Remove a directory at a path relative to a descriptor.
 * @details Implements the `remove-directory-at` function on the `descriptor`
 *          resource from the `wasi:filesystem/types` interface. This is
 *          similar to `unlinkat(..., AT_REMOVEDIR)` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path The path of the directory to remove, relative to `fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_remove_directory_at_impl(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd, const char *path)
{
    wasi_beneath_path_ref_t path_ref;
    int error;

    if (!path) {
        return EINVAL;
    }

    error = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&path_ref)) {
        error = EINVAL;
        goto done;
    }

    if (unlinkat(path_ref.parent_fd, path_ref.leaf, AT_REMOVEDIR) < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&path_ref);
    return error;
}

int
wasi_filesystem_remove_directory_at(wasi_descriptor_t fd, const char *path)
{
    return wasi_filesystem_remove_directory_at_impl(NULL, fd, path);
}

int
wasi_filesystem_remove_directory_at_with_fd_quota(wasm_exec_env_t exec_env,
                                                  wasi_descriptor_t fd,
                                                  const char *path)
{
    return wasi_filesystem_remove_directory_at_impl(exec_env, fd, path);
}

/**
 * @brief Remove a file at a path relative to a descriptor.
 * @details Implements the `unlink-file-at` function on the `descriptor`
 *          resource from the `wasi:filesystem/types` interface. This is
 *          similar to `unlinkat(..., 0)` in POSIX.
 * @param fd The base directory file descriptor.
 * @param path The path of the file to remove, relative to `fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_unlink_file_at_impl(wasm_exec_env_t exec_env,
                                    wasi_descriptor_t fd, const char *path)
{
    wasi_beneath_path_ref_t path_ref;
    int error;

    if (!path) {
        return EINVAL;
    }

    error = wasi_beneath_path_ref_init(exec_env, fd, path, &path_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&path_ref)
        || path_ref.trailing_slash) {
        error = EINVAL;
        goto done;
    }

    if (unlinkat(path_ref.parent_fd, path_ref.leaf, 0) < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&path_ref);
    return error;
}

int
wasi_filesystem_unlink_file_at(wasi_descriptor_t fd, const char *path)
{
    return wasi_filesystem_unlink_file_at_impl(NULL, fd, path);
}

int
wasi_filesystem_unlink_file_at_with_fd_quota(wasm_exec_env_t exec_env,
                                             wasi_descriptor_t fd,
                                             const char *path)
{
    return wasi_filesystem_unlink_file_at_impl(exec_env, fd, path);
}

/**
 * @brief Rename or move a file or directory.
 * @details Implements the `rename-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `renameat` in POSIX.
 * @param old_fd The source directory descriptor.
 * @param old_path The path of the object to rename, relative to `old_fd`.
 * @param new_fd The destination directory descriptor.
 * @param new_path The new path for the object, relative to `new_fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_rename_at_impl(wasm_exec_env_t exec_env,
                               wasi_descriptor_t old_fd, const char *old_path,
                               wasi_descriptor_t new_fd, const char *new_path)
{
    wasi_beneath_path_ref_t old_ref, new_ref;
    int error;

    if (!old_path || !new_path) {
        return EINVAL;
    }

    error = wasi_beneath_path_ref_init(exec_env, old_fd, old_path, &old_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    error = wasi_beneath_path_ref_init(exec_env, new_fd, new_path, &new_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        wasi_beneath_path_ref_destroy(&old_ref);
        return error;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&old_ref)
        || wasi_beneath_path_ref_leaf_is_dot(&new_ref) || old_ref.trailing_slash
        || new_ref.trailing_slash) {
        error = EINVAL;
        goto done;
    }

    if (renameat(old_ref.parent_fd, old_ref.leaf, new_ref.parent_fd,
                 new_ref.leaf)
        < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&new_ref);
    wasi_beneath_path_ref_destroy(&old_ref);
    return error;
}

int
wasi_filesystem_rename_at(wasi_descriptor_t old_fd, const char *old_path,
                          wasi_descriptor_t new_fd, const char *new_path)
{
    return wasi_filesystem_rename_at_impl(NULL, old_fd, old_path, new_fd,
                                          new_path);
}

int
wasi_filesystem_rename_at_with_fd_quota(wasm_exec_env_t exec_env,
                                        wasi_descriptor_t old_fd,
                                        const char *old_path,
                                        wasi_descriptor_t new_fd,
                                        const char *new_path)
{
    return wasi_filesystem_rename_at_impl(exec_env, old_fd, old_path, new_fd,
                                          new_path);
}

/**
 * @brief Create a symbolic link.
 * @details Implements the `symlink-at` function on the `descriptor` resource
 *          from the `wasi:filesystem/types` interface. This is similar to
 *          `symlinkat` in POSIX.
 * @param fd The base directory file descriptor where the link will be created.
 * @param old_path The string content of the new symbolic link (the path it
 * points to).
 * @param new_path The path where the new symbolic link will be created,
 * relative to `fd`.
 * @return A WASI error code, `SUCCESS` on success.
 */
static int
wasi_filesystem_symlink_at_impl(wasm_exec_env_t exec_env, wasi_descriptor_t fd,
                                const char *old_path, const char *new_path)
{
    wasi_beneath_path_ref_t new_ref;
    int error;

    if (!old_path || !new_path) {
        return EINVAL;
    }

    error = wasi_beneath_path_ref_init(exec_env, fd, new_path, &new_ref);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        return error;
    }
    if (wasi_beneath_path_ref_leaf_is_dot(&new_ref) || new_ref.trailing_slash) {
        error = EINVAL;
        goto done;
    }

    /* A relative symlink target is interpreted from the new link's parent.
     * Seed validation with that parent's depth so "../peer" can stay inside a
     * nested preopen while an absolute or root-escaping target is rejected. */
    error = wasi_validate_beneath_path(old_path, new_ref.parent_depth, NULL);
    if (error != WASI_ERROR_CODE_SUCCESS) {
        goto done;
    }

    if (symlinkat(old_path, new_ref.parent_fd, new_ref.leaf) < 0) {
        error = wasi_sandbox_error(errno);
        goto done;
    }
    error = WASI_ERROR_CODE_SUCCESS;

done:
    wasi_beneath_path_ref_destroy(&new_ref);
    return error;
}

int
wasi_filesystem_symlink_at(wasi_descriptor_t fd, const char *old_path,
                           const char *new_path)
{
    return wasi_filesystem_symlink_at_impl(NULL, fd, old_path, new_path);
}

int
wasi_filesystem_symlink_at_with_fd_quota(wasm_exec_env_t exec_env,
                                         wasi_descriptor_t fd,
                                         const char *old_path,
                                         const char *new_path)
{
    return wasi_filesystem_symlink_at_impl(exec_env, fd, old_path, new_path);
}
