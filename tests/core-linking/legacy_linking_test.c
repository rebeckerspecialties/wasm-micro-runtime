/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/* Integration-only parity check: not a dependency of the core-only feature. */
#include "wasm_component_runtime.h"
#include "wasm_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static WASMModule *
load(const char *name)
{
    FILE *f = fopen(name, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > 0);
    rewind(f);
    uint8 *bytes = malloc(length);
    assert(bytes && fread(bytes, 1, length, f) == (size_t)length);
    fclose(f);
    LoadArgs args = { 0 };
    args.no_resolve = true;
    args.clone_wasm_binary = true;
    char error[256] = { 0 };
    WASMModule *module = (WASMModule *)wasm_runtime_load_ex(bytes, length, &args,
                                                         error, sizeof(error));
    free(bytes);
    if (!module)
        fprintf(stderr, "%s\n", error);
    assert(module);
    return module;
}

static uint32
call(WASMModuleInstance *instance, const char *name, bool argument)
{
    wasm_exec_env_t env = wasm_runtime_create_exec_env(
        (wasm_module_inst_t)instance, 65536);
    wasm_function_inst_t f = wasm_runtime_lookup_function(
        (wasm_module_inst_t)instance, name);
    uint32 value = 5;
    assert(env && f && wasm_runtime_call_wasm(env, f, argument, &value));
    wasm_runtime_destroy_exec_env(env);
    return value;
}

int
main(void)
{
    assert(wasm_runtime_init());
    WASMModule *owner_module = load("prelinked_table_owner.wasm");
    WASMModule *provider_module = load("prelinked_table_provider.wasm");
    WASMModule *initializer_module = load("prelinked_table_initializer.wasm");
    for (unsigned order = 0; order < 2; ++order) {
        struct InstantiationArgs2 args;
        wasm_runtime_instantiation_args_set_defaults(&args);
        wasm_runtime_instantiation_args_set_host_managed_heap_size(&args, 0);
        char error[256] = { 0 };
        WASMModuleInstance *owner = wasm_instantiate(owner_module, NULL, NULL,
                                                   &args, error, sizeof(error));
        WASMModuleInstance *provider = wasm_instantiate(provider_module, NULL,
                                                      NULL, &args, error, sizeof(error));
        assert(owner && provider);
        WASMFunctionInstance *function = (WASMFunctionInstance *)
            wasm_runtime_lookup_function((wasm_module_inst_t)provider, "add-seven");
        WASMTableInstance *table = owner->tables[0];
        WASMCoreImports imports = { 0 };
        imports.func_count = imports.tables_count = 1;
        imports.func_instance = &function;
        imports.table_instance = &table;
        WASMComponentInstance component = { 0 };
        component.instantiation_args = args;
        WASMModuleInstance *initializer = wasm_instantiate_with_imports(
            initializer_module, &component, &imports, &args, error, sizeof(error));
        assert(initializer && initializer->tables[0] == table);
        assert(call(provider, "call-local", true) == 12);
        assert(call(owner, "call-local", true) == 105);
        assert(call(owner, "call", true) == 12);
        call(initializer, "mutate-table", false);
        assert(call(owner, "call", true) == 12);
        call(initializer, "copy-local-to-borrowed", false);
        assert(call(owner, "call", true) == 12);
        call(initializer, "copy-borrowed-to-local", false);
        assert(call(initializer, "call-local", true) == 12);
        if (order) {
            wasm_deinstantiate(initializer, false);
            wasm_deinstantiate(owner, false);
        }
        else {
            wasm_deinstantiate(owner, false);
            wasm_deinstantiate(initializer, false);
        }
        wasm_deinstantiate(provider, false);
    }
    wasm_runtime_unload((wasm_module_t)initializer_module);
    wasm_runtime_unload((wasm_module_t)provider_module);
    wasm_runtime_unload((wasm_module_t)owner_module);
    wasm_runtime_destroy();
    puts("legacy table linking, provenance, mutation and teardown passed");
    return 0;
}
