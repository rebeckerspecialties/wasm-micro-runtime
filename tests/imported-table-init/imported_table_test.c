/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "wasm_export.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    char error[256];
    FILE *file = fopen("imported-table.wasm", "rb");
    long size;
    uint8_t *bytes;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec;
    wasm_function_inst_t function;
    uint32_t result = 0;
    assert(file && fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)size);
    assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(wasm_runtime_init());
    module = wasm_runtime_load(bytes, (uint32_t)size, error, sizeof(error));
    assert(module);
    instance = wasm_runtime_instantiate(module, 65536, 0, error, sizeof(error));
    assert(instance);
    exec = wasm_runtime_create_exec_env(instance, 65536);
    assert(exec);
    function = wasm_runtime_lookup_function(instance, "run");
    assert(function && wasm_runtime_call_wasm(exec, function, 0, &result));
    assert(result == 42);
    function = wasm_runtime_lookup_function(instance, "out-of-bounds");
    assert(function && !wasm_runtime_call_wasm(exec, function, 0, &result));
    assert(strstr(wasm_runtime_get_exception(instance), "out of bounds"));
    wasm_runtime_destroy_exec_env(exec);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(bytes);
    puts("PASS: imported table.init and bounds trap");
    return 0;
}
