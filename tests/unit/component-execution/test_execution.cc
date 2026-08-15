/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include "helpers.h"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
bool reject_runtime_allocations = false;
uint64_t runtime_allocation_attempts = 0;

struct NestedHostResourceState {
    uint32_t make_calls = 0;
    uint32_t rep_calls = 0;
    uint32_t drop_calls = 0;
    uint32_t dropped_representation = 0;
    bool saw_root_custom_data = false;
};

static constexpr uint32_t nested_host_representation = 0xC0DE;

void
nested_host_make(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    auto *state = static_cast<NestedHostResourceState *>(
        wasm_component_get_custom_data_from_exec_env(exec_env));
    uint32_t handle = 0;

    if (state) {
        state->make_calls++;
        state->saw_root_custom_data = true;
    }
    canonical_cells[0] = wasm_component_host_resource_new(
                             exec_env, "test:nested/host@1.0.0", "item",
                             nested_host_representation, &handle)
                             ? handle
                             : 0;
}

void
nested_host_rep(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    auto *state = static_cast<NestedHostResourceState *>(
        wasm_component_get_custom_data_from_exec_env(exec_env));
    uint32_t representation = 0;

    if (state) {
        state->rep_calls++;
        state->saw_root_custom_data = true;
    }
    canonical_cells[0] =
        wasm_component_host_resource_rep(
            exec_env, "test:nested/host@1.0.0", "item",
            static_cast<uint32_t>(canonical_cells[0]), &representation)
            ? representation
            : 0;
}

bool
nested_host_drop(void *attachment, uint32_t representation)
{
    auto *state = static_cast<NestedHostResourceState *>(attachment);

    if (!state) {
        return false;
    }
    state->drop_calls++;
    state->dropped_representation = representation;
    return true;
}

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

    virtual void SetUp()
    {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();
    }

    virtual void TearDown()
    {
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

    ret = wasm_component_application_execute_func_ex(
        this->helper->component_inst, (char *)"add(3,4)", &argc1, &argv1);

    // Check results
    bool function_succeeded =
        ret
        && !wasm_component_runtime_get_exception(this->helper->component_inst);

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
    ASSERT_EQ(result, 7U); // Result should be 7
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

// Prepared flat calls avoid WAVE parsing, WIT-value lifting/lowering, and
// adapter allocation after the export and execution environment are cached.
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

    // Preserve the original leaf-name preparation path for compatibility.
    prepared = wasm_component_prepare_export_call(helper.component_inst, "add",
                                                  error_buf, sizeof(error_buf));
    ASSERT_NE(prepared, nullptr) << error_buf;
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

TEST_F(ComponentPreparedCallAllocationTest,
       OutstandingPreparedCallAtomicallyRejectsDeinstantiate)
{
    ASSERT_TRUE(helper.read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(helper.instantiate_component());

    ASSERT_GT(helper.component_inst->defined_instances_count, 0u);
    WASMComponentInstance *nested = helper.component_inst->defined_instances[0];
    ASSERT_NE(nested, nullptr);
    ASSERT_EQ(nested->lifecycle_root, helper.component_inst);
    ASSERT_TRUE(wasm_component_instance_begin_operation(nested));
    EXPECT_EQ(helper.component_inst->lifecycle_active_operations, 1u);
    wasm_component_deinstantiate(helper.component_inst);
    EXPECT_FALSE(helper.component_inst->deinstantiating);
    wasm_component_instance_end_operation(nested);
    EXPECT_EQ(helper.component_inst->lifecycle_active_operations, 0u);

    prepared = wasm_component_prepare_export_call(helper.component_inst, "add",
                                                  error_buf, sizeof(error_buf));
    ASSERT_NE(prepared, nullptr) << error_buf;
    ASSERT_EQ(helper.component_inst->outstanding_prepared_calls, 1u);

    std::thread teardown_attempt(
        [this] { wasm_component_deinstantiate(helper.component_inst); });
    teardown_attempt.join();

    const char *exception =
        wasm_component_runtime_get_exception(helper.component_inst);
    ASSERT_NE(exception, nullptr);
    EXPECT_NE(strstr(exception, "outstanding prepared calls"), nullptr);

    wasm_val_t args[2] = {};
    wasm_val_t result = {};
    args[0].kind = WASM_I32;
    args[0].of.i32 = 8;
    args[1].kind = WASM_I32;
    args[1].of.i32 = 5;
    ASSERT_TRUE(wasm_component_call_prepared(prepared, 1, &result, 2, args));
    EXPECT_EQ(result.of.i32, 13);
    ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared));

    wasm_component_destroy_prepared_call(prepared);
    prepared = nullptr;
    EXPECT_EQ(helper.component_inst->outstanding_prepared_calls, 0u);
}

TEST_F(ComponentExecutionTest, RootOperationsAreRecursiveAndSerialized)
{
    ASSERT_TRUE(helper->read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper->load_component());
    ASSERT_TRUE(helper->instantiate_component());
    ASSERT_GT(helper->component_inst->defined_instances_count, 0u);

    WASMComponentInstance *root = helper->component_inst;
    WASMComponentInstance *nested = root->defined_instances[0];
    ASSERT_NE(nested, nullptr);
    ASSERT_EQ(nested->lifecycle_root, root);

    /* Same-thread host callbacks may reenter a root operation. */
    ASSERT_TRUE(wasm_component_instance_begin_operation(root));
    ASSERT_TRUE(wasm_component_instance_begin_operation(nested));
    EXPECT_EQ(root->lifecycle_active_operations, 2u);
    wasm_component_instance_end_operation(nested);
    EXPECT_EQ(root->lifecycle_active_operations, 1u);

    std::atomic<bool> second_entered{ false };
    std::thread second([&] {
        bool entered = wasm_component_instance_begin_operation(nested);
        second_entered.store(entered, std::memory_order_release);
        if (entered) {
            wasm_component_instance_end_operation(nested);
        }
    });

    /* The lifecycle-gate user count reaches two before the second thread
     * waits on the operation lock, making the blocked state deterministic. */
    constexpr uint32_t gate_users_mask = 0x7FFFFFFFu;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((BH_ATOMIC_32_LOAD(root->lifecycle_gate_state) & gate_users_mask) < 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_GE(BH_ATOMIC_32_LOAD(root->lifecycle_gate_state) & gate_users_mask,
              2u);
    EXPECT_FALSE(second_entered.load(std::memory_order_acquire));
    EXPECT_EQ(root->lifecycle_active_operations, 1u);

    wasm_component_instance_end_operation(root);
    second.join();
    EXPECT_TRUE(second_entered.load(std::memory_order_acquire));
    EXPECT_EQ(root->lifecycle_active_operations, 0u);
}

TEST_F(ComponentExecutionTest, NestedHostResourceUsesCallingComponentTable)
{
    NativeSymbol host_symbols[] = {
        { "make", reinterpret_cast<void *>(nested_host_make), "()i", nullptr },
        { "rep", reinterpret_cast<void *>(nested_host_rep), "(i)i", nullptr },
    };
    NestedHostResourceState state;

    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:nested/host@1.0.0", host_symbols,
        static_cast<uint32_t>(sizeof(host_symbols) / sizeof(host_symbols[0]))));
    ASSERT_TRUE(helper->read_wasm_file("nested_host_resource.wasm"));
    ASSERT_TRUE(helper->load_component());
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper->component, "test:nested/host@1.0.0", "item", nested_host_drop));

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    ASSERT_NE(args, nullptr);
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_custom_data(args, &state);
    helper->component_inst = wasm_component_instantiate_ex2(
        helper->component, args, helper->error_buf, sizeof(helper->error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper->component_inst, nullptr) << helper->error_buf;
    helper->component_instantiated = true;

    WASMComponentPreparedCall *call = wasm_component_prepare_export_call(
        helper->component_inst, "round-trip", helper->error_buf,
        sizeof(helper->error_buf));
    ASSERT_NE(call, nullptr) << helper->error_buf;
    wasm_val_t result = {};
    ASSERT_TRUE(wasm_component_call_prepared(call, 1, &result, 0, nullptr));
    EXPECT_TRUE(wasm_component_prepared_call_post_return(call));
    wasm_component_destroy_prepared_call(call);

    EXPECT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, static_cast<int32_t>(nested_host_representation));
    EXPECT_EQ(state.make_calls, 1U);
    EXPECT_EQ(state.rep_calls, 1U);
    EXPECT_EQ(state.drop_calls, 1U);
    EXPECT_EQ(state.dropped_representation, nested_host_representation);
    EXPECT_TRUE(state.saw_root_custom_data);
}

TEST_F(ComponentExecutionTest, TestRepeatedLoadExecuteAndUnload)
{
    for (uint32_t i = 0; i < 8; i++) {
        ASSERT_TRUE(helper->read_wasm_file("add.wasm"));
        ASSERT_TRUE(helper->load_component());
        ASSERT_TRUE(helper->instantiate_component());

        uint32 argc = 0;
        uint32 *argv = (uint32 *)wasm_runtime_malloc(sizeof(uint32) * 10);
        ASSERT_NE(argv, nullptr);

        bool ret = wasm_component_application_execute_func_ex(
            helper->component_inst, (char *)"add(3,4)", &argc, &argv);
        ASSERT_TRUE(ret);
        ASSERT_EQ(wasm_component_runtime_get_exception(helper->component_inst),
                  nullptr);
        ASSERT_GT(argc, 0U);
        EXPECT_EQ(argv[0], 7U);

        wasm_runtime_free(argv);
        wasm_component_deinstantiate(helper->component_inst);
        helper->component_inst = nullptr;
        helper->component_instantiated = false;
        helper->reset_component();
        if (helper->component_raw) {
            BH_FREE(helper->component_raw);
            helper->component_raw = nullptr;
        }
    }
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
    ret = wasm_component_application_execute_func_ex(
        this->helper->component_inst, (char *)"random_func(3,4)", &argc1,
        &argv1);

    // Should fail
    ASSERT_FALSE(ret);

    // Should have exception message
    const char *exception =
        wasm_component_runtime_get_exception(this->helper->component_inst);
    ASSERT_TRUE(exception != NULL);
    ASSERT_TRUE(
        strstr(exception, "Exception: lookup function random_func failed")
        != NULL);

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
    const char *exception =
        wasm_component_runtime_get_exception(this->helper->component_inst);
    ASSERT_TRUE(exception != NULL);
    ASSERT_TRUE(
        strstr(exception, "This method waited 2 arguments, but received 3\n")
        != NULL);

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
    ret = wasm_component_application_execute_func_ex(
        this->helper->component_inst, (char *)"div(10, 0)", &argc1, &argv1);

    // Should fail due to trap
    ASSERT_FALSE(ret);

    // Should have exception message about the trap
    const char *exception =
        wasm_component_runtime_get_exception(this->helper->component_inst);

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
    ret = wasm_component_application_execute_func_ex(
        this->helper->component_inst, (char *)"fdiv(10.0, 0.0)", &argc1,
        &argv1);

    // Should succeed (no trap for float division by zero)
    bool function_succeeded =
        ret
        && !wasm_component_runtime_get_exception(this->helper->component_inst);

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
    ASSERT_TRUE(result > 0); // Positive infinity
}
