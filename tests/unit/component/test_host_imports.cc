/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <cstring>

#include "helpers.h"

extern "C" {
#include "wasm_component_host_resource.h"
#include "wasm_component_resource.h"
#include "wasm_native.h"
}

namespace {

static void
host_log(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    (void)exec_env;
    (void)canonical_cells;
}

static bool
host_resource_drop(void *, uint32_t)
{
    return true;
}

struct HostImportDropState {
    uint32_t first_count;
    uint32_t first_rep;
    uint32_t second_count;
    uint32_t second_rep;
};

struct CustomDataState {
    uint32_t marker;
    uint32_t raw_calls;
    uint32_t drops;
    uint32_t last_rep;
};

class HostResourceTableScope
{
  public:
    bool Initialize()
    {
        initialized_ = instantiate_host_resource_table();
        return initialized_;
    }

    ~HostResourceTableScope()
    {
        if (initialized_) {
            destroy_host_resource_table();
        }
    }

  private:
    bool initialized_ = false;
};

static uint32_t
replace_binary_text(uint8_t *bytes, uint32_t size, const char *from,
                    const char *to)
{
    size_t length = strlen(from);
    uint32_t replacements = 0;

    if (!bytes || strlen(to) != length || length > size) {
        return 0;
    }
    for (uint32_t offset = 0; offset <= size - length; offset++) {
        if (memcmp(bytes + offset, from, length) == 0) {
            memcpy(bytes + offset, to, length);
            replacements++;
            offset += (uint32_t)length - 1;
        }
    }
    return replacements;
}

static void
observe_custom_data(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    auto *component_state = static_cast<CustomDataState *>(
        wasm_component_get_custom_data_from_exec_env(exec_env));
    auto *module_state = static_cast<CustomDataState *>(
        wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(exec_env)));

    if (!component_state || component_state != module_state) {
        canonical_cells[0] = 0;
        return;
    }
    component_state->raw_calls++;
    canonical_cells[0] = component_state->marker;
}

static bool
observe_custom_data_drop(void *attachment, uint32_t rep)
{
    auto *state = static_cast<CustomDataState *>(attachment);

    if (!state) {
        return false;
    }
    state->drops++;
    state->last_rep = rep;
    return true;
}

static void
newer_random_u64(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    (void)exec_env;
    canonical_cells[0] = 0x0123456789ABCDEFULL;
}

static void
first_use_item(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    (void)exec_env;
    (void)canonical_cells;
}

static void
second_use_item(wasm_exec_env_t exec_env, uint64_t *canonical_cells)
{
    (void)exec_env;
    (void)canonical_cells;
}

static bool
first_item_drop(void *attachment, uint32_t rep)
{
    auto *state = static_cast<HostImportDropState *>(attachment);

    if (!state) {
        return false;
    }
    state->first_count++;
    state->first_rep = rep;
    return true;
}

static bool
second_item_drop(void *attachment, uint32_t rep)
{
    auto *state = static_cast<HostImportDropState *>(attachment);

    if (!state) {
        return false;
    }
    state->second_count++;
    state->second_rep = rep;
    return true;
}

static WASMComponentResourceInstance *
find_direct_resource(WASMComponentInstance *instance)
{
    if (!instance) {
        return nullptr;
    }
    for (uint32_t idx = 0; idx < instance->types_count; idx++) {
        WASMComponentTypeInstance *type = instance->types[idx];

        if (type
            && (type->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                || type->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
            return type->type_specific.resource;
        }
    }
    return nullptr;
}

static WASMComponentResourceInstance *
find_template_resource(WASMComponentInstTypeInstance *instance_type)
{
    if (!instance_type) {
        return nullptr;
    }
    for (uint32_t idx = 0; idx < instance_type->types_count; idx++) {
        WASMComponentTypeInstance *type = instance_type->types[idx];

        if (type
            && (type->type == COMPONENT_VAL_TYPE_RESOURCE_SYNC
                || type->type == COMPONENT_VAL_TYPE_RESOURCE_ASYNC)) {
            return type->type_specific.resource;
        }
    }
    return nullptr;
}

static WASMComponentResourceInstance *
find_resource(WASMComponentInstance *instance, const char *interface_name,
              const char *resource_name)
{
    if (!instance) {
        return nullptr;
    }
    for (uint32_t idx = 0; idx < instance->types_count; idx++) {
        WASMComponentTypeInstance *type = instance->types[idx];
        if (!type
            || (type->type != COMPONENT_VAL_TYPE_RESOURCE_SYNC
                && type->type != COMPONENT_VAL_TYPE_RESOURCE_ASYNC)
            || !type->type_specific.resource
            || !type->type_specific.resource->interface_name
            || !type->type_specific.resource->name) {
            continue;
        }
        WASMComponentResourceInstance *resource = type->type_specific.resource;
        if (strcmp(resource->interface_name, interface_name) == 0
            && strcmp(resource->name, resource_name) == 0) {
            return resource;
        }
    }
    for (uint32_t idx = 0; idx < instance->defined_instances_count; idx++) {
        WASMComponentResourceInstance *resource = find_resource(
            instance->defined_instances[idx], interface_name, resource_name);
        if (resource) {
            return resource;
        }
    }
    return nullptr;
}

class ComponentHostImportsTest : public testing::Test
{
  protected:
    ComponentHelper helper;

    void SetUp() override { helper.do_setup(); }
    void TearDown() override { helper.do_teardown(); }
};

TEST_F(ComponentHostImportsTest, ResolvesStaticCustomAndWasiInterfaces)
{
    ASSERT_TRUE(helper.runtime_init);

    NativeSymbol host_symbols[] = {
        { "log", (void *)host_log, "(iii)", nullptr },
    };
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:project/imported-interface@0.1.0", host_symbols,
        (uint32_t)(sizeof(host_symbols) / sizeof(host_symbols[0]))));
    const char *signature = nullptr;
    void *attachment = nullptr;
    bool call_conv_raw = false;
    EXPECT_EQ(wasm_native_resolve_symbol_exact(
                  "test:project/imported-interface@0.1.0", "_log", nullptr,
                  &signature, &attachment, &call_conv_raw),
              nullptr);

    ASSERT_TRUE(helper.read_wasm_file("complex_with_host.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper.component, "wasi:io/error@0.2.3", "error", host_resource_drop));

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    ASSERT_NE(args, nullptr);
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    uint32_t custom_data = 0xC0DEC0DE;
    wasm_runtime_instantiation_args_set_custom_data(args, &custom_data);

    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    EXPECT_EQ(wasm_component_get_custom_data(helper.component_inst),
              &custom_data);
    WASMComponentResourceInstance *error_resource =
        find_resource(helper.component_inst, "wasi:io/error@0.2.3", "error");
    ASSERT_NE(error_resource, nullptr);
    EXPECT_TRUE(error_resource->is_builtin_wasi);
    EXPECT_EQ(error_resource->host_drop_callback, host_resource_drop);
    EXPECT_EQ(error_resource->host_drop_attachment, &custom_data);
    ASSERT_GE(helper.component_inst->defined_instances_count, 9u);
    ASSERT_GE(helper.component_inst->component_instances_count, 9u);

    /* Rust-generated WASI components export a resource from its owning
     * interface, then outer-alias that nominal type into consumer interfaces.
     * The consumer must retain the exact owner type rather than clone and
     * rebind it as another host resource. */
    ASSERT_GT(helper.component_inst->types_count, 2u);
    WASMComponentInstance *error_interface =
        helper.component_inst->component_instances[1];
    WASMComponentInstance *streams_interface =
        helper.component_inst->component_instances[2];
    ASSERT_NE(error_interface, nullptr);
    ASSERT_NE(streams_interface, nullptr);
    ASSERT_GT(error_interface->types_count, 0u);
    ASSERT_GT(streams_interface->types_count, 1u);
    WASMComponentTypeInstance *nominal_error_type =
        helper.component_inst->types[2];
    EXPECT_EQ(error_interface->types[0], nominal_error_type);
    EXPECT_EQ(streams_interface->types[0], nominal_error_type);
    EXPECT_EQ(streams_interface->types[1], nominal_error_type);
    EXPECT_EQ(streams_interface->types[0]->type_specific.resource,
              error_resource);
    EXPECT_EQ(streams_interface->types[0]->type_specific.resource->drop_method,
              error_resource->drop_method);

    WASMComponentInstance *custom_interface =
        helper.component_inst->component_instances[0];
    ASSERT_NE(custom_interface, nullptr);
    ASSERT_EQ(custom_interface->functions_count, 1u);
    ASSERT_NE(custom_interface->functions[0]->core_func, nullptr);
    EXPECT_EQ(custom_interface->functions[0]
                  ->core_func->u.func_import->func_ptr_linked,
              (void *)host_log);
    EXPECT_TRUE(custom_interface->functions[0]
                    ->core_func->u.func_import->call_conv_raw);
}

TEST_F(ComponentHostImportsTest, RejectsUnversionedCustomInterfaceFallback)
{
    ASSERT_TRUE(helper.runtime_init);

    NativeSymbol host_symbols[] = {
        { "log", (void *)host_log, "(iii)", nullptr },
    };
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:project/imported-interface", host_symbols,
        (uint32_t)(sizeof(host_symbols) / sizeof(host_symbols[0]))));

    ASSERT_TRUE(helper.read_wasm_file("complex_with_host.wasm"));
    ASSERT_TRUE(helper.load_component());

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    WASMComponentInstance *instance = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);

    EXPECT_EQ(instance, nullptr);
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    EXPECT_NE(
        strstr(helper.error_buf, "Function log not found in native symbols"),
        nullptr)
        << helper.error_buf;
}

TEST_F(ComponentHostImportsTest,
       ExactStaticWasiHostPrecedesBuiltInVersionFallback)
{
    NativeSymbol symbols[] = {
        { "get-random-u64", (void *)newer_random_u64, "()I", nullptr },
    };

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "wasi:random/random@0.3.0", symbols,
        (uint32_t)(sizeof(symbols) / sizeof(symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("exact_newer_wasi_host.wasm"));
    ASSERT_TRUE(helper.load_component());

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    ASSERT_EQ(helper.component_inst->component_instances_count, 1u);
    WASMComponentInstance *host = helper.component_inst->component_instances[0];
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(host->functions_count, 1u);
    ASSERT_NE(host->functions[0], nullptr);
    ASSERT_NE(host->functions[0]->core_func, nullptr);
    EXPECT_EQ(host->functions[0]->core_func->u.func_import->func_ptr_linked,
              (void *)newer_random_u64);
}

TEST_F(ComponentHostImportsTest,
       ExactStaticWasiResourceRequiresCallbackAndPreservesTableCollision)
{
    NativeSymbol first_symbols[] = {
        { "use-item", (void *)first_use_item, "(i)", nullptr },
    };
    NativeSymbol second_symbols[] = {
        { "use-item", (void *)second_use_item, "(i)", nullptr },
    };
    HostImportDropState drop_state = {};

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "wasi:alias/first@1.0.0", first_symbols,
        (uint32_t)(sizeof(first_symbols) / sizeof(first_symbols[0]))));
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "wasi:alias/second@1.0.0", second_symbols,
        (uint32_t)(sizeof(second_symbols) / sizeof(second_symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("shared_host_instance_type.wasm"));
    ASSERT_EQ(replace_binary_text(helper.component_raw, helper.wasm_file_size,
                                  "test:alias/", "wasi:alias/"),
              2u);
    ASSERT_TRUE(helper.load_component());

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    WASMComponentResourceInstance *resource =
        find_resource(helper.component_inst, "wasi:alias/first@1.0.0", "item");
    ASSERT_NE(resource, nullptr);
    EXPECT_TRUE(resource->is_host);
    EXPECT_FALSE(resource->is_builtin_wasi);
    EXPECT_EQ(resource->host_drop_callback, nullptr);

    HostResourceTableScope table_scope;
    ASSERT_TRUE(table_scope.Initialize());
    HostResourceTable *host_table = get_global_host_resource_table();
    ASSERT_NE(host_table, nullptr);
    HostResource *collision =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(collision, nullptr);
    uint32_t representation = host_resource_table_add(host_table, collision);
    ASSERT_NE(representation, 0u);

    WASMResourceHandle *handle =
        wasm_create_resource_handle(resource, representation, true);
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(wasm_drop_resource_handle(handle));
    EXPECT_NE(host_resource_table_get(host_table, representation), nullptr);

    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        helper.component_inst, "wasi:alias/first@1.0.0", "item",
        first_item_drop, &drop_state));
    handle = wasm_create_resource_handle(resource, representation, true);
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(wasm_drop_resource_handle(handle));
    EXPECT_EQ(drop_state.first_count, 1u);
    EXPECT_EQ(drop_state.first_rep, representation);
    EXPECT_NE(host_resource_table_get(host_table, representation), nullptr);
    EXPECT_EQ(host_resource_table_delete(host_table, representation), 1u);
}

TEST_F(ComponentHostImportsTest,
       MixedStaticAndBuiltInWasiResourceRequiresCallback)
{
    NativeSymbol custom_symbols[] = {
        { "log", (void *)host_log, "(iii)", nullptr },
    };
    NativeSymbol override_symbols[] = {
        { "[method]descriptor.get-type", (void *)host_log, "(ii)", nullptr },
    };
    HostImportDropState drop_state = {};

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:project/imported-interface@0.1.0", custom_symbols,
        (uint32_t)(sizeof(custom_symbols) / sizeof(custom_symbols[0]))));
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "wasi:filesystem/types@0.2.3", override_symbols,
        (uint32_t)(sizeof(override_symbols) / sizeof(override_symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("complex_with_host.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper.component, "wasi:io/error@0.2.3", "error", host_resource_drop));

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    WASMComponentResourceInstance *resource = find_resource(
        helper.component_inst, "wasi:filesystem/types@0.2.3", "descriptor");
    ASSERT_NE(resource, nullptr);
    EXPECT_TRUE(resource->is_host);
    EXPECT_FALSE(resource->is_builtin_wasi);
    EXPECT_EQ(resource->host_drop_callback, nullptr);

    ASSERT_GT(helper.component_inst->component_instances_count, 7u);
    WASMComponentInstance *filesystem_types =
        helper.component_inst->component_instances[7];
    ASSERT_NE(filesystem_types, nullptr);
    uint32_t static_function_count = 0;
    uint32_t builtin_function_count = 0;
    for (uint32_t idx = 0; idx < filesystem_types->functions_count; idx++) {
        WASMFunctionInstance *core_func =
            filesystem_types->functions[idx]->core_func;
        ASSERT_NE(core_func, nullptr);
        ASSERT_NE(core_func->u.func_import, nullptr);
        if (core_func->u.func_import->func_ptr_linked == (void *)host_log) {
            static_function_count++;
        }
        else {
            ASSERT_NE(core_func->u.func_import->func_ptr_linked, nullptr);
            builtin_function_count++;
        }
    }
    EXPECT_EQ(static_function_count, 1u);
    EXPECT_GT(builtin_function_count, 0u);

    HostResourceTableScope table_scope;
    ASSERT_TRUE(table_scope.Initialize());
    HostResourceTable *host_table = get_global_host_resource_table();
    ASSERT_NE(host_table, nullptr);
    HostResource *collision =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(collision, nullptr);
    uint32_t representation = host_resource_table_add(host_table, collision);
    ASSERT_NE(representation, 0u);

    WASMResourceHandle *handle =
        wasm_create_resource_handle(resource, representation, true);
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(wasm_drop_resource_handle(handle));
    EXPECT_NE(host_resource_table_get(host_table, representation), nullptr);

    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        helper.component_inst, "wasi:filesystem/types@0.2.3", "descriptor",
        first_item_drop, &drop_state));
    handle = wasm_create_resource_handle(resource, representation, true);
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(wasm_drop_resource_handle(handle));
    EXPECT_EQ(drop_state.first_count, 1u);
    EXPECT_EQ(drop_state.first_rep, representation);
    EXPECT_NE(host_resource_table_get(host_table, representation), nullptr);
    EXPECT_EQ(host_resource_table_delete(host_table, representation), 1u);
}

TEST_F(ComponentHostImportsTest,
       CustomDataUpdatePropagatesButPreservesExplicitResourceAttachment)
{
    NativeSymbol symbols[] = {
        { "observe", (void *)observe_custom_data, "()i", nullptr },
    };
    CustomDataState initial = {};
    CustomDataState updated = {};
    CustomDataState final_state = {};
    CustomDataState explicit_drop_state = {};
    WASMComponentPreparedCall *prepared_call = nullptr;
    wasm_val_t result = {};

    initial.marker = 11;
    updated.marker = 22;
    final_state.marker = 33;
    explicit_drop_state.marker = 44;

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:context/host@1.0.0", symbols,
        (uint32_t)(sizeof(symbols) / sizeof(symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("custom_data_update.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper.component, "test:context/host@1.0.0", "item",
        observe_custom_data_drop));

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_custom_data(args, &initial);
    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    WASMComponentResourceInstance *resource =
        find_resource(helper.component_inst, "test:context/host@1.0.0", "item");
    ASSERT_NE(resource, nullptr);
    ASSERT_TRUE(resource->host_drop_attachment_is_custom_data);
    EXPECT_EQ(resource->host_drop_attachment, &initial);

    wasm_component_set_custom_data(helper.component_inst, &updated);
    EXPECT_EQ(wasm_component_get_custom_data(helper.component_inst), &updated);
    EXPECT_EQ(resource->host_drop_attachment, &updated);

    prepared_call = wasm_component_prepare_export_call(
        helper.component_inst, "call", helper.error_buf,
        sizeof(helper.error_buf));
    ASSERT_NE(prepared_call, nullptr) << helper.error_buf;
    ASSERT_TRUE(
        wasm_component_call_prepared(prepared_call, 1, &result, 0, nullptr));
    ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared_call));
    EXPECT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, 22);
    EXPECT_EQ(updated.raw_calls, 1u);
    EXPECT_EQ(initial.raw_calls, 0u);

    WASMResourceHandle *handle =
        wasm_create_resource_handle(resource, 101, true);
    ASSERT_NE(handle, nullptr);
    uint32_t handle_index = 0;
    ASSERT_TRUE(wasm_component_table_add(helper.component_inst->table, handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &handle_index));
    EXPECT_TRUE(wasm_component_table_drop_resource(helper.component_inst->table,
                                                   handle_index));
    EXPECT_EQ(updated.drops, 1u);
    EXPECT_EQ(updated.last_rep, 101u);
    EXPECT_EQ(initial.drops, 0u);

    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        helper.component_inst, "test:context/host@1.0.0", "item",
        observe_custom_data_drop, &explicit_drop_state));
    EXPECT_FALSE(resource->host_drop_attachment_is_custom_data);
    wasm_component_set_custom_data(helper.component_inst, &final_state);
    EXPECT_EQ(resource->host_drop_attachment, &explicit_drop_state);

    result = {};
    ASSERT_TRUE(
        wasm_component_call_prepared(prepared_call, 1, &result, 0, nullptr));
    ASSERT_TRUE(wasm_component_prepared_call_post_return(prepared_call));
    EXPECT_EQ(result.kind, WASM_I32);
    EXPECT_EQ(result.of.i32, 33);
    EXPECT_EQ(final_state.raw_calls, 1u);

    handle = wasm_create_resource_handle(resource, 202, true);
    ASSERT_NE(handle, nullptr);
    handle_index = 0;
    ASSERT_TRUE(wasm_component_table_add(helper.component_inst->table, handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &handle_index));
    EXPECT_TRUE(wasm_component_table_drop_resource(helper.component_inst->table,
                                                   handle_index));
    EXPECT_EQ(explicit_drop_state.drops, 1u);
    EXPECT_EQ(explicit_drop_state.last_rep, 202u);
    EXPECT_EQ(final_state.drops, 0u);

    wasm_component_destroy_prepared_call(prepared_call);
}

TEST_F(ComponentHostImportsTest, RejectsMismatchedExactNativeSignature)
{
    NativeSymbol first_symbols[] = {
        { "use-item", (void *)first_use_item, "()", nullptr },
    };
    NativeSymbol second_symbols[] = {
        { "use-item", (void *)second_use_item, "(i)", nullptr },
    };

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:alias/first@1.0.0", first_symbols,
        (uint32_t)(sizeof(first_symbols) / sizeof(first_symbols[0]))));
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:alias/second@1.0.0", second_symbols,
        (uint32_t)(sizeof(second_symbols) / sizeof(second_symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("shared_host_instance_type.wasm"));
    ASSERT_TRUE(helper.load_component());

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    WASMComponentInstance *instance = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);

    EXPECT_EQ(instance, nullptr);
    if (instance) {
        wasm_component_deinstantiate(instance);
    }
    EXPECT_NE(strstr(helper.error_buf,
                     "Function use-item not found in native symbols"),
              nullptr)
        << helper.error_buf;
}

TEST_F(ComponentHostImportsTest, LargeAliasSetUsesBoundedCloneStorage)
{
    NativeSymbol symbols[] = {
        { "ping", (void *)host_log, "()", nullptr },
    };

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:alias/large@1.0.0", symbols,
        (uint32_t)(sizeof(symbols) / sizeof(symbols[0]))));
    ASSERT_TRUE(helper.read_wasm_file("large_shared_host_aliases.wasm"));
    ASSERT_TRUE(helper.load_component());

    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    ASSERT_EQ(helper.component_inst->component_instances_count, 1u);
    WASMComponentInstance *host = helper.component_inst->component_instances[0];
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(host->types_count, 66u);
    for (uint32_t idx = 1; idx <= 64; idx++) {
        EXPECT_EQ(host->types[idx], host->types[0]);
    }
    EXPECT_LT(host->defined_types_size, 20u * 1024u);
    EXPECT_NE(
        host->types[0],
        helper.component_inst->types[0]->type_specific.instance->types[0]);
}

TEST_F(ComponentHostImportsTest,
       OwnsBindingsForStructurallySharedHostInstanceTypes)
{
    static int first_native_attachment;
    static int second_native_attachment;
    NativeSymbol first_symbols[] = {
        { "use-item", (void *)first_use_item, "(i)", &first_native_attachment },
    };
    NativeSymbol second_symbols[] = {
        { "use-item", (void *)second_use_item, "(i)",
          &second_native_attachment },
    };

    ASSERT_TRUE(helper.runtime_init);
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:alias/first@1.0.0", first_symbols,
        (uint32_t)(sizeof(first_symbols) / sizeof(first_symbols[0]))));
    ASSERT_TRUE(wasm_runtime_register_natives_raw(
        "test:alias/second@1.0.0", second_symbols,
        (uint32_t)(sizeof(second_symbols) / sizeof(second_symbols[0]))));

    ASSERT_TRUE(helper.read_wasm_file("shared_host_instance_type.wasm"));
    ASSERT_TRUE(helper.load_component());
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper.component, "test:alias/first@1.0.0", "item", first_item_drop));
    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        helper.component, "test:alias/second@1.0.0", "item", second_item_drop));

    auto import_sections = helper.get_section(WASM_COMP_SECTION_IMPORTS);
    ASSERT_EQ(import_sections.size(), 1u);
    WASMComponentImportSection *imports =
        import_sections[0]->parsed.import_section;
    ASSERT_NE(imports, nullptr);
    ASSERT_EQ(imports->count, 2u);
    ASSERT_NE(imports->imports[0].extern_desc, nullptr);
    ASSERT_NE(imports->imports[1].extern_desc, nullptr);
    EXPECT_EQ(imports->imports[0].extern_desc->extern_desc.instance.type_idx,
              imports->imports[1].extern_desc->extern_desc.instance.type_idx);

    HostImportDropState drop_state = {};
    struct InstantiationArgs2 *args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&args));
    ASSERT_NE(args, nullptr);
    wasm_runtime_instantiation_args_set_default_stack_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 64 * 1024);
    wasm_runtime_instantiation_args_set_custom_data(args, &drop_state);

    helper.component_inst = wasm_component_instantiate_ex2(
        helper.component, args, helper.error_buf, sizeof(helper.error_buf));
    wasm_runtime_instantiation_args_destroy(args);
    ASSERT_NE(helper.component_inst, nullptr) << helper.error_buf;
    helper.component_instantiated = true;

    ASSERT_EQ(helper.component_inst->types_count, 1u);
    ASSERT_EQ(helper.component_inst->component_instances_count, 2u);
    WASMComponentInstTypeInstance *type_template =
        helper.component_inst->types[0]->type_specific.instance;
    ASSERT_NE(type_template, nullptr);
    ASSERT_EQ(type_template->func_count, 1u);
    WASMComponentInstance *first =
        helper.component_inst->component_instances[0];
    WASMComponentInstance *second =
        helper.component_inst->component_instances[1];
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(first->functions_count, 1u);
    ASSERT_EQ(second->functions_count, 1u);

    WASMComponentResourceInstance *template_resource =
        find_template_resource(type_template);
    WASMComponentResourceInstance *first_resource = find_direct_resource(first);
    WASMComponentResourceInstance *second_resource =
        find_direct_resource(second);
    ASSERT_NE(template_resource, nullptr);
    ASSERT_NE(first_resource, nullptr);
    ASSERT_NE(second_resource, nullptr);
    EXPECT_NE(first_resource, template_resource);
    EXPECT_NE(second_resource, template_resource);
    EXPECT_NE(first_resource, second_resource);
    ASSERT_NE(first_resource->interface_name, nullptr);
    ASSERT_NE(second_resource->interface_name, nullptr);
    EXPECT_STREQ(first_resource->interface_name, "test:alias/first@1.0.0");
    EXPECT_STREQ(second_resource->interface_name, "test:alias/second@1.0.0");
    EXPECT_EQ(first_resource->host_drop_callback, first_item_drop);
    EXPECT_EQ(second_resource->host_drop_callback, second_item_drop);
    EXPECT_EQ(first_resource->host_drop_attachment, &drop_state);
    EXPECT_EQ(second_resource->host_drop_attachment, &drop_state);

    /* Resolving the imports must not turn the parsed structural template into
     * either of its two concrete host interfaces. */
    EXPECT_EQ(template_resource->interface_name, nullptr);
    EXPECT_FALSE(template_resource->is_host);
    EXPECT_EQ(template_resource->host_drop_callback, nullptr);
    EXPECT_EQ(template_resource->host_drop_attachment, nullptr);

    WASMFunctionInstance *template_core = &type_template->defined_core_funcs[0];
    WASMFunctionInstance *first_core = first->functions[0]->core_func;
    WASMFunctionInstance *second_core = second->functions[0]->core_func;
    ASSERT_NE(template_core->u.func_import, nullptr);
    ASSERT_NE(first_core, nullptr);
    ASSERT_NE(second_core, nullptr);
    ASSERT_NE(first_core->u.func_import, nullptr);
    ASSERT_NE(second_core->u.func_import, nullptr);
    ASSERT_NE(first_core->u.func_import->func_type, nullptr);
    ASSERT_NE(second_core->u.func_import->func_type, nullptr);
    EXPECT_EQ(first_core->u.func_import->func_type->param_count, 1u);
    EXPECT_EQ(first_core->u.func_import->func_type->result_count, 0u);
    EXPECT_EQ(first_core->u.func_import->func_type->types[0], VALUE_TYPE_I32);
    EXPECT_EQ(second_core->u.func_import->func_type->param_count, 1u);
    EXPECT_EQ(second_core->u.func_import->func_type->result_count, 0u);
    EXPECT_EQ(second_core->u.func_import->func_type->types[0], VALUE_TYPE_I32);
    EXPECT_NE(first_core, template_core);
    EXPECT_NE(second_core, template_core);
    EXPECT_NE(first_core, second_core);
    EXPECT_NE(first_core->u.func_import, template_core->u.func_import);
    EXPECT_NE(second_core->u.func_import, template_core->u.func_import);
    EXPECT_NE(first_core->u.func_import, second_core->u.func_import);
    EXPECT_EQ(first_core->u.func_import->func_ptr_linked,
              (void *)first_use_item);
    EXPECT_EQ(second_core->u.func_import->func_ptr_linked,
              (void *)second_use_item);
    EXPECT_EQ(first_core->u.func_import->attachment, &first_native_attachment);
    EXPECT_EQ(second_core->u.func_import->attachment,
              &second_native_attachment);
    EXPECT_TRUE(first_core->u.func_import->call_conv_raw);
    EXPECT_TRUE(second_core->u.func_import->call_conv_raw);
    EXPECT_EQ(template_core->u.func_import->func_ptr_linked, nullptr);
    EXPECT_EQ(template_core->u.func_import->signature, nullptr);
    EXPECT_EQ(template_core->u.func_import->attachment, nullptr);
    EXPECT_FALSE(template_core->u.func_import->call_conv_raw);

    EXPECT_NE(first->functions[0]->func_type, type_template->funcs[0]);
    EXPECT_NE(second->functions[0]->func_type, type_template->funcs[0]);
    EXPECT_NE(first->functions[0]->func_type, second->functions[0]->func_type);
    WASMComponentTypeInstance *first_param =
        first->functions[0]->func_type->params->params[0].type;
    WASMComponentTypeInstance *second_param =
        second->functions[0]->func_type->params->params[0].type;
    ASSERT_NE(first_param, nullptr);
    ASSERT_NE(second_param, nullptr);
    ASSERT_NE(first_param->type_specific.resource_handle, nullptr);
    ASSERT_NE(second_param->type_specific.resource_handle, nullptr);
    EXPECT_NE(first_param, second_param);
    EXPECT_EQ(first_param->type_specific.resource_handle->resource,
              first_resource);
    EXPECT_EQ(second_param->type_specific.resource_handle->resource,
              second_resource);

    WASMResourceHandle *first_handle =
        wasm_create_resource_handle(first_resource, 101, true);
    WASMResourceHandle *second_handle =
        wasm_create_resource_handle(second_resource, 202, true);
    ASSERT_NE(first_handle, nullptr);
    ASSERT_NE(second_handle, nullptr);
    EXPECT_TRUE(wasm_drop_resource_handle(first_handle));
    EXPECT_TRUE(wasm_drop_resource_handle(second_handle));
    EXPECT_EQ(drop_state.first_count, 1u);
    EXPECT_EQ(drop_state.first_rep, 101u);
    EXPECT_EQ(drop_state.second_count, 1u);
    EXPECT_EQ(drop_state.second_rep, 202u);
}

} // namespace
