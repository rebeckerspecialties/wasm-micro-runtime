/*
 * Copyright (C) 2026 Matt Hargett. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

extern "C" {
#include "wasm_component_runtime.h"
#include "wasm_export.h"
#include "wasm_runtime.h"
#include "wasm_c_api_internal.h"
}

namespace {

struct RawHostState {
    uint32_t expected_cookie;
    std::atomic<uint32_t> calls{ 0 };
    std::atomic<bool> saw_custom_data{ false };
};

struct ThreadResult {
    bool ok = false;
    char error[128] = {};
};

static void
raw_notify(wasm_exec_env_t exec_env, uint64_t *)
{
    auto *state = static_cast<RawHostState *>(
        wasm_component_get_custom_data_from_exec_env(exec_env));
    if (state && state->expected_cookie == 0xC0DEC0DE) {
        state->saw_custom_data.store(true, std::memory_order_relaxed);
        state->calls.fetch_add(1, std::memory_order_relaxed);
    }
}

static std::vector<uint8_t>
read_fixture(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

static WASMMemoryInstance *
find_exported_memory(WASMModuleInstance *instance, const char *name)
{
    for (uint32_t i = 0; i < instance->export_memory_count; i++) {
        if (strcmp(instance->export_memories[i].name, name) == 0)
            return instance->export_memories[i].memory;
    }
    return nullptr;
}

static WASMTableInstance *
find_exported_table(WASMModuleInstance *instance, const char *name)
{
    for (uint32_t i = 0; i < instance->export_table_count; i++) {
        if (strcmp(instance->export_tables[i].name, name) == 0)
            return instance->export_tables[i].table;
    }
    return nullptr;
}

static WASMGlobalInstance *
find_exported_global(WASMModuleInstance *instance, const char *name)
{
    for (uint32_t i = 0; i < instance->export_global_count; i++) {
        if (strcmp(instance->export_globals[i].name, name) == 0)
            return instance->export_globals[i].global;
    }
    return nullptr;
}

static void
run_prelinked_instance(WASMModule *owner_module, WASMModule *consumer_module,
                       WASMFunctionInstance *host_function,
                       ThreadResult *result)
{
    WASMModuleInstance *owner = nullptr;
    WASMModuleInstance *consumer = nullptr;
    RawHostState state{ 0xC0DEC0DE };
    WASMComponentInstance component_instance = {};
    struct InstantiationArgs2 args;

    if (!wasm_runtime_init_thread_env()) {
        snprintf(result->error, sizeof(result->error),
                 "failed to initialize WAMR thread environment");
        return;
    }

    wasm_runtime_instantiation_args_set_defaults(&args);
    wasm_runtime_instantiation_args_set_default_stack_size(&args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(&args, 0);

    owner = wasm_instantiate(owner_module, nullptr, nullptr, &args,
                             result->error, sizeof(result->error));
    if (owner) {
        WASMMemoryInstance *memory = find_exported_memory(owner, "memory");
        WASMTableInstance *table = find_exported_table(owner, "table");
        WASMGlobalInstance *global = find_exported_global(owner, "global");
        WASMFunctionInstance *functions[] = { host_function };
        WASMMemoryInstance *memories[] = { memory };
        WASMTableInstance *tables[] = { table };
        WASMGlobalInstance *globals[] = { global };
        WASMCoreImports imports = {};

        imports.func_count = 1;
        imports.func_instance = functions;
        imports.mem_count = 1;
        imports.mem_instance = memories;
        imports.tables_count = 1;
        imports.table_instance = tables;
        imports.globals_count = 1;
        imports.global_instance = globals;

        component_instance.custom_data = &state;
        component_instance.instantiation_args = args;
        consumer = wasm_instantiate_with_imports(
            consumer_module, &component_instance, &imports, &args,
            result->error, sizeof(result->error));

        if (consumer && memory && table && global) {
            wasm_global_inst_t exported_global = {};
            uint32_t stored_global = 0;
            uint32_t current_global = 0;
            bool found_global = wasm_runtime_get_export_global_inst(
                reinterpret_cast<wasm_module_inst_t>(consumer), "global",
                &exported_global);
            memcpy(&stored_global, memory->memory_data, sizeof(stored_global));
            memcpy(&current_global,
                   global->module_instance->global_data + global->data_offset,
                   sizeof(current_global));

            result->ok =
                state.calls.load(std::memory_order_relaxed) == 1
                && state.saw_custom_data.load(std::memory_order_relaxed)
                && stored_global == 41 && current_global == 42 && found_global
                && exported_global.global_data
                       == global->module_instance->global_data
                              + global->data_offset
                && consumer->memories[0] == memory
                && consumer->tables[0] == table
                && consumer->e->globals[0].import_global_inst == global
                && find_exported_memory(consumer, "memory") == memory
                && find_exported_table(consumer, "table") == table
                && find_exported_global(consumer, "global")
                       == &consumer->e->globals[0];
        }
    }

    /* Component instances are currently destroyed in definition order.  The
       importer must therefore tolerate its memory owner disappearing first. */
    if (owner)
        wasm_deinstantiate(owner, false);
    if (consumer)
        wasm_deinstantiate(consumer, false);
    wasm_runtime_destroy_thread_env();
}

static void
run_prelinked_table_instance(WASMModule *owner_module,
                             WASMModule *provider_module,
                             WASMModule *initializer_module,
                             ThreadResult *result)
{
    WASMModuleInstance *owner = nullptr;
    WASMModuleInstance *provider = nullptr;
    WASMModuleInstance *initializer = nullptr;
    wasm_exec_env_t exec_env = nullptr;
    WASMComponentInstance component_instance = {};
    struct InstantiationArgs2 args;
    bool local_table_ok = false;
    bool mutation_ok = false;
    bool foreign_get_closed = false;
    bool foreign_copy_closed = false;
    bool owner_local_ok = false;
    bool host_table_api_ok = false;

    if (!wasm_runtime_init_thread_env()) {
        snprintf(result->error, sizeof(result->error),
                 "failed to initialize WAMR thread environment");
        return;
    }

    wasm_runtime_instantiation_args_set_defaults(&args);
    wasm_runtime_instantiation_args_set_default_stack_size(&args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(&args, 0);

    provider = wasm_instantiate(provider_module, nullptr, nullptr, &args,
                                result->error, sizeof(result->error));
    owner = wasm_instantiate(owner_module, nullptr, nullptr, &args,
                             result->error, sizeof(result->error));
    if (owner && provider) {
        WASMTableInstance *table = find_exported_table(owner, "table");
        wasm_function_inst_t call_local = wasm_runtime_lookup_function(
            reinterpret_cast<wasm_module_inst_t>(provider), "call-local");
        uint32_t local_argv[] = { 5 };

        exec_env = wasm_runtime_create_exec_env(
            reinterpret_cast<wasm_module_inst_t>(provider), 64 * 1024);
        local_table_ok =
            call_local && exec_env && provider->table_count == 1
            && !provider->tables[0]->component_func_refs
            && wasm_runtime_call_wasm(exec_env, call_local, 1, local_argv)
            && local_argv[0] == 12;
        if (!local_table_ok) {
            snprintf(result->error, sizeof(result->error),
                     "local table regression failed (tables=%u sidecar=%p "
                     "value=%u)",
                     provider->table_count,
                     provider->table_count ? static_cast<void *>(
                         provider->tables[0]->component_func_refs)
                                           : nullptr,
                     local_argv[0]);
        }
        if (exec_env) {
            wasm_runtime_destroy_exec_env(exec_env);
            exec_env = nullptr;
        }

        auto *provider_function = reinterpret_cast<WASMFunctionInstance *>(
            wasm_runtime_lookup_function(
                reinterpret_cast<wasm_module_inst_t>(provider), "add-seven"));
        WASMFunctionInstance *functions[] = { provider_function };
        WASMTableInstance *tables[] = { table };
        WASMCoreImports imports = {};

        imports.func_count = 1;
        imports.func_instance = functions;
        imports.tables_count = 1;
        imports.table_instance = tables;
        component_instance.instantiation_args = args;
        initializer = wasm_instantiate_with_imports(
            initializer_module, &component_instance, &imports, &args,
            result->error, sizeof(result->error));

        if (initializer && table) {
            wasm_function_inst_t mutate_table = wasm_runtime_lookup_function(
                reinterpret_cast<wasm_module_inst_t>(initializer),
                "mutate-table");
            wasm_function_inst_t copy_local_to_borrowed =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(initializer),
                    "copy-local-to-borrowed");
            wasm_function_inst_t copy_borrowed_to_local =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(initializer),
                    "copy-borrowed-to-local");
            wasm_function_inst_t call_initializer_local =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(initializer),
                    "call-local");
            wasm_function_inst_t call = wasm_runtime_lookup_function(
                reinterpret_cast<wasm_module_inst_t>(owner), "call");
            wasm_function_inst_t get_foreign_and_store =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(owner),
                    "get-foreign-and-store");
            wasm_function_inst_t copy_foreign_to_local =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(owner),
                    "copy-foreign-to-local");
            wasm_function_inst_t call_owner_local =
                wasm_runtime_lookup_function(
                    reinterpret_cast<wasm_module_inst_t>(owner), "call-local");
            uint32_t argv[] = { 5 };
            uint32_t local_argv[] = { 5 };

            exec_env = wasm_runtime_create_exec_env(
                reinterpret_cast<wasm_module_inst_t>(initializer), 64 * 1024);
            mutation_ok =
                mutate_table && copy_local_to_borrowed && copy_borrowed_to_local
                && call_initializer_local && exec_env
                && wasm_runtime_call_wasm(exec_env, mutate_table, 0, nullptr)
                && wasm_runtime_call_wasm(exec_env, copy_borrowed_to_local, 0,
                                          nullptr)
                && wasm_runtime_call_wasm(exec_env, call_initializer_local, 1,
                                          local_argv)
                && local_argv[0] == 12
                && wasm_runtime_call_wasm(exec_env, copy_local_to_borrowed, 0,
                                          nullptr)
                && table->cur_size == 1 && table->component_func_refs
                && table->component_func_refs[0]
                       == &initializer->e->functions[0];
            if (!mutation_ok && result->error[0] == '\0') {
                snprintf(result->error, sizeof(result->error),
                         "borrowed table mutation failed (size=%u sidecar=%p "
                         "ref0=%p expected=%p)",
                         table->cur_size,
                         static_cast<void *>(table->component_func_refs),
                         table->component_func_refs ? static_cast<void *>(
                             table->component_func_refs[0])
                                                    : nullptr,
                         static_cast<void *>(&initializer->e->functions[0]));
            }
            if (exec_env) {
                wasm_runtime_destroy_exec_env(exec_env);
                exec_env = nullptr;
            }

            if (mutation_ok) {
                wasm_store_t store = {};
                wasm_valtype_t val_type = {};
                wasm_tabletype_t table_type = {};
                wasm_table_t table_api = {};
                wasm_ref_t *foreign_ref;
                WASMFunctionInstance *runtime_ref;
                wasm_table_inst_t table_view = {};
                bool view_ok;
                bool get_ok;
                bool clear_ok = false;
                bool restore_ok = false;

                val_type.kind = WASM_FUNCREF;
                table_type.val_type = &val_type;
                table_api.store = &store;
                table_api.kind = WASM_EXTERN_TABLE;
                table_api.type = &table_type;
                table_api.table_idx_rt = 0;
                table_api.inst_comm_rt =
                    reinterpret_cast<WASMModuleInstanceCommon *>(owner);

                foreign_ref = wasm_table_get(&table_api, 0);
                view_ok = wasm_runtime_get_export_table_inst(
                    reinterpret_cast<wasm_module_inst_t>(owner), "table",
                    &table_view);
                runtime_ref =
                    view_ok ? reinterpret_cast<WASMFunctionInstance *>(
                        wasm_table_get_func_inst(
                            reinterpret_cast<wasm_module_inst_t>(owner),
                            &table_view, 0))
                            : nullptr;
                get_ok = view_ok && foreign_ref
                         && foreign_ref->inst_comm_rt
                                == reinterpret_cast<WASMModuleInstanceCommon *>(
                                    initializer)
                         && foreign_ref->ref_idx_rt
                                == initializer->e->functions[0].func_idx
                         && runtime_ref == &initializer->e->functions[0]
                         && runtime_ref != &owner->e->functions[0];

                if (get_ok) {
                    clear_ok =
                        wasm_table_set(&table_api, 0, nullptr)
                        && table->elems[0] == NULL_REF
                        && table->component_func_refs[0] == nullptr
                        && wasm_table_get_func_inst(
                               reinterpret_cast<wasm_module_inst_t>(owner),
                               &table_view, 0)
                               == nullptr;
                }
                if (clear_ok) {
                    restore_ok = wasm_table_set(&table_api, 0, foreign_ref);
                }
                host_table_api_ok =
                    restore_ok
                    && table->component_func_refs[0]
                           == &initializer->e->functions[0]
                    && reinterpret_cast<WASMFunctionInstance *>(
                           wasm_table_get_func_inst(
                               reinterpret_cast<wasm_module_inst_t>(owner),
                               &table_view, 0))
                           == &initializer->e->functions[0];
                if (foreign_ref) {
                    wasm_ref_delete(foreign_ref);
                }
                if (!host_table_api_ok && result->error[0] == '\0') {
                    snprintf(result->error, sizeof(result->error),
                             "host table API lost function provenance");
                }
            }

            exec_env = wasm_runtime_create_exec_env(
                reinterpret_cast<wasm_module_inst_t>(owner), 64 * 1024);
            if (call && get_foreign_and_store && copy_foreign_to_local
                && call_owner_local && exec_env
                && wasm_runtime_call_wasm(exec_env, call, 1, argv)
                && argv[0] == 12) {
                foreign_get_closed = !wasm_runtime_call_wasm(
                    exec_env, get_foreign_and_store, 0, nullptr);
                const char *exception = wasm_runtime_get_exception(
                    reinterpret_cast<wasm_module_inst_t>(owner));
                foreign_get_closed =
                    foreign_get_closed && exception
                    && strstr(exception, "foreign function reference");
                wasm_runtime_clear_exception(
                    reinterpret_cast<wasm_module_inst_t>(owner));

                foreign_copy_closed = !wasm_runtime_call_wasm(
                    exec_env, copy_foreign_to_local, 0, nullptr);
                exception = wasm_runtime_get_exception(
                    reinterpret_cast<wasm_module_inst_t>(owner));
                foreign_copy_closed =
                    foreign_copy_closed && exception
                    && strstr(exception, "foreign function reference");
                wasm_runtime_clear_exception(
                    reinterpret_cast<wasm_module_inst_t>(owner));

                local_argv[0] = 5;
                owner_local_ok = wasm_runtime_call_wasm(
                                     exec_env, call_owner_local, 1, local_argv)
                                 && local_argv[0] == 105;
            }
            result->ok = local_table_ok && mutation_ok && foreign_get_closed
                         && foreign_copy_closed && owner_local_ok
                         && host_table_api_ok && initializer->tables[0] == table
                         && table->component_func_refs
                         && table->component_func_refs[0]
                                == &initializer->e->functions[0];
            if (!result->ok && exec_env) {
                const char *exception = wasm_runtime_get_exception(
                    reinterpret_cast<wasm_module_inst_t>(owner));
                if (exception) {
                    snprintf(result->error, sizeof(result->error), "%s",
                             exception);
                }
            }
        }
    }

    if (exec_env)
        wasm_runtime_destroy_exec_env(exec_env);
    /* Exercise the component runtime's current definition-order teardown. */
    if (owner)
        wasm_deinstantiate(owner, false);
    if (initializer)
        wasm_deinstantiate(initializer, false);
    if (provider)
        wasm_deinstantiate(provider, false);
    wasm_runtime_destroy_thread_env();
}

static bool
run_prelinked_limits_case(WASMModule *source_module,
                          WASMModule *consumer_module, bool expect_success,
                          const char *expected_error)
{
    WASMModuleInstance *source = nullptr;
    WASMModuleInstance *consumer = nullptr;
    WASMComponentInstance component_instance = {};
    struct InstantiationArgs2 args;
    char error[256] = {};
    bool result = false;

    wasm_runtime_instantiation_args_set_defaults(&args);
    wasm_runtime_instantiation_args_set_default_stack_size(&args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(&args, 0);

    source = wasm_instantiate(source_module, nullptr, nullptr, &args, error,
                              sizeof(error));
    if (source) {
        WASMTableInstance *table = find_exported_table(source, "table");
        WASMMemoryInstance *memory = find_exported_memory(source, "memory");
        WASMTableInstance *tables[] = { table };
        WASMMemoryInstance *memories[] = { memory };
        WASMCoreImports imports = {};

        imports.tables_count = 1;
        imports.table_instance = tables;
        imports.mem_count = 1;
        imports.mem_instance = memories;
        component_instance.instantiation_args = args;
        consumer = wasm_instantiate_with_imports(consumer_module,
                                                 &component_instance, &imports,
                                                 &args, error, sizeof(error));
        result = expect_success ? consumer != nullptr
                                : consumer == nullptr && expected_error
                                      && strstr(error, expected_error);
        if (!result) {
            ADD_FAILURE() << "unexpected prelinked limit result: " << error;
        }
    }

    if (source)
        wasm_deinstantiate(source, false);
    if (consumer)
        wasm_deinstantiate(consumer, false);
    return result;
}

class PrelinkedCoreImportsTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    }

    void TearDown() override { wasm_runtime_destroy(); }
};

TEST_F(PrelinkedCoreImportsTest,
       BindsBeforeStartAndBorrowsResourcesAcrossTwoThreads)
{
    std::vector<uint8_t> owner_bytes = read_fixture("prelinked_owner.wasm");
    std::vector<uint8_t> consumer_bytes =
        read_fixture("prelinked_consumer.wasm");
    char error_buf[128] = {};
    LoadArgs load_args = {};

    ASSERT_FALSE(owner_bytes.empty());
    ASSERT_FALSE(consumer_bytes.empty());
    load_args.no_resolve = true;
    load_args.is_component = false;

    auto *owner_module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
        owner_bytes.data(), static_cast<uint32_t>(owner_bytes.size()),
        &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(owner_module, nullptr) << error_buf;
    auto *consumer_module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
        consumer_bytes.data(), static_cast<uint32_t>(consumer_bytes.size()),
        &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(consumer_module, nullptr) << error_buf;

    WASMFunctionImport host_import = {};
    host_import.module_name = const_cast<char *>("host");
    host_import.field_name = const_cast<char *>("notify");
    host_import.func_type =
        consumer_module->import_functions[0].u.function.func_type;
    host_import.func_ptr_linked = reinterpret_cast<void *>(raw_notify);
    host_import.signature = "()";
    host_import.call_conv_raw = true;

    WASMFunctionInstance host_function = {};
    host_function.is_import_func = true;
    host_function.u.func_import = &host_import;

    ThreadResult results[2];
    std::thread first(run_prelinked_instance, owner_module, consumer_module,
                      &host_function, &results[0]);
    std::thread second(run_prelinked_instance, owner_module, consumer_module,
                       &host_function, &results[1]);
    first.join();
    second.join();

    EXPECT_TRUE(results[0].ok) << results[0].error;
    EXPECT_TRUE(results[1].ok) << results[1].error;

    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(consumer_module));
    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(owner_module));
}

TEST_F(PrelinkedCoreImportsTest,
       ImportedFuncrefTableTracksInitializerProvenanceAcrossTwoThreads)
{
    std::vector<uint8_t> owner_bytes =
        read_fixture("prelinked_table_owner.wasm");
    std::vector<uint8_t> provider_bytes =
        read_fixture("prelinked_table_provider.wasm");
    std::vector<uint8_t> initializer_bytes =
        read_fixture("prelinked_table_initializer.wasm");
    char error_buf[128] = {};
    LoadArgs load_args = {};

    ASSERT_FALSE(owner_bytes.empty());
    ASSERT_FALSE(provider_bytes.empty());
    ASSERT_FALSE(initializer_bytes.empty());
    load_args.no_resolve = true;
    load_args.is_component = false;

    auto *owner_module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
        owner_bytes.data(), static_cast<uint32_t>(owner_bytes.size()),
        &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(owner_module, nullptr) << error_buf;
    auto *provider_module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
        provider_bytes.data(), static_cast<uint32_t>(provider_bytes.size()),
        &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(provider_module, nullptr) << error_buf;
    auto *initializer_module = reinterpret_cast<WASMModule *>(
        wasm_runtime_load_ex(initializer_bytes.data(),
                             static_cast<uint32_t>(initializer_bytes.size()),
                             &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(initializer_module, nullptr) << error_buf;

    ThreadResult results[2];
    std::thread first(run_prelinked_table_instance, owner_module,
                      provider_module, initializer_module, &results[0]);
    std::thread second(run_prelinked_table_instance, owner_module,
                       provider_module, initializer_module, &results[1]);
    first.join();
    second.join();

    EXPECT_TRUE(results[0].ok) << results[0].error;
    EXPECT_TRUE(results[1].ok) << results[1].error;

    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(initializer_module));
    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(provider_module));
    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(owner_module));
}

TEST_F(PrelinkedCoreImportsTest, EnforcesTableAndMemoryMaximumSubtyping)
{
    const char *fixture_names[] = {
        "prelinked_limits_consumer.wasm",
        "prelinked_limits_compatible.wasm",
        "prelinked_limits_wide_table.wasm",
        "prelinked_limits_wide_memory.wasm",
        "prelinked_limits_unbounded.wasm",
        "prelinked_limits_unbounded_memory.wasm",
    };
    WASMModule *modules[6] = {};
    char error_buf[128] = {};
    LoadArgs load_args = {};

    load_args.no_resolve = true;
    load_args.is_component = false;
    for (size_t i = 0; i < 6; i++) {
        std::vector<uint8_t> bytes = read_fixture(fixture_names[i]);
        ASSERT_FALSE(bytes.empty()) << fixture_names[i];
        modules[i] = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
            bytes.data(), static_cast<uint32_t>(bytes.size()), &load_args,
            error_buf, sizeof(error_buf)));
        ASSERT_NE(modules[i], nullptr) << fixture_names[i] << ": " << error_buf;
    }

    EXPECT_TRUE(
        run_prelinked_limits_case(modules[1], modules[0], true, nullptr));
    EXPECT_TRUE(run_prelinked_limits_case(modules[2], modules[0], false,
                                          "table import"));
    EXPECT_TRUE(run_prelinked_limits_case(modules[3], modules[0], false,
                                          "memory import"));
    EXPECT_TRUE(run_prelinked_limits_case(modules[4], modules[0], false,
                                          "table import"));
    EXPECT_TRUE(run_prelinked_limits_case(modules[5], modules[0], false,
                                          "memory import"));

    for (size_t i = 6; i > 0; i--) {
        wasm_runtime_unload(reinterpret_cast<wasm_module_t>(modules[i - 1]));
    }
}

TEST_F(PrelinkedCoreImportsTest,
       StackTopEqualToDataEndHasNoOverlappingAuxiliaryStack)
{
    std::vector<uint8_t> module_bytes =
        read_fixture("stack_top_equals_data_end.wasm");
    char error_buf[128] = {};
    LoadArgs load_args = {};
    struct InstantiationArgs2 args;

    ASSERT_FALSE(module_bytes.empty());
    load_args.no_resolve = true;
    load_args.is_component = false;

    auto *module = reinterpret_cast<WASMModule *>(wasm_runtime_load_ex(
        module_bytes.data(), static_cast<uint32_t>(module_bytes.size()),
        &load_args, error_buf, sizeof(error_buf)));
    ASSERT_NE(module, nullptr) << error_buf;
    EXPECT_EQ(module->aux_stack_bottom, module->aux_data_end);
    EXPECT_EQ(module->aux_stack_size, 0u);

    wasm_runtime_instantiation_args_set_defaults(&args);
    wasm_runtime_instantiation_args_set_default_stack_size(&args, 64 * 1024);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(&args, 0);
    WASMModuleInstance *instance = wasm_instantiate(
        module, nullptr, nullptr, &args, error_buf, sizeof(error_buf));
    ASSERT_NE(instance, nullptr) << error_buf;

    WASMExecEnv *exec_env = wasm_runtime_get_exec_env_singleton(
        reinterpret_cast<WASMModuleInstanceCommon *>(instance));
    ASSERT_NE(exec_env, nullptr);
    wasm_function_inst_t answer = wasm_runtime_lookup_function(
        reinterpret_cast<wasm_module_inst_t>(instance), "answer");
    uint32_t result = 0;
    ASSERT_NE(answer, nullptr);
    ASSERT_TRUE(wasm_runtime_call_wasm(exec_env, answer, 0, &result));
    EXPECT_EQ(result, 42u);

    wasm_deinstantiate(instance, false);
    wasm_runtime_unload(reinterpret_cast<wasm_module_t>(module));
}

} // namespace
