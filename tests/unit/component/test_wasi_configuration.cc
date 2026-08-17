/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <cstring>

#include "helpers.h"

extern "C" {
#include "wasm_runtime_common.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

namespace {

class ComponentWasiConfigurationTest : public testing::Test
{
  protected:
    ComponentHelper helper;

    void SetUp() override { helper.do_setup(); }
    void TearDown() override { helper.do_teardown(); }
};

TEST_F(ComponentWasiConfigurationTest, ComponentDefaultsAndSettersAreExplicit)
{
    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(helper.read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper.load_component());

    EXPECT_FALSE(helper.component->wasi_args.set_by_user);
    EXPECT_EQ(helper.component->wasi_args.stdio[0], os_invalid_raw_handle());
    EXPECT_EQ(helper.component->wasi_args.stdio[1], os_invalid_raw_handle());
    EXPECT_EQ(helper.component->wasi_args.stdio[2], os_invalid_raw_handle());

    WASMComponent args_component = {};
    wasi_args_set_defaults(&args_component.wasi_args);
    char arg[] = "guest";
    char *argv[] = { arg };
    wasm_component_runtime_set_wasi_args(&args_component, nullptr, 0, nullptr,
                                         0, nullptr, 0, argv, 1);
    EXPECT_TRUE(args_component.wasi_args.set_by_user);
    EXPECT_TRUE(args_component.import_wasi_api);

    WASMComponent addr_component = {};
    wasi_args_set_defaults(&addr_component.wasi_args);
    const char *addr_pool[] = { "127.0.0.1/32" };
    wasm_component_runtime_set_wasi_addr_pool(&addr_component, addr_pool, 1);
    EXPECT_TRUE(addr_component.wasi_args.set_by_user);

    WASMComponent lookup_component = {};
    wasi_args_set_defaults(&lookup_component.wasi_args);
    const char *lookup_pool[] = { "localhost" };
    wasm_component_runtime_set_wasi_ns_lookup_pool(&lookup_component,
                                                   lookup_pool, 1);
    EXPECT_TRUE(lookup_component.wasi_args.set_by_user);

    WASMComponent options_component = {};
    wasi_args_set_defaults(&options_component.wasi_args);
    libc_wasi_options_t options = {};
    wasm_component_runtime_set_wasi_options(&options_component, &options);
    EXPECT_TRUE(options_component.wasi_args.set_by_user);
}

TEST_F(ComponentWasiConfigurationTest,
       ExplicitComponentArgumentsEnvironmentAndPreopenReachContext)
{
    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(helper.read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper.load_component());

    const char *dirs[] = { "." };
    const char *env[] = { "KEY=value" };
    char arg0[] = "guest";
    char arg1[] = "arg1";
    char *argv[] = { arg0, arg1 };
    wasm_component_runtime_set_wasi_args(helper.component, dirs, 1, nullptr, 0,
                                         env, 1, argv, 2);

    ASSERT_TRUE(helper.instantiate_component()) << helper.error_buf;
    WASIContext *wasi_ctx = helper.component_inst->wasi_ctx;
    ASSERT_NE(wasi_ctx, nullptr);
    ASSERT_NE(wasi_ctx->argv_list, nullptr);
    ASSERT_NE(wasi_ctx->env_list, nullptr);
    EXPECT_STREQ(wasi_ctx->argv_list[0], "guest");
    EXPECT_STREQ(wasi_ctx->argv_list[1], "arg1");
    EXPECT_EQ(wasi_ctx->argv_list[2], nullptr);
    EXPECT_STREQ(wasi_ctx->env_list[0], "KEY=value");
    EXPECT_EQ(wasi_ctx->env_list[1], nullptr);
    ASSERT_NE(wasi_ctx->prestats, nullptr);
    ASSERT_GT(wasi_ctx->prestats->size, 3u);
    ASSERT_NE(wasi_ctx->prestats->prestats[3].dir, nullptr);
    EXPECT_STREQ(wasi_ctx->prestats->prestats[3].dir, ".");
}

TEST_F(ComponentWasiConfigurationTest,
       MixedComponentAndInstantiationConfigurationIsRejected)
{
    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(helper.read_wasm_file("add.wasm"));
    ASSERT_TRUE(helper.load_component());

    char component_arg[] = "component";
    char *component_argv[] = { component_arg };
    wasm_component_runtime_set_wasi_args(helper.component, nullptr, 0, nullptr,
                                         0, nullptr, 0, component_argv, 1);

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    char instance_arg[] = "instance";
    char *instance_argv[] = { instance_arg };
    wasm_runtime_instantiation_args_set_wasi_arg(args, instance_argv, 1);
    WASMComponentInstance *instance = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);

    EXPECT_EQ(instance, nullptr);
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    EXPECT_NE(
        strstr(helper.error_buf, "both the component and InstantiationArgs2"),
        nullptr)
        << helper.error_buf;
}

} // namespace
