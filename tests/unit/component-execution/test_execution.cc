/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include "helpers.h"
#include <iostream>
#include <cmath>
#include <cstdlib>

namespace {
bool reject_runtime_allocations = false;
uint64_t runtime_allocation_attempts = 0;

void *
prepared_call_test_malloc(unsigned int size)
{
    runtime_allocation_attempts++;
    return reject_runtime_allocations ? nullptr : std::malloc(size);
}

void *
prepared_call_test_realloc(void *ptr, unsigned int size)
{
    runtime_allocation_attempts++;
    return reject_runtime_allocations ? nullptr : std::realloc(ptr, size);
}

void
prepared_call_test_free(void *ptr)
{
    std::free(ptr);
}
} // namespace

class ComponentExecutionTest : public testing::Test
{
  public:
    ComponentExecutionTest() {}
    ~ComponentExecutionTest() {}
    RuntimeInitArgs init_args;
    unsigned char *component_raw = NULL;
    bool exception = false;
    std::unique_ptr<ComponentHelper> helper;

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;

    virtual void SetUp() {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();
    }

    virtual void TearDown() {
        helper->do_teardown();
        helper = nullptr;
    }
};

// Test correct call on add(3, 4) method
TEST_F(ComponentExecutionTest, TestAddWASM)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file((std::string("add.wasm").c_str()));
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->component_raw != NULL);

    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ret = helper->instantiate_component();
    ASSERT_TRUE(ret);

    uint32 argc1 = 0;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
    ASSERT_TRUE(argv1 != NULL);

    ret = wasm_component_application_execute_func_ex(this->helper->component_inst, (char*)"add(3,4)", &argc1, &argv1);

    // Check results
    bool function_succeeded = ret && !wasm_component_runtime_get_exception(this->helper->component_inst);

    uint32 result = 0;
    if (function_succeeded && argv1) {
        result = argv1[0];
    }

    // Clean up
    if (argv1) {
        wasm_runtime_free(argv1);
        argv1 = NULL;
    }


    ASSERT_TRUE(function_succeeded);

    ASSERT_GT(argc1, 0U);  // Should have at least one result cell
    ASSERT_EQ(result, 7U);  // Result should be 7
}

class ComponentPreparedCallAllocationTest : public testing::Test
{
  public:
    ComponentHelper helper;
    WASMComponentPreparedCall *prepared = nullptr;
    char error_buf[128] = {};

    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_Allocator;
        init_args.mem_alloc_option.allocator.malloc_func =
            reinterpret_cast<void *>(prepared_call_test_malloc);
        init_args.mem_alloc_option.allocator.realloc_func =
            reinterpret_cast<void *>(prepared_call_test_realloc);
        init_args.mem_alloc_option.allocator.free_func =
            reinterpret_cast<void *>(prepared_call_test_free);
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        helper.runtime_init = true;
    }

    void TearDown() override
    {
        reject_runtime_allocations = false;
        if (prepared) {
            wasm_component_prepared_call_post_return(prepared);
            wasm_component_destroy_prepared_call(prepared);
            prepared = nullptr;
        }
        helper.do_teardown();
    }
};

TEST_F(ComponentPreparedCallAllocationTest,
       TestPreparedFlatAddRepeatedWithoutAllocations)
{
    static constexpr const char *interface_name =
        "test:project/my-interface@0.1.0";

    ASSERT_TRUE(helper.read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(helper.instantiate_component());

    wasm_val_t args[2] = {};
    wasm_val_t results[1] = {};
    args[0].kind = WASM_I32;
    args[1].kind = WASM_I32;

    prepared = wasm_component_prepare_export_call(helper.component_inst, "add",
                                                  error_buf, sizeof(error_buf));
    ASSERT_NE(prepared, nullptr) << error_buf;
    EXPECT_FALSE(wasm_component_prepared_call_requires_post_return(nullptr));
    EXPECT_FALSE(wasm_component_prepared_call_requires_post_return(prepared));
    args[0].of.i32 = 3;
    args[1].of.i32 = 4;
    ASSERT_TRUE(wasm_component_call_prepared(prepared, 1, results, 2, args));
    ASSERT_EQ(results[0].of.i32, 7);
    ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared));
    wasm_component_destroy_prepared_call(prepared);
    prepared = nullptr;

    WASMComponentPreparedCall *wrong_interface =
        wasm_component_prepare_export_call_qualified(
            helper.component_inst, "test:project/my-interface@0.2.0", "add",
            error_buf, sizeof(error_buf));
    ASSERT_EQ(wrong_interface, nullptr);
    ASSERT_NE(strstr(error_buf, "qualified prepared export lookup failed"),
              nullptr);

    prepared = wasm_component_prepare_export_call_qualified(
        helper.component_inst, interface_name, "add", error_buf,
        sizeof(error_buf));
    ASSERT_NE(prepared, nullptr) << error_buf;

    runtime_allocation_attempts = 0;
    reject_runtime_allocations = true;

    for (uint32_t i = 0; i < 10000; i++) {
        args[0].of.i32 = static_cast<int32_t>(i);
        args[1].of.i32 = 7;
        ASSERT_TRUE(
            wasm_component_call_prepared(prepared, 1, results, 2, args));
        ASSERT_EQ(results[0].kind, WASM_I32);
        ASSERT_EQ(results[0].of.i32, static_cast<int32_t>(i + 7));
        ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared));
    }

    reject_runtime_allocations = false;
    EXPECT_EQ(runtime_allocation_attempts, 0U);
    wasm_component_destroy_prepared_call(prepared);
    prepared = nullptr;
}

// Call non-existent function
TEST_F(ComponentExecutionTest, TestCallNonExistentFunction)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ret = helper->instantiate_component();
    ASSERT_TRUE(ret);

    uint32 argc1 = 0;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
    ASSERT_TRUE(argv1 != NULL);

    // Call non-existent function
    ret = wasm_component_application_execute_func_ex(this->helper->component_inst, (char*)"random_func(3,4)", &argc1, &argv1);

    // Should fail
    ASSERT_FALSE(ret);

    // Should have exception message
    const char* exception = wasm_component_runtime_get_exception(this->helper->component_inst);
    ASSERT_TRUE(exception != NULL);
    ASSERT_TRUE(strstr(exception, "Exception: lookup function random_func failed") != NULL);

    // Cleanup
    wasm_runtime_free(argv1);
}

// Call with wrong parameter count
TEST_F(ComponentExecutionTest, TestCallWithWrongParameterCount)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ret = helper->instantiate_component();
    ASSERT_TRUE(ret);


    uint32 argc1 = 0;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
    ASSERT_TRUE(argv1 != NULL);

    // add function expects 2 parameters, but provide 3
    ret = wasm_component_application_execute_func_ex(
        this->helper->component_inst, (char *)"add(3,4,5)", &argc1, &argv1);

    // Should fail
    ASSERT_FALSE(ret);

    // Should have exception message about argument count
    const char* exception = wasm_component_runtime_get_exception(this->helper->component_inst);
    ASSERT_TRUE(exception != NULL);
    ASSERT_TRUE(strstr(exception, "This method waited 2 arguments, but received 3\n") != NULL);

    // Cleanup
    wasm_runtime_free(argv1);
}

// Call div with zero to trigger trap
TEST_F(ComponentExecutionTest, TestIntDivideByZeroTrap)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ret = helper->instantiate_component();
    ASSERT_TRUE(ret);

    uint32 argc1 = 0;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
    ASSERT_TRUE(argv1 != NULL);

    // Call div with divisor = 0
    ret = wasm_component_application_execute_func_ex(this->helper->component_inst, (char *)"div(10, 0)", &argc1, &argv1);

    // Should fail due to trap
    ASSERT_FALSE(ret);

    // Should have exception message about the trap
    const char* exception = wasm_component_runtime_get_exception(this->helper->component_inst);

    ASSERT_TRUE(exception != NULL);
    ASSERT_TRUE(strstr(exception, "integer divide by zero") != NULL);

    // Cleanup
    wasm_runtime_free(argv1);
}

// Test float division by zero returns infinity
TEST_F(ComponentExecutionTest, TestFloatDivideByZeroReturnsInfinity)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ret = helper->instantiate_component();
    ASSERT_TRUE(ret);

    uint32 argc1 = 0;
    uint32 *argv1 = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
    ASSERT_TRUE(argv1 != NULL);

    // Call fdiv with divisor = 0.0
    ret = wasm_component_application_execute_func_ex(this->helper->component_inst, (char *)"fdiv(10.0, 0.0)", &argc1, &argv1);

    // Should succeed (no trap for float division by zero)
    bool function_succeeded = ret && !wasm_component_runtime_get_exception(this->helper->component_inst);

    float result = 0.0f;
    if (function_succeeded && argv1) {
        result = *((float *)argv1);
    }

    // Cleanup
    wasm_runtime_free(argv1);

    // Assert function succeeded
    ASSERT_TRUE(function_succeeded);

    // Assert result is infinity
    ASSERT_TRUE(isinf(result));
    ASSERT_TRUE(result > 0);  // Positive infinity
}
