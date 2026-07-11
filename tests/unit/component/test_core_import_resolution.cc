/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <unordered_set>

extern "C" {
#include "wasm_component_runtime.h"
#include "wasm_export.h"
#include "wasm_runtime.h"
}

namespace {

std::unordered_set<void *> live_allocations;
int fail_after = -1;
size_t invalid_frees = 0;

void *
test_malloc(unsigned int size)
{
    if (fail_after == 0) {
        return nullptr;
    }
    if (fail_after > 0) {
        fail_after--;
    }

    void *result = std::malloc(size ? size : 1);
    if (result) {
        live_allocations.insert(result);
    }
    return result;
}

void *
test_realloc(void *ptr, unsigned int size)
{
    if (!ptr) {
        return test_malloc(size);
    }
    if (live_allocations.find(ptr) == live_allocations.end()) {
        invalid_frees++;
        return nullptr;
    }
    if (fail_after == 0) {
        return nullptr;
    }
    if (fail_after > 0) {
        fail_after--;
    }

    live_allocations.erase(ptr);
    void *result = std::realloc(ptr, size ? size : 1);
    if (!result) {
        live_allocations.insert(ptr);
        return nullptr;
    }
    live_allocations.insert(result);
    return result;
}

void
test_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    if (live_allocations.erase(ptr) == 0) {
        invalid_frees++;
        return;
    }
    std::free(ptr);
}

class CoreImportResolutionTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_Allocator;
        init_args.mem_alloc_option.allocator.malloc_func =
            reinterpret_cast<void *>(test_malloc);
        init_args.mem_alloc_option.allocator.realloc_func =
            reinterpret_cast<void *>(test_realloc);
        init_args.mem_alloc_option.allocator.free_func =
            reinterpret_cast<void *>(test_free);
        fail_after = -1;
        invalid_frees = 0;
        live_allocations.clear();
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        baseline = live_allocations.size();
    }

    void TearDown() override
    {
        fail_after = -1;
        wasm_runtime_destroy();
        EXPECT_EQ(invalid_frees, 0u);
        EXPECT_TRUE(live_allocations.empty());
    }

    void prepare_resolution(uint32_t instance_idx)
    {
        import.kind = IMPORT_KIND_FUNC;
        import.u.names.module_name = module_name;
        import.u.names.field_name = field_name;

        argument_name.name_len = 3;
        argument_name.name = module_name;
        argument.name = &argument_name;
        argument.idx.instance_idx = instance_idx;
        expression.with_args.arg_len = 1;
        expression.with_args.args = &argument;

        exported_function.name = field_name;
        exported_function.function = &function;
        core_instance.export_func_count = 1;
        core_instance.export_functions = &exported_function;
        core_instances[0] = &core_instance;
        component_instance.core_module_instances = core_instances;
        component_instance.core_module_instances_count = 1;
    }

    size_t baseline = 0;
    char module_name[4] = "mod";
    char field_name[6] = "field";
    char error[256] = {};
    WASMImport import = {};
    WASMComponentCoreName argument_name = {};
    WASMComponentInstArg argument = {};
    WASMInstExpr expression = {};
    WASMFunctionInstance function = {};
    WASMExportFuncInstance exported_function = {};
    WASMModuleInstance core_instance = {};
    WASMModuleInstance *core_instances[1] = {};
    WASMComponentInstance component_instance = {};
};

TEST_F(CoreImportResolutionTest, RejectsIndexEqualToCurrentInstanceCount)
{
    prepare_resolution(1);

    EXPECT_EQ(wasm_import_find_in_args(&import, &expression,
                                       &component_instance, error,
                                       sizeof(error)),
              nullptr);
    EXPECT_NE(strstr(error, "not yet defined"), nullptr) << error;
    EXPECT_EQ(live_allocations.size(), baseline);
}

TEST_F(CoreImportResolutionTest, AllocationFailureIsReportedWithoutLeak)
{
    prepare_resolution(0);
    fail_after = 0;

    EXPECT_EQ(wasm_import_find_in_args(&import, &expression,
                                       &component_instance, error,
                                       sizeof(error)),
              nullptr);
    EXPECT_NE(strstr(error, "allocate"), nullptr) << error;
    EXPECT_EQ(live_allocations.size(), baseline);

    fail_after = -1;
    error[0] = '\0';
    WASMCoreExport *resolved = wasm_import_find_in_args(
        &import, &expression, &component_instance, error, sizeof(error));
    ASSERT_NE(resolved, nullptr) << error;
    EXPECT_EQ(resolved->kind, IMPORT_KIND_FUNC);
    EXPECT_EQ(resolved->exp.func_instance, &function);
    wasm_runtime_free(resolved);
    EXPECT_EQ(live_allocations.size(), baseline);
}

} // namespace
