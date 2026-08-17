/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "helpers.h"
#include "wasm_allocation_quota.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_resource_table.h"
#include "wasm_exec_env.h"

namespace {

constexpr uint64_t canonical_argv_bytes = 31 * sizeof(uint32_t);
constexpr uint32_t host_representation = 0xC011AB1E;

struct AllocationQuotaState {
    std::mutex lock;
    uint64_t live_bytes = 0;
    uint64_t reserved_bytes = 0;
    uint64_t released_bytes = 0;
    uint32_t live_allocations = 0;
    uint32_t reserved_allocations = 0;
    uint32_t released_allocations = 0;
    uint32_t denied_calls = 0;
    uint32_t canonical_argv_reservations = 0;
    uint32_t retain_calls = 0;
    uint32_t release_calls = 0;
    uint32_t canonical_argv_charge_bytes = 0;
    uint32_t deny_canonical_argv_countdown = 0;
};

struct CanonicalOwnState {
    AllocationQuotaState quota;
    WASMComponentResourceTable *last_table = nullptr;
    uint32_t last_handle = 0;
    uint32_t make_calls = 0;
    uint32_t drop_calls = 0;
};

bool
reserve_allocation(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *quota = static_cast<AllocationQuotaState *>(attachment);
    std::lock_guard<std::mutex> guard(quota->lock);

    if (bytes == quota->canonical_argv_charge_bytes && allocations == 1) {
        quota->canonical_argv_reservations++;
        if (quota->deny_canonical_argv_countdown) {
            quota->deny_canonical_argv_countdown--;
            if (!quota->deny_canonical_argv_countdown) {
                quota->denied_calls++;
                return false;
            }
        }
    }
    if (UINT64_MAX - quota->live_bytes < bytes
        || UINT32_MAX - quota->live_allocations < allocations) {
        quota->denied_calls++;
        return false;
    }
    quota->live_bytes += bytes;
    quota->live_allocations += allocations;
    quota->reserved_bytes += bytes;
    quota->reserved_allocations += allocations;
    return true;
}

void
release_allocation(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *quota = static_cast<AllocationQuotaState *>(attachment);
    std::lock_guard<std::mutex> guard(quota->lock);

    ASSERT_LE(bytes, quota->live_bytes);
    ASSERT_LE(allocations, quota->live_allocations);
    quota->live_bytes -= bytes;
    quota->live_allocations -= allocations;
    quota->released_bytes += bytes;
    quota->released_allocations += allocations;
}

bool
retain_quota_attachment(void *attachment)
{
    auto *quota = static_cast<AllocationQuotaState *>(attachment);
    std::lock_guard<std::mutex> guard(quota->lock);

    quota->retain_calls++;
    return true;
}

void
release_quota_attachment(void *attachment)
{
    auto *quota = static_cast<AllocationQuotaState *>(attachment);
    std::lock_guard<std::mutex> guard(quota->lock);

    quota->release_calls++;
}

void
host_make(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    auto *state = static_cast<CanonicalOwnState *>(
        wasm_component_get_custom_data_from_exec_env(exec_env));
    uint32_t handle = 0;

    if (!state || !exec_env || !exec_env->component_inst
        || !wasm_component_host_resource_new(exec_env, "test:quota/host@1.0.0",
                                             "item", host_representation,
                                             &handle)) {
        canonical_cells[0] = 0;
        return;
    }
    state->make_calls++;
    state->last_table = exec_env->component_inst->table;
    state->last_handle = handle;
    canonical_cells[0] = handle;
}

bool
host_drop(void *attachment, uint32_t representation)
{
    auto *state = static_cast<CanonicalOwnState *>(attachment);

    if (!state || representation != host_representation)
        return false;
    state->drop_calls++;
    return true;
}

class CanonicalOwnPreEntryTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        static NativeSymbol host_symbols[] = {
            { "make", reinterpret_cast<void *>(host_make), "()i", nullptr },
        };

        helper.do_setup();
        ASSERT_TRUE(helper.runtime_init);
        ASSERT_TRUE(wasm_allocation_header_calculate_size(
            canonical_argv_bytes, WASM_ALLOCATION_MAX_ALIGNMENT,
            &state.quota.canonical_argv_charge_bytes));
        ASSERT_TRUE(wasm_runtime_register_natives_raw(
            "test:quota/host@1.0.0", host_symbols,
            static_cast<uint32_t>(sizeof(host_symbols)
                                  / sizeof(host_symbols[0]))));
    }

    void TearDown() override
    {
        if (prepared) {
            wasm_component_destroy_prepared_call(prepared);
            prepared = nullptr;
        }
        helper.do_teardown();

        std::lock_guard<std::mutex> guard(state.quota.lock);
        EXPECT_EQ(state.quota.live_bytes, 0U);
        EXPECT_EQ(state.quota.live_allocations, 0U);
        EXPECT_EQ(state.quota.reserved_bytes, state.quota.released_bytes);
        EXPECT_EQ(state.quota.reserved_allocations,
                  state.quota.released_allocations);
        EXPECT_EQ(state.quota.deny_canonical_argv_countdown, 0U);
        EXPECT_EQ(state.quota.retain_calls, state.quota.release_calls);
    }

    void LoadAndInstantiate(uint32_t stack_size)
    {
        static char component_name[] = "canonical-own-pre-entry";
        LoadArgs2 load_args;
        struct InstantiationArgs2 *inst_args = nullptr;

        ASSERT_TRUE(
            helper.read_wasm_file("canonical_own_pre_entry_quota.wasm"));
        wasm_runtime_load_args2_init(&load_args);
        load_args.name = component_name;
        load_args.clone_wasm_binary = false;
        wasm_runtime_load_args2_set_allocation_quota(
            &load_args, &state.quota, reserve_allocation, release_allocation,
            retain_quota_attachment, release_quota_attachment);
        helper.component = wasm_component_load_ex2(
            helper.component_raw, helper.wasm_file_size, &load_args,
            helper.error_buf, sizeof(helper.error_buf));
        ASSERT_NE(helper.component, nullptr) << helper.error_buf;
        helper.component_init = true;
        ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
            helper.component, "test:quota/host@1.0.0", "item", host_drop));

        ASSERT_TRUE(wasm_runtime_instantiation_args_create(&inst_args));
        ASSERT_NE(inst_args, nullptr);
        wasm_runtime_instantiation_args_set_default_stack_size(inst_args,
                                                               stack_size);
        wasm_runtime_instantiation_args_set_host_managed_heap_size(inst_args,
                                                                   0);
        wasm_runtime_instantiation_args_set_custom_data(inst_args, &state);
        helper.component_inst = wasm_component_instantiate_ex2(
            helper.component, inst_args, helper.error_buf,
            sizeof(helper.error_buf));
        wasm_runtime_instantiation_args_destroy(inst_args);
        ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
        helper.component_instantiated = true;

        prepared = wasm_component_prepare_export_call(helper.component_inst,
                                                      "run", helper.error_buf,
                                                      sizeof(helper.error_buf));
        ASSERT_NE(prepared, nullptr) << helper.error_buf;
    }

    bool Call(wasm_val_t *result)
    {
        return wasm_component_call_prepared(prepared, 1, result, 0, nullptr);
    }

    void ExpectOriginalHandleWasRolledBack()
    {
        ASSERT_NE(state.last_table, nullptr);
        ASSERT_NE(state.last_handle, 0U);
        EXPECT_NE(wasm_component_table_get(state.last_table, state.last_handle,
                                           WASM_TABLE_ELEM_RESOURCE_HANDLE),
                  nullptr);
    }

    ComponentHelper helper;
    CanonicalOwnState state;
    WASMComponentPreparedCall *prepared = nullptr;
};

TEST_F(CanonicalOwnPreEntryTest, QuotaDeniedArgvAllocationRollsBackOwnParameter)
{
    wasm_val_t result = {};
    uint32_t argv_reservations_before;

    LoadAndInstantiate(64 * 1024);

    {
        std::lock_guard<std::mutex> guard(state.quota.lock);
        argv_reservations_before = state.quota.canonical_argv_reservations;
    }
    ASSERT_TRUE(Call(&result));
    ASSERT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, 99);
    ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared));
    {
        std::lock_guard<std::mutex> guard(state.quota.lock);
        ASSERT_EQ(state.quota.canonical_argv_reservations,
                  argv_reservations_before + 2);
        /* The first equal-sized allocation holds the lifted WIT parameter
           list; the second is wasm_runtime_call_wasm_a's 31-cell argv. */
        state.quota.deny_canonical_argv_countdown = 2;
    }

    result = {};
    EXPECT_FALSE(Call(&result));
    {
        std::lock_guard<std::mutex> guard(state.quota.lock);
        EXPECT_EQ(state.quota.deny_canonical_argv_countdown, 0U);
        EXPECT_EQ(state.quota.denied_calls, 1U);
    }
    EXPECT_EQ(state.make_calls, 2U);
    EXPECT_EQ(state.drop_calls, 1U);
    ExpectOriginalHandleWasRolledBack();
}

TEST_F(CanonicalOwnPreEntryTest,
       CalleeFrameOverflowRollsBackOwnParameterBeforeFirstOpcode)
{
    wasm_val_t result = {};

    LoadAndInstantiate(1024);
    EXPECT_FALSE(Call(&result));
    EXPECT_EQ(state.make_calls, 1U);
    EXPECT_EQ(state.drop_calls, 0U);
    ExpectOriginalHandleWasRolledBack();

    const char *exception =
        wasm_component_runtime_get_exception(helper.component_inst);
    ASSERT_NE(exception, nullptr);
    EXPECT_NE(std::strstr(exception, "wasm operand stack overflow"), nullptr)
        << exception;
}

} /* namespace */
