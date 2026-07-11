/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>

#include "helpers.h"

namespace {

enum class ProbeMode {
    ValidRange,
    InvalidRanges,
};

struct MemoryProbeState {
    ProbeMode mode = ProbeMode::ValidRange;
    wasm_exec_env_t callback_exec_env = nullptr;
    uint32_t contents_offset = 0;
    uint32_t contents_size = 0;
    bool callback_called = false;
    bool validate_succeeded = false;
    bool mutable_succeeded = false;
    bool const_succeeded = false;
    bool pointers_match = false;
    bool mutable_round_trip_succeeded = false;
    bool empty_range_succeeded = false;
    bool null_output_rejected = false;
    bool bounds_rejected = false;
    bool overflow_rejected = false;
    bool failed_outputs_cleared = false;
};

MemoryProbeState *memory_probe_state;

void
host_log(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    MemoryProbeState *state = memory_probe_state;
    uint8_t *mutable_data = nullptr;
    const uint8_t *const_data = nullptr;
    uint32_t level = static_cast<uint32_t>(canonical_cells[0]);
    uint32_t contents_offset = static_cast<uint32_t>(canonical_cells[1]);
    uint32_t contents_size = static_cast<uint32_t>(canonical_cells[2]);

    (void)level;

    if (state == nullptr) {
        return;
    }

    state->callback_called = true;
    state->callback_exec_env = exec_env;
    state->contents_offset = contents_offset;
    state->contents_size = contents_size;

    if (state->mode == ProbeMode::InvalidRanges) {
        auto *mutable_sentinel = reinterpret_cast<uint8_t *>(1);
        const auto *const_sentinel = reinterpret_cast<const uint8_t *>(1);

        state->bounds_rejected =
            !wasm_component_validate_memory_range(exec_env, 0, UINT32_MAX)
            && !wasm_component_get_memory_range(exec_env, 0, UINT32_MAX,
                                                &mutable_sentinel);
        state->overflow_rejected =
            !wasm_component_validate_memory_range(exec_env, UINT32_MAX, 2)
            && !wasm_component_get_memory_range_const(exec_env, UINT32_MAX, 2,
                                                      &const_sentinel);
        state->failed_outputs_cleared =
            mutable_sentinel == nullptr && const_sentinel == nullptr;
        return;
    }

    state->validate_succeeded = wasm_component_validate_memory_range(
        exec_env, contents_offset, contents_size);
    state->mutable_succeeded = wasm_component_get_memory_range(
        exec_env, contents_offset, contents_size, &mutable_data);
    state->const_succeeded = wasm_component_get_memory_range_const(
        exec_env, contents_offset, contents_size, &const_data);
    state->pointers_match = mutable_data != nullptr && const_data != nullptr
                            && mutable_data == const_data && contents_size > 0;

    if (state->pointers_match) {
        uint8_t original = mutable_data[0];
        mutable_data[0] ^= 0x20;
        state->mutable_round_trip_succeeded =
            const_data[0] == static_cast<uint8_t>(original ^ 0x20);
        mutable_data[0] = original;
    }

    if ((uint64_t)contents_offset + contents_size <= UINT32_MAX) {
        state->empty_range_succeeded = wasm_component_validate_memory_range(
            exec_env, contents_offset + contents_size, 0);
    }
    state->null_output_rejected = !wasm_component_get_memory_range(
        exec_env, contents_offset, contents_size, nullptr);
}

std::array<NativeSymbol, 1> host_symbols = { {
    { "log", reinterpret_cast<void *>(host_log), "(iii)", nullptr },
} };

class ComponentHostMemoryTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();
        ASSERT_TRUE(helper->runtime_init);
        ASSERT_TRUE(wasm_runtime_register_natives_raw(
            "test:project/imported-interface@0.1.0", host_symbols.data(),
            static_cast<uint32_t>(host_symbols.size())));
    }

    void TearDown() override
    {
        memory_probe_state = nullptr;
        helper->do_teardown();
        helper = nullptr;
    }

    void LoadComponent()
    {
        ASSERT_TRUE(helper->read_wasm_file("complex_with_host.wasm"));
        ASSERT_TRUE(helper->load_component()) << helper->error_buf;
        ASSERT_TRUE(helper->instantiate_component()) << helper->error_buf;
    }

    auto ExecuteProbe(MemoryProbeState *state) -> bool
    {
        uint32_t result_cell_count = 0;
        auto *result_cells =
            static_cast<uint32_t *>(wasm_runtime_malloc(sizeof(uint32_t) * 16));

        if (result_cells == nullptr) {
            return false;
        }

        memory_probe_state = state;
        bool succeeded = wasm_component_application_execute_func_ex(
            helper->component_inst, const_cast<char *>("process-people([])"),
            &result_cell_count, &result_cells);
        memory_probe_state = nullptr;
        wasm_runtime_free(result_cells);
        return succeeded;
    }

  private:
    std::unique_ptr<ComponentHelper> helper;
};

TEST_F(ComponentHostMemoryTest, ResolvesMutableAndConstCallbackRanges)
{
    MemoryProbeState state;
    auto *stale_mutable = reinterpret_cast<uint8_t *>(1);
    const auto *stale_const = reinterpret_cast<const uint8_t *>(1);

    LoadComponent();
    EXPECT_TRUE(ExecuteProbe(&state));

    EXPECT_TRUE(state.callback_called);
    EXPECT_TRUE(state.validate_succeeded);
    EXPECT_TRUE(state.mutable_succeeded);
    EXPECT_TRUE(state.const_succeeded);
    EXPECT_TRUE(state.pointers_match);
    EXPECT_TRUE(state.mutable_round_trip_succeeded);
    EXPECT_TRUE(state.empty_range_succeeded);
    EXPECT_TRUE(state.null_output_rejected);

    ASSERT_NE(state.callback_exec_env, nullptr);
    EXPECT_FALSE(wasm_component_validate_memory_range(
        state.callback_exec_env, state.contents_offset, state.contents_size));
    EXPECT_FALSE(wasm_component_get_memory_range(
        state.callback_exec_env, state.contents_offset, state.contents_size,
        &stale_mutable));
    EXPECT_FALSE(wasm_component_get_memory_range_const(
        state.callback_exec_env, state.contents_offset, state.contents_size,
        &stale_const));
    EXPECT_EQ(stale_mutable, nullptr);
    EXPECT_EQ(stale_const, nullptr);
}

TEST_F(ComponentHostMemoryTest, RejectsBoundsOverflowAndWrongContext)
{
    MemoryProbeState state;
    auto *null_context_output = reinterpret_cast<uint8_t *>(1);

    state.mode = ProbeMode::InvalidRanges;
    LoadComponent();
    EXPECT_FALSE(ExecuteProbe(&state));

    EXPECT_TRUE(state.callback_called);
    EXPECT_TRUE(state.bounds_rejected);
    EXPECT_TRUE(state.overflow_rejected);
    EXPECT_TRUE(state.failed_outputs_cleared);
    EXPECT_FALSE(wasm_component_validate_memory_range(nullptr, 0, 0));
    EXPECT_FALSE(
        wasm_component_get_memory_range(nullptr, 0, 0, &null_context_output));
    EXPECT_EQ(null_context_output, nullptr);
}

} /* namespace */
