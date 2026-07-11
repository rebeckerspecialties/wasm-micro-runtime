/*
 * Copyright (C) 2026 Bytecode Alliance and contributors. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <cstdlib>
#include <cstring>

#include "gtest/gtest.h"
#include "wasm_export.h"

static const uint8_t dependency_wasm_template[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07,
    0x09, 0x01, 0x05, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x07, 0x0b,
};

static const uint8_t parent_wasm_template[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
    0x00, 0x01, 0x7f, 0x02, 0x14, 0x01, 0x0a, 0x64, 0x65, 0x70, 0x65, 0x6e,
    0x64, 0x65, 0x6e, 0x63, 0x79, 0x05, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00,
    0x00, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e,
    0x00, 0x01, 0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b,
};

static uint8_t dependency_wasm[sizeof(dependency_wasm_template)];
static uint8_t parent_wasm[sizeof(parent_wasm_template)];
static wasm_module_t tracked_parent;
static wasm_module_t tracked_dependency;
static bool tracked_parent_freed;
static bool tracked_dependency_freed;
static uint8_t *reader_buffer;
static bool reader_buffer_destroyed;

static void *
tracking_malloc(unsigned int size)
{
    return malloc(size);
}

static void *
tracking_realloc(void *ptr, unsigned int size)
{
    return realloc(ptr, size);
}

static void
tracking_free(void *ptr)
{
    if (ptr == tracked_parent) {
        tracked_parent_freed = true;
    }
    if (ptr == tracked_dependency) {
        tracked_dependency_freed = true;
    }
    free(ptr);
}

static bool
module_reader_callback(package_type_t module_type, const char *module_name,
                       uint8_t **p_buffer, uint32_t *p_size)
{
    if (module_type != Wasm_Module_Bytecode
        || strcmp(module_name, "dependency") != 0) {
        return false;
    }

    reader_buffer = (uint8_t *)malloc(sizeof(dependency_wasm_template));
    if (!reader_buffer) {
        return false;
    }
    memcpy(reader_buffer, dependency_wasm_template,
           sizeof(dependency_wasm_template));
    *p_buffer = reader_buffer;
    *p_size = (uint32_t)sizeof(dependency_wasm_template);
    return true;
}

static void
module_destroyer_callback(uint8_t *buffer, uint32_t size)
{
    (void)size;
    if (buffer == reader_buffer) {
        reader_buffer_destroyed = true;
        free(reader_buffer);
        reader_buffer = nullptr;
    }
}

class wasm_runtime_module_lifetime_test_suite : public testing::Test
{
  protected:
    void SetUp() override
    {
        RuntimeInitArgs init_args = {};

        memcpy(parent_wasm, parent_wasm_template, sizeof(parent_wasm));
        memcpy(dependency_wasm, dependency_wasm_template,
               sizeof(dependency_wasm));
        tracked_parent = nullptr;
        tracked_dependency = nullptr;
        tracked_parent_freed = false;
        tracked_dependency_freed = false;
        reader_buffer = nullptr;
        reader_buffer_destroyed = false;

        init_args.mem_alloc_type = Alloc_With_Allocator;
        init_args.mem_alloc_option.allocator.malloc_func =
            (void *)tracking_malloc;
        init_args.mem_alloc_option.allocator.realloc_func =
            (void *)tracking_realloc;
        init_args.mem_alloc_option.allocator.free_func = (void *)tracking_free;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        runtime_initialized = true;
        wasm_runtime_set_module_reader(module_reader_callback,
                                       module_destroyer_callback);
    }

    void TearDown() override
    {
        if (runtime_initialized) {
            wasm_runtime_destroy();
        }
        if (reader_buffer) {
            free(reader_buffer);
            reader_buffer = nullptr;
        }
        tracked_parent = nullptr;
        tracked_dependency = nullptr;
    }

    bool runtime_initialized = false;
};

TEST_F(wasm_runtime_module_lifetime_test_suite,
       unregister_then_unload_parent_before_dependency)
{
    char error_buf[128] = {};

    tracked_dependency = wasm_runtime_load(
        dependency_wasm, sizeof(dependency_wasm), error_buf, sizeof(error_buf));
    ASSERT_NE(tracked_dependency, nullptr);
    ASSERT_TRUE(wasm_runtime_register_module("dependency", tracked_dependency,
                                             error_buf, sizeof(error_buf)))
        << error_buf;
    tracked_parent = wasm_runtime_load(parent_wasm, sizeof(parent_wasm),
                                       error_buf, sizeof(error_buf));
    ASSERT_NE(tracked_parent, nullptr) << error_buf;
    ASSERT_TRUE(wasm_runtime_register_module("parent", tracked_parent,
                                             error_buf, sizeof(error_buf)))
        << error_buf;

    wasm_runtime_unload(tracked_parent);
    wasm_runtime_unload(tracked_dependency);
    EXPECT_FALSE(tracked_parent_freed);
    EXPECT_FALSE(tracked_dependency_freed);
    EXPECT_EQ(wasm_runtime_find_module_registered("parent"), tracked_parent);
    EXPECT_EQ(wasm_runtime_find_module_registered("dependency"),
              tracked_dependency);

    EXPECT_TRUE(wasm_runtime_unregister_module(tracked_parent));
    wasm_runtime_unload(tracked_parent);
    EXPECT_TRUE(tracked_parent_freed);
    EXPECT_EQ(wasm_runtime_find_module_registered("parent"), nullptr);
    EXPECT_EQ(wasm_runtime_find_module_registered("dependency"),
              tracked_dependency);

    EXPECT_TRUE(wasm_runtime_unregister_module(tracked_dependency));
    wasm_runtime_unload(tracked_dependency);
    EXPECT_TRUE(tracked_dependency_freed);
    EXPECT_EQ(wasm_runtime_find_module_registered("dependency"), nullptr);
}

TEST_F(wasm_runtime_module_lifetime_test_suite,
       reader_owned_dependency_remains_registered_until_runtime_destroy)
{
    char error_buf[128] = {};

    tracked_parent = wasm_runtime_load(parent_wasm, sizeof(parent_wasm),
                                       error_buf, sizeof(error_buf));
    ASSERT_NE(tracked_parent, nullptr) << error_buf;
    tracked_dependency = wasm_runtime_find_module_registered("dependency");
    ASSERT_NE(tracked_dependency, nullptr);
    ASSERT_TRUE(wasm_runtime_register_module("parent", tracked_parent,
                                             error_buf, sizeof(error_buf)))
        << error_buf;

    EXPECT_FALSE(wasm_runtime_unregister_module(tracked_dependency));
    wasm_runtime_unload(tracked_dependency);
    EXPECT_FALSE(tracked_dependency_freed);
    EXPECT_FALSE(reader_buffer_destroyed);
    EXPECT_EQ(wasm_runtime_find_module_registered("dependency"),
              tracked_dependency);

    EXPECT_TRUE(wasm_runtime_unregister_module(tracked_parent));
    wasm_runtime_unload(tracked_parent);
    EXPECT_TRUE(tracked_parent_freed);
    EXPECT_EQ(wasm_runtime_find_module_registered("dependency"),
              tracked_dependency);

    wasm_runtime_destroy();
    runtime_initialized = false;
    EXPECT_TRUE(tracked_dependency_freed);
    EXPECT_TRUE(reader_buffer_destroyed);
}
