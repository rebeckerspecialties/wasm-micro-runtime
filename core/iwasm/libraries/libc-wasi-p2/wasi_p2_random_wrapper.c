/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasi_p2_random_wrapper.h"
#include "wasi_p2_common.h"
#include "wasm_runtime_common.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
#include "component-model/wasm_canonical_abi.h"
#include "component-model/wasm_component_canonical.h"
#include <string.h>

static bool
wit_value_array_allocation_fits(uint64_t count)
{
    return count <= UINT32_MAX && count <= UINT32_MAX / sizeof(wit_value_t);
}

static void
free_wit_value_array(wit_value_t *elems, uint32_t constructed)
{
    if (!elems)
        return;

    while (constructed > 0)
        free_wit_value(elems[--constructed]);
    wasm_runtime_free(elems);
}

static wit_value_t
u8_list_ctor(const wasi_list_u8_t *list)
{
    wit_value_t *elems = NULL;
    wit_value_t result = NULL;
    uint32_t count;
    uint32_t constructed = 0;

    if (!list || (list->buf_len > 0 && !list->buf)
        || !wit_value_array_allocation_fits(list->buf_len)) {
        return NULL;
    }

    count = (uint32_t)list->buf_len;
    if (count > 0) {
        elems = wasm_runtime_malloc(count * sizeof(wit_value_t));
        if (!elems)
            return NULL;

        for (; constructed < count; constructed++) {
            elems[constructed] = wit_u8_ctor(list->buf[constructed]);
            if (!elems[constructed]) {
                free_wit_value_array(elems, constructed);
                return NULL;
            }
        }
    }

    result = wit_list_ctor(elems, count);
    if (!result)
        free_wit_value_array(elems, constructed);
    return result;
}

static wit_value_t
u64_pair_ctor(uint64_t first, uint64_t second)
{
    wit_value_t *elems = wasm_runtime_malloc(2 * sizeof(wit_value_t));
    wit_value_t result;

    if (!elems)
        return NULL;
    elems[0] = wit_u64_ctor(first);
    if (!elems[0]) {
        wasm_runtime_free(elems);
        return NULL;
    }
    elems[1] = wit_u64_ctor(second);
    if (!elems[1]) {
        free_wit_value(elems[0]);
        wasm_runtime_free(elems);
        return NULL;
    }

    result = wit_tuple_ctor(elems, 2);
    if (!result)
        free_wit_value_array(elems, 2);
    return result;
}

/**
 * @brief Wrapper for the `get-random-bytes` function of the
 * `wasi:random/random` interface.
 * @details This function retrieves cryptographically-secure random bytes and
 *          copies them into the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param len The number of random bytes to generate.
 * @param[out] offset_addr The address where that will be populated with the
 * resulting byte array
 */
void
wasi_random_get_random_bytes_wrapper(wasm_exec_env_t exec_env, uint64_t len,
                                     uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);

    wasi_list_u8_t list = { 0 };
    wit_value_t result = NULL;
    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        set_component_exception(exec_env->cx,
                                "random capability is unavailable");
        return;
    }

    /* wasm_runtime_malloc takes a uint32_t size.  Passing a larger request to
       the native helper would truncate the allocation and then fill `len`
       bytes past it. */
    if (!wit_value_array_allocation_fits(len)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "random byte request is too large");
        goto end;
    }

    wasi_random_get_random_bytes(len, &list);
    if (list.buf_len != len || (len > 0 && !list.buf)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to allocate random byte buffer");
        goto end;
    }
    result = u8_list_ctor(&list);

end:
    if (!result
        && wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
               == NULL) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct random byte result");
    }
    if (result)
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    if (list.buf)
        wasm_runtime_free(list.buf);
    free_wit_value(result);
}

/**
 * @brief Wrapper for the `get-random-u64` function of the `wasi:random/random`
 * interface.
 * @param exec_env The execution environment.
 * @return A cryptographically-secure random 64-bit unsigned integer.
 */
uint64_t
wasi_random_get_random_u64_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }
    return wasi_random_get_random_u64();
}

/**
 * @brief Wrapper for the `get-insecure-random-bytes` function of the
 * `wasi:random/insecure` interface.
 * @details This function retrieves insecure random bytes and copies them into
 *          the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 * @param len The number of random bytes to generate.
 * @param[out] offset_addr The address where that will be populated with the
 * resulting byte array
 */
void
wasi_random_get_insecure_random_bytes_wrapper(wasm_exec_env_t exec_env,
                                              uint64_t len,
                                              uint32_t offset_addr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wasi_list_u8_t list = { 0 };
    wit_value_t result = NULL;

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        set_component_exception(exec_env->cx,
                                "random capability is unavailable");
        return;
    }

    if (!wit_value_array_allocation_fits(len)) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "insecure random byte request is too large");
        goto end;
    }

    wasi_random_get_insecure_random_bytes(len, &list);
    if (list.buf_len != len || (len > 0 && !list.buf)) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to allocate insecure random byte buffer");
        goto end;
    }
    result = u8_list_ctor(&list);

end:
    if (!result
        && wasm_runtime_get_exception((wasm_module_inst_t)exec_env->module_inst)
               == NULL) {
        wasm_runtime_set_exception(
            exec_env->module_inst,
            "failed to construct insecure random byte result");
    }
    if (result)
        (void)store(exec_env->cx, offset_addr, func_type->results->result,
                    result);
    if (list.buf)
        wasm_runtime_free(list.buf);
    free_wit_value(result);
}

/**
 * @brief Wrapper for the `get-insecure-random-u64` function of the
 * `wasi:random/insecure` interface.
 * @param exec_env The execution environment.
 * @return An insecure random 64-bit unsigned integer.
 */
uint64_t
wasi_random_get_insecure_random_u64_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);

    if (!wasi_ctx || !wasi_ctx->wasi_options || !wasi_ctx->wasi_options->cli
        || !wasi_ctx->wasi_options->common) {
        return 0;
    }
    return wasi_random_get_insecure_random_u64();
}

/**
 * @brief Wrapper for the `insecure-seed` function of the
 `wasi:random/insecure-seed` interface.
 * @details This function retrieves a new seed for the insecure random number
 *          generator and writes it back to the WebAssembly guest's memory.
 * @param exec_env The execution environment.
 @param[out] offset_addr The address where the resulting
 *                        tuple of two `u64` values will be written.
 */
void
wasi_random_insecure_seed_wrapper(wasm_exec_env_t exec_env,
                                  uint32_t offset_addr)
{

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    const WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(module_inst);
    WASMComponentFuncTypeInstance *func_type =
        wasm_get_component_func_type(exec_env);
    wit_value_t result = NULL;
    uint64_t seed1 = 0, seed2 = 0;

    if (wasi_ctx && wasi_ctx->wasi_options && wasi_ctx->wasi_options->cli
        && wasi_ctx->wasi_options->common)
        wasi_random_get_insecure_seed(&seed1, &seed2);
    result = u64_pair_ctor(seed1, seed2);
    if (!result) {
        wasm_runtime_set_exception(exec_env->module_inst,
                                   "failed to construct insecure seed result");
        return;
    }
    (void)store(exec_env->cx, offset_addr, func_type->results->result, result);
    free_wit_value(result);
}
