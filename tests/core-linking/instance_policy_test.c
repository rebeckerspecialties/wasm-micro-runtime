/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "wasm_c_api.h"
#include "wasm_export.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

typedef struct Policy {
    pthread_mutex_t lock;
    wasm_module_inst_t published;
    unsigned created, retired, pages, ceiling;
    bool reject, stop;
} Policy;

static bool
created(void *attachment, wasm_module_inst_t instance)
{
    Policy *policy = attachment;
    assert(wasm_runtime_get_custom_data(instance) == policy);
    pthread_mutex_lock(&policy->lock);
    assert(!policy->published);
    policy->created++;
    if (!policy->reject)
        policy->published = instance;
    pthread_mutex_unlock(&policy->lock);
    return !policy->reject;
}

static void
retiring(void *attachment, wasm_module_inst_t instance)
{
    Policy *policy = attachment;
    pthread_mutex_lock(&policy->lock);
    assert(policy->published == instance);
    policy->published = NULL;
    policy->retired++;
    pthread_mutex_unlock(&policy->lock);
}

static bool
reserve(void *attachment, uint32_t pages)
{
    Policy *policy = attachment;
    if (pages > policy->ceiling - policy->pages)
        return false;
    policy->pages += pages;
    return true;
}

static void
refund(void *attachment, uint32_t pages)
{
    Policy *policy = attachment;
    assert(policy->pages >= pages);
    policy->pages -= pages;
}

static void *
interrupt(void *attachment)
{
    Policy *policy = attachment;
    const struct timespec interval = { 0, 1000000 };
    for (;;) {
        pthread_mutex_lock(&policy->lock);
        if (policy->stop) {
            pthread_mutex_unlock(&policy->lock);
            return NULL;
        }
        if (policy->published)
            wasm_runtime_terminate(policy->published);
        pthread_mutex_unlock(&policy->lock);
        nanosleep(&interval, NULL);
    }
}

static wasm_module_t *
load(wasm_store_t *store, const char *name)
{
    wasm_byte_vec_t bytes;
    wasm_module_t *module;
    FILE *file = fopen(name, "rb");
    long size;
    assert(file && !fseek(file, 0, SEEK_END));
    size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    wasm_byte_vec_new_uninitialized(&bytes, size);
    assert(bytes.data && fread(bytes.data, 1, size, file) == (size_t)size);
    fclose(file);
    module = wasm_module_new(store, &bytes);
    wasm_byte_vec_delete(&bytes);
    assert(module);
    return module;
}

static void
exercise(wasm_engine_t *engine, const char *name, bool reject, bool watchdog)
{
    Policy policy = { .lock = PTHREAD_MUTEX_INITIALIZER, .ceiling = 2,
                      .reject = reject };
    wasm_store_t *store = wasm_store_new(engine);
    wasm_module_t *module = load(store, name);
    wasm_extern_vec_t empty = WASM_EMPTY_VEC, exports;
    wasm_trap_t *trap = NULL;
    struct InstantiationArgs2 *args;
    pthread_t thread;
    assert(!wasm_store_set_instance_lifecycle(store, &policy, created, NULL));
    assert(wasm_store_set_instance_lifecycle(store, &policy, created, retiring));
    assert(wasm_runtime_instantiation_args_create(&args));
    wasm_runtime_instantiation_args_set_default_stack_size(args, 65536);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args, 0);
    wasm_runtime_instantiation_args_set_custom_data(args, &policy);
    wasm_runtime_instantiation_args_set_memory_page_quota(args, &policy,
                                                          reserve, refund);
    if (watchdog)
        assert(!pthread_create(&thread, NULL, interrupt, &policy));
    wasm_instance_t *instance =
        wasm_instance_new_with_args_ex2(store, module, &empty, &trap, args);
    if (reject || watchdog || name[0] == 's') {
        assert(!instance && trap);
        assert(policy.created == 1 && policy.retired == 0);
        wasm_trap_delete(trap);
    }
    else {
        wasm_val_t argument[] = { WASM_I32_VAL(1) };
        wasm_val_t result[] = { WASM_INIT_VAL };
        wasm_val_vec_t arguments = WASM_ARRAY_VEC(argument);
        wasm_val_vec_t results = WASM_ARRAY_VEC(result);
        assert(instance && !trap && policy.pages == 1);
        assert(!wasm_store_set_instance_lifecycle(store, NULL, NULL, NULL));
        wasm_instance_exports(instance, &exports);
        wasm_func_t *grow = wasm_extern_as_func(exports.data[2]);
        assert(!wasm_func_call(grow, &arguments, &results));
        assert(result[0].of.i32 == 1 && policy.pages == 2);
        assert(!wasm_func_call(grow, &arguments, &results));
        assert(result[0].of.i32 == -1 && policy.pages == 2);
        wasm_extern_vec_delete(&exports);
    }
    wasm_runtime_instantiation_args_destroy(args);
    /* Keep the interrupter alive across teardown: retiring fences every use. */
    wasm_store_delete(store);
    assert(!policy.published && !policy.pages);
    assert(policy.retired == (reject ? 0 : 1));
    pthread_mutex_lock(&policy.lock);
    policy.stop = true;
    pthread_mutex_unlock(&policy.lock);
    if (watchdog)
        assert(!pthread_join(thread, NULL));
    pthread_mutex_destroy(&policy.lock);
}

int
main(void)
{
    wasm_engine_t *engine = wasm_engine_new();
    assert(engine);
    exercise(engine, "memory-owner.wasm", false, false);
    exercise(engine, "memory-owner.wasm", true, false);
    exercise(engine, "start-trap.wasm", false, false);
    for (unsigned i = 0; i < 20; i++)
        exercise(engine, "start-loop.wasm", false, true);
    wasm_engine_delete(engine);
    puts("instance policy: quotas, host state, admission, failed start and "
         "watchdog retirement passed");
    return 0;
}
