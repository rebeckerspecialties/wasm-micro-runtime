/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include "helpers.h"

namespace {

struct PageQuotaState {
    uint32_t limit = 0;
    uint32_t committed = 0;
    uint32_t peak = 0;
    uint32_t reserve_calls = 0;
    uint32_t release_calls = 0;
    uint32_t denied_calls = 0;
};

bool
reserve_pages(void *attachment, uint32_t pages)
{
    auto *state = static_cast<PageQuotaState *>(attachment);

    state->reserve_calls++;
    if (pages > state->limit - state->committed) {
        state->denied_calls++;
        return false;
    }
    state->committed += pages;
    state->peak = std::max(state->peak, state->committed);
    return true;
}

void
release_pages(void *attachment, uint32_t pages)
{
    auto *state = static_cast<PageQuotaState *>(attachment);

    ASSERT_LE(pages, state->committed);
    state->release_calls++;
    state->committed -= pages;
}

class ComponentMemoryPageQuotaTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();
        ASSERT_TRUE(helper->runtime_init);
    }

    void TearDown() override
    {
        helper->do_teardown();
        helper = nullptr;
    }

    void Load(const char *path)
    {
        ASSERT_TRUE(helper->read_wasm_file(path));
        ASSERT_TRUE(helper->load_component()) << helper->error_buf;
    }

    auto Instantiate(uint32_t per_memory_limit, PageQuotaState *quota)
        -> WASMComponentInstance *
    {
        struct InstantiationArgs2 *args = nullptr;

        EXPECT_TRUE(wasm_runtime_instantiation_args_create(&args));
        if (args == nullptr) {
            return nullptr;
        }
        wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
        wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 0);
        wasm_runtime_instantiation_args_set_max_memory_pages(args,
                                                             per_memory_limit);
        wasm_runtime_instantiation_args_set_memory_page_quota(
            args, quota, reserve_pages, release_pages);
        helper->component_inst = wasm_component_instantiate_ex2(
            helper->component, args, helper->error_buf,
            sizeof(helper->error_buf));
        wasm_runtime_instantiation_args_destroy(args);
        if (helper->component_inst != nullptr) {
            helper->component_instantiated = true;
        }
        return helper->component_inst;
    }

    std::unique_ptr<ComponentHelper> helper;
};

TEST_F(ComponentMemoryPageQuotaTest,
       RejectsShrunkFixedNestedCoreMemoryBeforeReservation)
{
    PageQuotaState quota{ 2 };

    Load("memory_page_quota_oversized.wasm");
    EXPECT_EQ(Instantiate(2, &quota), nullptr);
    EXPECT_NE(std::strstr(
                  helper->error_buf,
                  "effective initial memory pages 3 exceed configured limit 2"),
              nullptr)
        << helper->error_buf;
    EXPECT_EQ(quota.reserve_calls, 0U);
    EXPECT_EQ(quota.release_calls, 0U);
    EXPECT_EQ(quota.committed, 0U);
}

TEST_F(ComponentMemoryPageQuotaTest,
       RejectsAggregateNestedCoreMemoriesAndRollsBackReservations)
{
    PageQuotaState quota{ 3 };

    Load("memory_page_quota_aggregate.wasm");
    EXPECT_EQ(Instantiate(8, &quota), nullptr);
    EXPECT_NE(std::strstr(helper->error_buf,
                          "aggregate memory page quota denied 2 initial pages"),
              nullptr)
        << helper->error_buf;
    EXPECT_EQ(quota.reserve_calls, 2U);
    EXPECT_EQ(quota.denied_calls, 1U);
    EXPECT_EQ(quota.release_calls, 1U);
    EXPECT_EQ(quota.peak, 2U);
    EXPECT_EQ(quota.committed, 0U);
}

TEST_F(ComponentMemoryPageQuotaTest,
       ReservesGrowthAndReleasesCommittedPagesOnDeinstantiate)
{
    PageQuotaState quota{ 2 };

    Load("memory_page_quota_growth.wasm");
    ASSERT_NE(Instantiate(8, &quota), nullptr) << helper->error_buf;
    ASSERT_EQ(quota.committed, 1U);

    auto *call = wasm_component_prepare_export_call(helper->component_inst,
                                                    "grow", helper->error_buf,
                                                    sizeof(helper->error_buf));
    ASSERT_NE(call, nullptr) << helper->error_buf;
    wasm_val_t argument = {};
    wasm_val_t result = {};
    argument.kind = WASM_I32;
    argument.of.i32 = 1;

    ASSERT_TRUE(wasm_component_call_prepared(call, 1, &result, 1, &argument));
    EXPECT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, 1);
    EXPECT_EQ(quota.committed, 2U);
    EXPECT_EQ(quota.peak, 2U);
    EXPECT_TRUE(wasm_component_prepared_call_post_return(call));

    ASSERT_TRUE(wasm_component_call_prepared(call, 1, &result, 1, &argument));
    EXPECT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, -1);
    EXPECT_EQ(quota.denied_calls, 1U);
    EXPECT_EQ(quota.committed, 2U);
    EXPECT_TRUE(wasm_component_prepared_call_post_return(call));
    wasm_component_destroy_prepared_call(call);

    wasm_component_deinstantiate(helper->component_inst);
    helper->component_inst = nullptr;
    helper->component_instantiated = false;
    EXPECT_EQ(quota.release_calls, 1U);
    EXPECT_EQ(quota.committed, 0U);
}

TEST_F(ComponentMemoryPageQuotaTest,
       HostPageLimitStillCapsGrowthWhenAggregateQuotaIsLarger)
{
    PageQuotaState quota{ 8 };

    Load("memory_page_quota_growth.wasm");
    ASSERT_NE(Instantiate(2, &quota), nullptr) << helper->error_buf;
    ASSERT_EQ(quota.committed, 1U);

    auto *call = wasm_component_prepare_export_call(helper->component_inst,
                                                    "grow", helper->error_buf,
                                                    sizeof(helper->error_buf));
    ASSERT_NE(call, nullptr) << helper->error_buf;
    wasm_val_t argument = {};
    wasm_val_t result = {};
    argument.kind = WASM_I32;
    argument.of.i32 = 1;

    ASSERT_TRUE(wasm_component_call_prepared(call, 1, &result, 1, &argument));
    EXPECT_EQ(result.of.i32, 1);
    EXPECT_EQ(quota.committed, 2U);
    EXPECT_TRUE(wasm_component_prepared_call_post_return(call));

    ASSERT_TRUE(wasm_component_call_prepared(call, 1, &result, 1, &argument));
    EXPECT_EQ(result.of.i32, -1);
    EXPECT_EQ(quota.reserve_calls, 2U);
    EXPECT_EQ(quota.denied_calls, 0U);
    EXPECT_EQ(quota.committed, 2U);
    EXPECT_TRUE(wasm_component_prepared_call_post_return(call));
    wasm_component_destroy_prepared_call(call);

    wasm_component_deinstantiate(helper->component_inst);
    helper->component_inst = nullptr;
    helper->component_instantiated = false;
    EXPECT_EQ(quota.committed, 0U);
}

} /* namespace */
