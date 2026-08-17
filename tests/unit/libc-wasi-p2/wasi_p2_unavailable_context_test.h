/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WAMR_TEST_WASI_P2_UNAVAILABLE_CONTEXT_H
#define WAMR_TEST_WASI_P2_UNAVAILABLE_CONTEXT_H

#include <cstring>
#include <dirent.h>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "wasm_allocation_quota.h"
#include "wasm_component_canonical.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_runtime.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace wasi_p2_test {

inline void
count_host_resource(void *, void *, void *user_data)
{
    auto *count = static_cast<uint32_t *>(user_data);
    (*count)++;
}

inline uint32_t
host_resource_count()
{
    uint32_t count = 0;
    HostResourceTable *table = get_global_host_resource_table();

    if (table)
        EXPECT_TRUE(bh_hash_map_traverse(table, count_host_resource, &count));
    return count;
}

inline int
open_fd_count()
{
    DIR *directory = opendir("/dev/fd");
    if (!directory)
        directory = opendir("/proc/self/fd");
    if (!directory)
        return -1;

    int count = 0;
    while (dirent *entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0
            && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(directory);
    return count;
}

struct SideEffectSnapshot {
    uint32_t host_resources = host_resource_count();
    int open_fds = open_fd_count();
    WASMAllocationQuotaToken *quota = wasm_allocation_quota_get_current();

    void expect_unchanged() const
    {
        EXPECT_EQ(host_resource_count(), host_resources);
        if (open_fds >= 0)
            EXPECT_EQ(open_fd_count(), open_fds);
        EXPECT_EQ(wasm_allocation_quota_get_current(), quota);
    }
};

class ScopedUnavailableWasi
{
  public:
    ScopedUnavailableWasi(WASMComponentInstance *instance, bool remove_context)
      : instance_(instance)
      , remove_context_(remove_context)
    {
        if (!instance_)
            return;
        if (remove_context_) {
            for (uint32_t i = 0; i < instance_->core_module_instances_count;
                 i++) {
                WASMModuleInstance *module =
                    instance_->core_module_instances[i];
                if (!module || !module->e)
                    continue;
                WASIContext *context = wasm_runtime_get_wasi_ctx(
                    reinterpret_cast<WASMModuleInstanceCommon *>(module));
                if (!context)
                    continue;
                contexts_.push_back({ module, context });
                wasm_runtime_set_wasi_ctx(
                    reinterpret_cast<WASMModuleInstanceCommon *>(module),
                    nullptr);
            }
            valid_ = !contexts_.empty();
        }
        else if (instance_->wasi_ctx) {
            options_ = instance_->wasi_ctx->wasi_options;
            instance_->wasi_ctx->wasi_options = nullptr;
            valid_ = options_ != nullptr;
        }
    }

    ~ScopedUnavailableWasi()
    {
        if (!instance_)
            return;
        if (remove_context_) {
            for (const auto &entry : contexts_) {
                wasm_runtime_set_wasi_ctx(
                    reinterpret_cast<WASMModuleInstanceCommon *>(entry.first),
                    entry.second);
            }
        }
        else if (instance_->wasi_ctx) {
            instance_->wasi_ctx->wasi_options = options_;
        }
    }

    bool valid() const { return valid_; }

  private:
    WASMComponentInstance *instance_ = nullptr;
    bool remove_context_ = false;
    bool valid_ = false;
    libc_wasi_options_t *options_ = nullptr;
    std::vector<std::pair<WASMModuleInstance *, WASIContext *>> contexts_;
};

class ScopedStringEncoding
{
  public:
    ScopedStringEncoding(CanonicalOptions *options, StringEncoding encoding)
      : options_(options)
    {
        if (!options_ || !options_->lift_lower_opts
            || !options_->lift_lower_opts->lift_opts) {
            return;
        }
        original_ = options_->lift_lower_opts->lift_opts->string_encoding;
        options_->lift_lower_opts->lift_opts->string_encoding = encoding;
        valid_ = true;
    }

    ~ScopedStringEncoding()
    {
        if (valid_)
            options_->lift_lower_opts->lift_opts->string_encoding = original_;
    }

    bool valid() const { return valid_; }

  private:
    CanonicalOptions *options_ = nullptr;
    StringEncoding original_ = ENCODING_UTF_8;
    bool valid_ = false;
};

} // namespace wasi_p2_test

#endif /* WAMR_TEST_WASI_P2_UNAVAILABLE_CONTEXT_H */
