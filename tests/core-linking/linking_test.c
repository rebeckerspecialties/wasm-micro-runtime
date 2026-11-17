/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "wasm_c_api.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int finalized;
static void
finalizer(void *env)
{
    assert(env);
    finalized++;
}

static wasm_module_t *
load(wasm_store_t *store, const char *path)
{
    FILE *file = fopen(path, "rb");
    wasm_byte_vec_t bytes;
    wasm_module_t *module;
    long size;
    assert(file && fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    wasm_byte_vec_new_uninitialized(&bytes, (size_t)size);
    assert(bytes.data
           && fread(bytes.data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    module = wasm_module_new(store, &bytes);
    wasm_byte_vec_delete(&bytes);
    assert(module);
    return module;
}

static wasm_trap_t *
value(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
{
    assert(args->num_elems == 0);
    results->data[0] = (wasm_val_t)WASM_I32_VAL(*(int *)env);
    return NULL;
}

static wasm_trap_t *
start(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
{
    (void)args;
    (void)results;
    ++*(int *)env;
    return NULL;
}

static int
call(wasm_extern_t *external, int argument, bool has_argument)
{
    wasm_val_t a[] = { WASM_I32_VAL(argument) }, r[] = { WASM_INIT_VAL };
    wasm_val_vec_t args = WASM_ARRAY_VEC(a), results = WASM_ARRAY_VEC(r);
    wasm_trap_t *trap;
    if (!has_argument)
        args.size = args.num_elems = 0;
    trap = wasm_func_call(wasm_extern_as_func(external), &args, &results);
    if (trap) {
        wasm_message_t message;
        wasm_trap_message(trap, &message);
        fprintf(stderr, "call trap: %.*s\n", (int)message.size, message.data);
    }
    assert(!trap);
    return r[0].of.i32;
}

int
main(void)
{
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store = wasm_store_new(engine);
    wasm_store_t *foreign = wasm_store_new(engine);
    wasm_module_t *owner_module = load(store, "memory-owner.wasm");
    wasm_module_t *user_module = load(store, "memory-user.wasm");
    wasm_module_t *start_module = load(store, "start-import.wasm");
    wasm_extern_vec_t empty = WASM_EMPTY_VEC, owner_exports, user_exports,
                      second_exports;
    wasm_trap_t *trap = NULL;
    wasm_instance_t *owner = wasm_instance_new_with_args(
        store, owner_module, &empty, &trap, 65536, 0);
    wasm_instance_t *user, *second;
    int first_value = 41, second_value = 77, starts = 0;
    wasm_functype_t *type = wasm_functype_new_0_1(wasm_valtype_new_i32());
    wasm_func_t *first =
        wasm_func_new_with_env(store, type, value, &first_value, finalizer);
    wasm_func_t *other =
        wasm_func_new_with_env(store, type, value, &second_value, NULL);
    wasm_func_t *alien =
        wasm_func_new_with_env(foreign, type, value, &first_value, NULL);
    wasm_extern_t *items[2];
    wasm_extern_vec_t imports = WASM_ARRAY_VEC(items);
    assert(owner && !trap);
    wasm_instance_exports(owner, &owner_exports);
    items[0] = wasm_func_as_extern(first);
    items[1] = owner_exports.data[0];
    user = wasm_instance_new_with_args(store, user_module, &imports, &trap,
                                       65536, 0);
    assert(user && !trap);
    wasm_instance_exports(user, &user_exports);
    assert(wasm_memory_data(wasm_extern_as_memory(items[1]))
           == wasm_memory_data(wasm_extern_as_memory(user_exports.data[0])));
    assert(call(owner_exports.data[1], 0, true) == 41);
    assert(call(owner_exports.data[1], 8, true) == 'l');
    assert(call(user_exports.data[2], 1, true) == 1);
    assert(wasm_memory_size(wasm_extern_as_memory(items[1])) == 2);
    assert(call(owner_exports.data[2], 1, true) == 2);
    assert(wasm_memory_size(wasm_extern_as_memory(user_exports.data[0])) == 3);
    assert(call(user_exports.data[2], 1, true) == -1);
    items[0] = wasm_func_as_extern(other);
    second = wasm_instance_new_with_args(store, user_module, &imports, &trap,
                                         65536, 0);
    assert(second && !trap);
    wasm_instance_exports(second, &second_exports);
    assert(call(user_exports.data[3], 0, false) == 41);
    assert(call(second_exports.data[3], 0, false) == 77);
    wasm_func_delete(first);
    first = NULL;
    assert(finalized == 0);
    assert(call(user_exports.data[3], 0, false) == 41);
    items[0] = wasm_func_as_extern(alien);
    assert(!wasm_instance_new_with_args(store, user_module, &imports, &trap,
                                        65536, 0));
    assert(trap);
    wasm_trap_delete(trap);
    trap = NULL;
    imports.num_elems = imports.size = 1;
    assert(!wasm_instance_new_with_args(store, user_module, &imports, &trap,
                                        65536, 0));
    assert(trap);
    wasm_trap_delete(trap);
    trap = NULL;
    wasm_functype_delete(type);
    type = wasm_functype_new_0_0();
    wasm_func_t *starter =
        wasm_func_new_with_env(store, type, start, &starts, NULL);
    items[0] = wasm_func_as_extern(starter);
    assert(wasm_instance_new_with_args(store, start_module, &imports, &trap,
                                       65536, 0));
    assert(!trap && starts == 1);
    wasm_instance_delete(owner); /* Store retains the underlying owner. */
    assert(call(user_exports.data[1], 8, true) == 'l');
    wasm_extern_vec_delete(&second_exports);
    wasm_extern_vec_delete(&user_exports);
    wasm_extern_vec_delete(&owner_exports);
    wasm_func_delete(starter);
    wasm_func_delete(first);
    wasm_func_delete(other);
    wasm_func_delete(alien);
    wasm_functype_delete(type);
    wasm_store_delete(foreign);
    wasm_store_delete(store);
    assert(finalized == 1);
    wasm_engine_delete(engine);
    puts("core linking: shared memory, growth, start callbacks, instance "
         "isolation and rejection passed");
    return 0;
}
