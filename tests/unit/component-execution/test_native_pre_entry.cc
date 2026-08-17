/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

extern "C" {
#include "wasm_allocation_quota.h"
#include "wasm_c_api.h"
#include "wasm_exec_env.h"
#include "wasm_export.h"
#include "wasm_runtime.h"
#include "wasm_runtime_common.h"
}

#include "helpers.h"

namespace {

constexpr uint32_t target_param_count = 17;

struct NativePreEntryState {
    std::mutex lock;
    uint64_t live_bytes = 0;
    uint64_t reserved_bytes = 0;
    uint64_t released_bytes = 0;
    uint32_t live_allocations = 0;
    uint32_t reserved_allocations = 0;
    uint32_t released_allocations = 0;
    uint32_t retain_calls = 0;
    uint32_t release_calls = 0;
    uint32_t denied_calls = 0;
    uint32_t raw_argv_charge_bytes = 0;
    uint32_t c_api_params_charge_bytes = 0;
    uint32_t raw_argv_reservations = 0;
    uint32_t c_api_params_reservations = 0;
    uint32_t pre_entry_calls = 0;
    uint32_t raw_host_calls = 0;
    uint32_t c_api_host_calls = 0;
    bool deny_raw_argv = false;
    bool deny_c_api_params = false;
};

bool
reserve_allocation(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<NativePreEntryState *>(attachment);
    std::lock_guard<std::mutex> guard(state->lock);

    if (bytes == state->raw_argv_charge_bytes && allocations == 1) {
        state->raw_argv_reservations++;
        if (state->deny_raw_argv) {
            state->deny_raw_argv = false;
            state->denied_calls++;
            return false;
        }
    }
    if (bytes == state->c_api_params_charge_bytes && allocations == 1) {
        state->c_api_params_reservations++;
        if (state->deny_c_api_params) {
            state->deny_c_api_params = false;
            state->denied_calls++;
            return false;
        }
    }
    if (UINT64_MAX - state->live_bytes < bytes
        || UINT32_MAX - state->live_allocations < allocations) {
        state->denied_calls++;
        return false;
    }
    state->live_bytes += bytes;
    state->live_allocations += allocations;
    state->reserved_bytes += bytes;
    state->reserved_allocations += allocations;
    return true;
}

void
release_allocation(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *state = static_cast<NativePreEntryState *>(attachment);
    std::lock_guard<std::mutex> guard(state->lock);

    EXPECT_LE(bytes, state->live_bytes);
    EXPECT_LE(allocations, state->live_allocations);
    state->live_bytes -= bytes;
    state->live_allocations -= allocations;
    state->released_bytes += bytes;
    state->released_allocations += allocations;
}

bool
retain_attachment(void *attachment)
{
    auto *state = static_cast<NativePreEntryState *>(attachment);
    std::lock_guard<std::mutex> guard(state->lock);

    state->retain_calls++;
    return true;
}

void
release_attachment(void *attachment)
{
    auto *state = static_cast<NativePreEntryState *>(attachment);
    std::lock_guard<std::mutex> guard(state->lock);

    state->release_calls++;
}

bool
record_pre_entry(void *attachment)
{
    auto *state = static_cast<NativePreEntryState *>(attachment);
    std::lock_guard<std::mutex> guard(state->lock);

    state->pre_entry_calls++;
    return true;
}

void
raw_target(wasm_exec_env_t exec_env, uint64_t *)
{
    auto *state = static_cast<NativePreEntryState *>(
        wasm_runtime_get_function_attachment(exec_env));
    if (state) {
        std::lock_guard<std::mutex> guard(state->lock);
        state->raw_host_calls++;
    }
}

wasm_trap_t *
c_api_target(void *environment, const wasm_val_vec_t *, wasm_val_vec_t *)
{
    auto *state = static_cast<NativePreEntryState *>(environment);
    std::lock_guard<std::mutex> guard(state->lock);

    state->c_api_host_calls++;
    return nullptr;
}

class NativePreEntryTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        static NativeSymbol symbols[] = {
            { "target", reinterpret_cast<void *>(raw_target),
              "(iiiiiiiiiiiiiiiii)", nullptr },
        };
        std::ifstream input("native_pre_entry_17_params.wasm",
                            std::ios::binary);
        std::vector<uint8_t> binary{ std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>() };
        LoadArgs2 load_args;
        static char module_name[] = "native-pre-entry-17";

        helper.do_setup();
        ASSERT_TRUE(helper.runtime_init);
        ASSERT_TRUE(input.good() || input.eof());
        ASSERT_FALSE(binary.empty());
        ASSERT_TRUE(wasm_allocation_header_calculate_size(
            target_param_count * sizeof(uint64_t),
            WASM_ALLOCATION_MAX_ALIGNMENT, &state.raw_argv_charge_bytes));
        ASSERT_TRUE(wasm_allocation_header_calculate_size(
            target_param_count * sizeof(wasm_val_t),
            WASM_ALLOCATION_MAX_ALIGNMENT, &state.c_api_params_charge_bytes));
        ASSERT_TRUE(wasm_runtime_register_natives_raw(
            "test:quota/native", symbols,
            static_cast<uint32_t>(sizeof(symbols) / sizeof(symbols[0]))));

        wasm_runtime_load_args2_init(&load_args);
        load_args.name = module_name;
        load_args.clone_wasm_binary = true;
        wasm_runtime_load_args2_set_allocation_quota(
            &load_args, &state, reserve_allocation, release_allocation,
            retain_attachment, release_attachment);
        module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex2(
            binary.data(), static_cast<uint32_t>(binary.size()), &load_args,
            error, sizeof(error)));
        ASSERT_NE(module, nullptr) << error;

        instance = reinterpret_cast<WASMModuleInstance *>(
            wasm_runtime_instantiate(reinterpret_cast<wasm_module_t>(module),
                                     64 * 1024, 0, error, sizeof(error)));
        ASSERT_NE(instance, nullptr) << error;
        exec_env = wasm_runtime_get_exec_env_singleton(
            reinterpret_cast<WASMModuleInstanceCommon *>(instance));
        ASSERT_NE(exec_env, nullptr);
        function = reinterpret_cast<WASMFunctionInstance *>(
            wasm_runtime_lookup_function(
                reinterpret_cast<wasm_module_inst_t>(instance), "target"));
        ASSERT_NE(function, nullptr);
        ASSERT_TRUE(function->is_import_func);
        function->u.func_import->attachment = &state;

        for (auto &arg : args) {
            arg.kind = WASM_I32;
            arg.of.i32 = 7;
        }
    }

    void TearDown() override
    {
        if (exec_env) {
            os_mutex_lock(&exec_env->wait_lock);
            WASM_SUSPEND_FLAGS_FETCH_AND(exec_env->suspend_flags,
                                         ~WASM_SUSPEND_FLAG_TERMINATE);
            os_mutex_unlock(&exec_env->wait_lock);
        }
        if (instance) {
            wasm_runtime_deinstantiate(
                reinterpret_cast<wasm_module_inst_t>(instance));
            instance = nullptr;
        }
        if (module) {
            wasm_runtime_unload(reinterpret_cast<wasm_module_t>(module));
            module = nullptr;
        }
        helper.do_teardown();

        std::lock_guard<std::mutex> guard(state.lock);
        EXPECT_EQ(state.live_bytes, 0U);
        EXPECT_EQ(state.live_allocations, 0U);
        EXPECT_EQ(state.reserved_bytes, state.released_bytes);
        EXPECT_EQ(state.reserved_allocations, state.released_allocations);
        EXPECT_EQ(state.retain_calls, state.release_calls);
        EXPECT_FALSE(state.deny_raw_argv);
        EXPECT_FALSE(state.deny_c_api_params);
    }

    bool Call()
    {
        return wasm_runtime_call_wasm_a_with_pre_entry(
            exec_env, reinterpret_cast<WASMFunctionInstanceCommon *>(function),
            0, nullptr, target_param_count, args, record_pre_entry, &state);
    }

    void UseCApiImport()
    {
        WASMAllocationQuotaScope scope;

        ASSERT_EQ(instance->c_api_func_imports, nullptr);
        ASSERT_TRUE(
            wasm_allocation_quota_scope_enter_for_allocation(&scope, instance));
        instance->c_api_func_imports = static_cast<CApiFuncImport *>(
            wasm_runtime_malloc(sizeof(CApiFuncImport)));
        wasm_allocation_quota_scope_leave(&scope);
        ASSERT_NE(instance->c_api_func_imports, nullptr);
        std::memset(instance->c_api_func_imports, 0, sizeof(CApiFuncImport));
        instance->c_api_func_imports[0].func_ptr_linked =
            reinterpret_cast<void *>(c_api_target);
        instance->c_api_func_imports[0].with_env_arg = true;
        instance->c_api_func_imports[0].env_arg = &state;
        function->u.func_import->call_conv_raw = false;
        function->u.func_import->call_conv_wasm_c_api = true;
    }

    ComponentHelper helper;
    NativePreEntryState state;
    WASMModule *module = nullptr;
    WASMModuleInstance *instance = nullptr;
    WASMExecEnv *exec_env = nullptr;
    WASMFunctionInstance *function = nullptr;
    wasm_val_t args[target_param_count] = {};
    char error[256] = {};
};

TEST_F(NativePreEntryTest, RawSeventeenParamQuotaFailureDoesNotCommit)
{
    {
        std::lock_guard<std::mutex> guard(state.lock);
        state.deny_raw_argv = true;
    }
    EXPECT_FALSE(Call());
    std::lock_guard<std::mutex> guard(state.lock);
    EXPECT_EQ(state.denied_calls, 1U);
    EXPECT_EQ(state.raw_argv_reservations, 1U);
    EXPECT_EQ(state.pre_entry_calls, 0U);
    EXPECT_EQ(state.raw_host_calls, 0U);
}

TEST_F(NativePreEntryTest, CApiSeventeenParamQuotaFailureDoesNotCommit)
{
    UseCApiImport();
    {
        std::lock_guard<std::mutex> guard(state.lock);
        state.deny_c_api_params = true;
    }
    EXPECT_FALSE(Call());
    std::lock_guard<std::mutex> guard(state.lock);
    EXPECT_EQ(state.denied_calls, 1U);
    EXPECT_EQ(state.c_api_params_reservations, 1U);
    EXPECT_EQ(state.pre_entry_calls, 0U);
    EXPECT_EQ(state.c_api_host_calls, 0U);
}

TEST_F(NativePreEntryTest, TerminateBeforeRawEntryDoesNotCommitOrCallHost)
{
    os_mutex_lock(&exec_env->wait_lock);
    WASM_SUSPEND_FLAGS_FETCH_OR(exec_env->suspend_flags,
                                WASM_SUSPEND_FLAG_TERMINATE);
    os_mutex_unlock(&exec_env->wait_lock);

    EXPECT_FALSE(Call());
    std::lock_guard<std::mutex> guard(state.lock);
    EXPECT_EQ(state.pre_entry_calls, 0U);
    EXPECT_EQ(state.raw_host_calls, 0U);
}

} /* namespace */
