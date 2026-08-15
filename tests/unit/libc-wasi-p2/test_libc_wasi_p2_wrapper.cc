/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include "gtest/gtest.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "wasm_native.h"
#include "libc_wasi_p2_wrapper.h"
#include "wasi_p2_common.h"
#include "test_helper.h"
#include "component-model/wasm_component_resource.h"
#include "component-model/wasm_component_resource_table.h"

static uint32_t failed_lower_resource_drop_count;

static void
count_failed_lower_resource_drop(void *)
{
    failed_lower_resource_drop_count++;
}

class WasiP2WrapperTest : public testing::Test {
protected:
    void SetUp() override
    {
        runtime_ = std::make_unique<WAMRRuntimeRAII<>>();
    }

    void TearDown() override
    {
        runtime_.reset();
    }

    std::unique_ptr<WAMRRuntimeRAII<>> runtime_;
};

class ScopedWasiP2ModuleVersion
{
  public:
    ScopedWasiP2ModuleVersion(wasi_p2_module_t *module, const char *version)
      : module_(module)
      , original_version_(module->version)
    {
        module_->version = version;
    }

    ~ScopedWasiP2ModuleVersion() { module_->version = original_version_; }

  private:
    wasi_p2_module_t *module_;
    const char *original_version_;
};

TEST_F(WasiP2WrapperTest, VersionCompatibilityUsesRuntimePatchAsUpperBound)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t module_count = get_libc_wasi_p2_export_apis(&modules);
    wasi_p2_module_t *io_poll_module = NULL;

    for (uint32_t i = 0; i < module_count; i++) {
        if (strcmp(modules[i].module_name, "wasi:io/poll") == 0) {
            io_poll_module = &modules[i];
            break;
        }
    }

    ASSERT_NE(io_poll_module, nullptr);
    ScopedWasiP2ModuleVersion version(io_poll_module, "0.2.6");

    EXPECT_TRUE(wasm_check_wasi_p2_version("wasi:io/poll"));
    EXPECT_TRUE(wasm_check_wasi_p2_version("wasi:io/poll@0.2.0"));
    EXPECT_TRUE(wasm_check_wasi_p2_version("wasi:io/poll@0.2.3"));
    EXPECT_TRUE(wasm_check_wasi_p2_version("wasi:io/poll@0.2.6"));
    EXPECT_FALSE(wasm_check_wasi_p2_version("wasi:io/poll@0.2.7"));
    EXPECT_FALSE(wasm_check_wasi_p2_version("wasi:io/poll@0.1.6"));
    EXPECT_FALSE(wasm_check_wasi_p2_version("wasi:io/poll@1.2.6"));
    EXPECT_FALSE(wasm_check_wasi_p2_version("wasi:io/poll@0.2"));
    EXPECT_FALSE(wasm_check_wasi_p2_version("wasi:io/poll@0.2.x"));
}

TEST_F(WasiP2WrapperTest, BuiltInModulesDeclareImplementedPackageVersion)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t module_count = get_libc_wasi_p2_export_apis(&modules);

    ASSERT_GT(module_count, 0u);
    for (uint32_t i = 0; i < module_count; i++) {
        EXPECT_STREQ(modules[i].version, "0.2.6") << modules[i].module_name;
    }
}

TEST_F(WasiP2WrapperTest, OfflineCommonsFailedOwnedLoweringReclaimsHostResource)
{
    ASSERT_TRUE(instantiate_host_resource_table());
    HostResourceTable *table = get_global_host_resource_table();
    ASSERT_NE(table, nullptr);

    HostResource *resource = host_resource_create(WASI_P2_POLLABLE, 1);
    ASSERT_NE(resource, nullptr);
    failed_lower_resource_drop_count = 0;
    host_resource_set_dtor(resource, count_failed_lower_resource_drop);

    uint32_t rep = host_resource_table_add(table, resource);
    ASSERT_NE(rep, 0u);
    uint32_t lowered_index = UINT32_MAX;

    EXPECT_FALSE(lower_owned_host_resource(nullptr, nullptr, table, rep,
                                           &lowered_index));
    EXPECT_EQ(host_resource_table_get(table, rep), nullptr);
    EXPECT_EQ(failed_lower_resource_drop_count, 1u);
    EXPECT_EQ(lowered_index, UINT32_MAX);

    destroy_host_resource_table();
}

struct FailedOwnedResultQuotaState {
    uint32_t active = 0;
    uint32_t releases = 0;
    uint32_t closes = 0;
    bool underflow = false;
};

struct FailedOwnedResultFd {
    FailedOwnedResultQuotaState *state;
    int fd;
};

static void
close_failed_owned_result_fd(void *data)
{
    auto *owned_fd = static_cast<FailedOwnedResultFd *>(data);

    if (owned_fd && owned_fd->state && owned_fd->fd >= 0) {
        close(owned_fd->fd);
        owned_fd->fd = -1;
        owned_fd->state->closes++;
    }
}

static void
release_failed_owned_result_quota(void *attachment, uint32_t fd_count)
{
    auto *state = static_cast<FailedOwnedResultQuotaState *>(attachment);

    if (!state || fd_count > state->active) {
        if (state) {
            state->underflow = true;
        }
        return;
    }
    state->active -= fd_count;
    state->releases++;
}

class WasiP2OwnedResultRollbackTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        runtime_ = std::make_unique<WAMRRuntimeRAII<>>();
        ASSERT_TRUE(instantiate_host_resource_table());
        host_table_ = get_global_host_resource_table();
        ASSERT_NE(host_table_, nullptr);

        component_.table = wasm_component_table_init(4, 100);
        ASSERT_NE(component_.table, nullptr);
        context_.inst = &component_;
        exec_env_.cx = &context_;
        exec_env_.component_inst = &component_;
        resource_type_.is_builtin_wasi = true;
        handle_type_.resource = &resource_type_;
    }

    void TearDown() override
    {
        if (component_.table) {
            wasm_component_table_destroy(component_.table);
            component_.table = nullptr;
        }
        destroy_host_resource_table();
        EXPECT_FALSE(quota_.underflow);
        EXPECT_EQ(quota_.active, 0u);
        runtime_.reset();
    }

    uint32_t AddMeteredFd(int fd, HostResourceType type)
    {
        HostResource *resource =
            host_resource_create(type, sizeof(FailedOwnedResultFd));
        if (!resource) {
            close(fd);
            return 0;
        }

        auto *owned_fd = static_cast<FailedOwnedResultFd *>(resource->data);
        owned_fd->state = &quota_;
        owned_fd->fd = fd;
        host_resource_set_dtor(resource, close_failed_owned_result_fd);
        quota_.active++;
        host_resource_set_native_fd_quota(resource, &quota_,
                                          release_failed_owned_result_quota, 1);

        uint32_t rep = host_resource_table_add(host_table_, resource);
        if (rep == 0) {
            destroy_host_resource(resource);
        }
        return rep;
    }

    bool LowerOwn(uint32_t rep, uint32_t *out_index)
    {
        wit_value_t value = wit_resource_ctor(rep);
        if (!value) {
            return false;
        }
        bool lowered = lower_own(&context_, &handle_type_, value, out_index);
        free_wit_value(value);
        return lowered;
    }

    std::unique_ptr<WAMRRuntimeRAII<>> runtime_;
    HostResourceTable *host_table_ = nullptr;
    WASMComponentInstance component_ = {};
    LiftLowerContext context_ = {};
    WASMExecEnv exec_env_ = {};
    WASMComponentResourceInstance resource_type_ = {};
    WASMComponentResourceHandleInstance handle_type_ = {};
    FailedOwnedResultQuotaState quota_;
};

TEST_F(WasiP2OwnedResultRollbackTest, FailureBeforeFirstOwnRefundsEveryResource)
{
    int pipe_fds[2] = { -1, -1 };
    ASSERT_EQ(pipe(pipe_fds), 0);
    uint32_t reps[] = {
        AddMeteredFd(pipe_fds[0], WASI_P2_IO_INPUT_STREAM),
        AddMeteredFd(pipe_fds[1], WASI_P2_IO_OUTPUT_STREAM),
    };
    ASSERT_NE(reps[0], 0u);
    ASSERT_NE(reps[1], 0u);
    ASSERT_EQ(quota_.active, 2u);

    EXPECT_FALSE(wasi_p2_store_owned_host_resource_result(
        &exec_env_, 0, nullptr, nullptr, reps, 2));

    EXPECT_EQ(host_resource_table_get(host_table_, reps[0]), nullptr);
    EXPECT_EQ(host_resource_table_get(host_table_, reps[1]), nullptr);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(quota_.releases, 2u);
    EXPECT_EQ(quota_.closes, 2u);
    errno = 0;
    EXPECT_EQ(fcntl(pipe_fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(fcntl(pipe_fds[1], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(WasiP2OwnedResultRollbackTest,
       ExhaustedTableRollsBackPartialTupleAndAllowsReopen)
{
    int first_pipe[2] = { -1, -1 };
    ASSERT_EQ(pipe(first_pipe), 0);
    uint32_t first_reps[] = {
        AddMeteredFd(first_pipe[0], WASI_P2_IO_INPUT_STREAM),
        AddMeteredFd(first_pipe[1], WASI_P2_IO_OUTPUT_STREAM),
    };
    ASSERT_NE(first_reps[0], 0u);
    ASSERT_NE(first_reps[1], 0u);

    /* Leave exactly one reusable slot, then put the monotonic index past the
     * canonical table limit. The first tuple own lowers; the second cannot. */
    component_.table->free_list[0] = 1;
    component_.table->free_count = 1;
    component_.table->next_index = WASM_COMPONENT_TABLE_MAX_LENGTH + 1;
    uint32_t first_index = 0;
    uint32_t second_index = 0;
    EXPECT_TRUE(LowerOwn(first_reps[0], &first_index));
    EXPECT_EQ(first_index, 1u);
    EXPECT_FALSE(LowerOwn(first_reps[1], &second_index));

    wasi_p2_cleanup_failed_owned_host_resources(&exec_env_, first_reps, 2);
    /* Restore the synthetic exhaustion state before any assertion can exit the
     * test and before fixture teardown walks the table. */
    component_.table->next_index = 2;

    EXPECT_EQ(component_.table->array[first_index], nullptr);
    EXPECT_EQ(host_resource_table_get(host_table_, first_reps[0]), nullptr);
    EXPECT_EQ(host_resource_table_get(host_table_, first_reps[1]), nullptr);
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(quota_.releases, 2u);
    EXPECT_EQ(quota_.closes, 2u);

    int reopened_pipe[2] = { -1, -1 };
    ASSERT_EQ(pipe(reopened_pipe), 0);
    uint32_t reopened_reps[] = {
        AddMeteredFd(reopened_pipe[0], WASI_P2_IO_INPUT_STREAM),
        AddMeteredFd(reopened_pipe[1], WASI_P2_IO_OUTPUT_STREAM),
    };
    ASSERT_NE(reopened_reps[0], 0u);
    ASSERT_NE(reopened_reps[1], 0u);
    uint32_t reopened_indices[2] = { 0 };
    ASSERT_TRUE(LowerOwn(reopened_reps[0], &reopened_indices[0]));
    ASSERT_TRUE(LowerOwn(reopened_reps[1], &reopened_indices[1]));
    EXPECT_TRUE(wasm_component_table_drop_resource(component_.table,
                                                   reopened_indices[1]));
    EXPECT_TRUE(wasm_component_table_drop_resource(component_.table,
                                                   reopened_indices[0]));
    EXPECT_EQ(quota_.active, 0u);
    EXPECT_EQ(quota_.releases, 4u);
    EXPECT_EQ(quota_.closes, 4u);
}

// Test to verify that all exported WASI P2 symbols can be resolved.
TEST_F(WasiP2WrapperTest, LookupAllWasiP2Symbols)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t module_count = get_libc_wasi_p2_export_apis(&modules);

    EXPECT_GT(module_count, 0);

    for (uint32_t i = 0; i < module_count; i++) {
        wasi_p2_module_t module = modules[i];
        for (uint32_t j = 0; j < module.symbol_count; j++) {
            const NativeSymbol symbol = module.symbols[j];

            EXPECT_TRUE(wasm_native_register_wasi_p2_module_func(module.module_name, symbol.symbol));

            void *func_ptr = wasm_native_resolve_symbol(
                module.module_name, symbol.symbol, nullptr, nullptr, nullptr,
                nullptr);
            EXPECT_NE(func_ptr, nullptr)
                << "Failed to resolve symbol: " << symbol.symbol
                << " in module " << module.module_name;
        }

        // Test that a non-existent symbol in a valid module is not resolved.
        void *func_ptr = wasm_native_resolve_symbol(
            module.module_name, "non-existent", nullptr, nullptr, nullptr,
            nullptr);
        EXPECT_EQ(func_ptr, nullptr);
    }

    // Test that a symbol in a non-existent module is not resolved.
    void *func_ptr = wasm_native_resolve_symbol(
        "wasi:non-existent/interface@0.2.6", "non-existent", nullptr, nullptr,
        nullptr, nullptr);
    EXPECT_EQ(func_ptr, nullptr);
}

TEST_F(WasiP2WrapperTest, BuiltinResourceRegistryIsExactAndVersioned)
{
    EXPECT_TRUE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network@0.2.6", "network"));
    EXPECT_TRUE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:filesystem/types@0.2.6", "descriptor"));

    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network@0.2.6", "unknown"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:unknown/network@0.2.6", "network"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network", "network"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network@0.2.7", "network"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network@0.3.0", "network"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(nullptr, "network"));
    EXPECT_FALSE(wasm_native_has_builtin_wasi_p2_resource(
        "wasi:sockets/network@0.2.6", nullptr));
}

// Test to ensure that WASI P2 symbols are not resolved in the WASI P1 namespace.
TEST_F(WasiP2WrapperTest, LookupWasiP2SymbolsInWasiP1NamespaceShouldFail)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t module_count = get_libc_wasi_p2_export_apis(&modules);

    EXPECT_GT(module_count, 0);

    for (uint32_t i = 0; i < module_count; i++) {
        wasi_p2_module_t module = modules[i];
        for (uint32_t j = 0; j < module.symbol_count; j++) {
            const NativeSymbol symbol = module.symbols[j];

            // Register it first to ensure it's available in the native symbols list
            EXPECT_TRUE(wasm_native_register_wasi_p2_module_func(module.module_name, symbol.symbol));

            void *func_ptr = wasm_native_resolve_symbol(
                "wasi_snapshot_preview1", symbol.symbol, nullptr, nullptr,
                nullptr, nullptr);
            EXPECT_EQ(func_ptr, nullptr);
        }
    }
}

// Test the registration and unregistration of all WASI P2 modules.
TEST_F(WasiP2WrapperTest, RegisterUnregisterAllModules)
{
    const char *module_name = "wasi:clocks/wall-clock@0.2.6";
    const char *symbol_name = "now";

    // Unregister all modules.
    wasm_native_unregister_wasi_p2_modules();

    // Now, the symbol should not be resolved.
    void *func_ptr = wasm_native_resolve_symbol(module_name, symbol_name, nullptr,
                                          nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr, nullptr);

    // Register all modules again.
    EXPECT_TRUE(wasm_native_register_wasi_p2_modules());

    // The symbol should be resolved again.
    func_ptr = wasm_native_resolve_symbol(module_name, symbol_name, nullptr,
                                          nullptr, nullptr, nullptr);
    EXPECT_NE(func_ptr, nullptr);
}

// Test the registration and unregistration of a single WASI P2 module.
TEST_F(WasiP2WrapperTest, RegisterUnregisterSingleModule)
{
    const char *module_to_test = "wasi:clocks/monotonic-clock@0.2.6";
    const char *symbol_to_test = "now";
    const char *another_module = "wasi:clocks/wall-clock@0.2.6";
    const char *another_symbol = "now";

    // Ensure they are registered first
    EXPECT_TRUE(wasm_native_register_wasi_p2_module(another_module));

    // Unregister the specific module.
    wasm_native_unregister_wasi_p2_module(module_to_test);

    // The symbol from the unregistered module should not be resolved.
    void *func_ptr1 = wasm_native_resolve_symbol(module_to_test, symbol_to_test,
                                           nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr1, nullptr);

    // The symbol from the other module should still be resolved.
    void *func_ptr2 = wasm_native_resolve_symbol(another_module, another_symbol,
                                           nullptr, nullptr, nullptr, nullptr);
    EXPECT_NE(func_ptr2, nullptr);

    // Register the module again.
    EXPECT_TRUE(wasm_native_register_wasi_p2_module(module_to_test));

    // The symbol should be resolved again.
    func_ptr1 = wasm_native_resolve_symbol(module_to_test, symbol_to_test,
                                           nullptr, nullptr, nullptr, nullptr);
    EXPECT_NE(func_ptr1, nullptr);
}

// Test the registration and unregistration of a single function within a WASI P2 module.
TEST_F(WasiP2WrapperTest, RegisterUnregisterSingleFunction)
{
    const char *module_name = "wasi:random/random@0.2.6";
    const char *func_to_test = "get-random-bytes";
    const char *another_func = "get-random-u64";

    // Unregister the whole module to ensure a clean state.
    wasm_native_unregister_wasi_p2_module(module_name);

    // Ensure both functions are not resolved.
    void *func_ptr1 = wasm_native_resolve_symbol(
        module_name, func_to_test, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr1, nullptr);
    void *func_ptr2 = wasm_native_resolve_symbol(
        module_name, another_func, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr2, nullptr);

    // Register just one function.
    EXPECT_TRUE(
        wasm_native_register_wasi_p2_module_func(module_name, func_to_test));

    // Check that only the registered function is resolved.
    func_ptr1 = wasm_native_resolve_symbol(module_name, func_to_test, nullptr,
                                           nullptr, nullptr, nullptr);
    EXPECT_NE(func_ptr1, nullptr);
    func_ptr2 = wasm_native_resolve_symbol(module_name, another_func, nullptr,
                                           nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr2, nullptr);

    // Unregister the single function.
    wasm_native_unregister_wasi_p2_module_func(module_name, func_to_test);

    // Check that it's no longer resolved.
    func_ptr1 = wasm_native_resolve_symbol(module_name, func_to_test, nullptr,
                                           nullptr, nullptr, nullptr);
    EXPECT_EQ(func_ptr1, nullptr);

    // Re-register the whole module so other tests are not affected.
    EXPECT_TRUE(wasm_native_register_wasi_p2_module(module_name));
}
