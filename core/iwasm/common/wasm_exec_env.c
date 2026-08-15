/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_exec_env.h"
#include "wasm_runtime_common.h"
#if WASM_ENABLE_GC != 0
#include "mem_alloc.h"
#endif
#if WASM_ENABLE_INTERP != 0
#include "../interpreter/wasm_runtime.h"
#endif
#if WASM_ENABLE_AOT != 0
#include "../aot/aot_runtime.h"
#endif

#if WASM_ENABLE_AOT != 0
#include "aot_runtime.h"
#endif

#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_LIBC_WASI_P2 != 0 \
    && WASM_ENABLE_THREAD_MGR != 0
#include "../libraries/libc-wasi-p2/wasi_p2_io.h"
#endif

#if WASM_ENABLE_THREAD_MGR != 0
#include "../libraries/thread-mgr/thread_manager.h"
#if WASM_ENABLE_DEBUG_INTERP != 0
#include "../libraries/debug-engine/debug_engine.h"
#endif
#endif

WASMExecEnv *
wasm_exec_env_create_internal(struct WASMModuleInstanceCommon *module_inst,
                              uint32 stack_size)
{
    WASMAllocationQuotaScope scope;
    uint64 total_size =
        offsetof(WASMExecEnv, wasm_stack_u.bottom) + (uint64)stack_size;
    WASMExecEnv *exec_env = NULL;

    if (!module_inst
        || !wasm_allocation_quota_scope_enter_for_allocation(&scope,
                                                             module_inst)) {
        return NULL;
    }

    if (total_size >= UINT32_MAX
        || !(exec_env = wasm_runtime_malloc((uint32)total_size)))
        goto done;

    memset(exec_env, 0, (uint32)total_size);

#if WASM_ENABLE_AOT != 0
    if (!(exec_env->argv_buf = wasm_runtime_malloc(sizeof(uint32) * 64))) {
        goto fail1;
    }
#endif

#if WASM_ENABLE_THREAD_MGR != 0
    if (os_mutex_init(&exec_env->wait_lock) != 0)
        goto fail2;

    if (os_cond_init(&exec_env->wait_cond) != 0)
        goto fail3;

#if WASM_ENABLE_DEBUG_INTERP != 0
    if (!(exec_env->current_status = wasm_cluster_create_exenv_status()))
        goto fail4;
#endif
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK
    if (!wasm_allocation_quota_reservation_acquire_for_allocation(
            &exec_env->guard_page_reservation, exec_env, os_getpagesize(), 1)) {
        goto fail5;
    }
    if (!(exec_env->exce_check_guard_page =
              os_mmap(NULL, os_getpagesize(), MMAP_PROT_NONE, MMAP_MAP_NONE,
                      os_get_invalid_handle()))) {
        wasm_allocation_quota_reservation_release(
            &exec_env->guard_page_reservation);
        goto fail5;
    }
#endif

    exec_env->module_inst = module_inst;
    exec_env->wasm_stack_size = stack_size;
    exec_env->wasm_stack.bottom = exec_env->wasm_stack_u.bottom;
    exec_env->wasm_stack.top_boundary =
        exec_env->wasm_stack.bottom + stack_size;
    exec_env->wasm_stack.top = exec_env->wasm_stack.bottom;

#if WASM_ENABLE_AOT != 0
    if (module_inst->module_type == Wasm_Module_AoT) {
        AOTModuleInstance *i = (AOTModuleInstance *)module_inst;
        AOTModule *m = (AOTModule *)i->module;
        exec_env->native_symbol = m->native_symbol_list;
    }
#endif

#if WASM_ENABLE_MEMORY_TRACING != 0
    wasm_runtime_dump_exec_env_mem_consumption(exec_env);
#endif

#if WASM_ENABLE_INSTRUCTION_METERING != 0
    exec_env->instructions_to_execute = -1;
#endif

    goto done;

#ifdef OS_ENABLE_HW_BOUND_CHECK
fail5:
#if WASM_ENABLE_THREAD_MGR != 0 && WASM_ENABLE_DEBUG_INTERP != 0
    wasm_cluster_destroy_exenv_status(exec_env->current_status);
#endif
#endif
#if WASM_ENABLE_THREAD_MGR != 0
#if WASM_ENABLE_DEBUG_INTERP != 0
fail4:
    os_cond_destroy(&exec_env->wait_cond);
#endif
fail3:
    os_mutex_destroy(&exec_env->wait_lock);
fail2:
#endif
#if WASM_ENABLE_AOT != 0
    wasm_runtime_free(exec_env->argv_buf);
fail1:
#endif
    wasm_runtime_free(exec_env);
    exec_env = NULL;

done:
    wasm_allocation_quota_scope_leave(&scope);
    return exec_env;
}

void
wasm_exec_env_destroy_internal(WASMExecEnv *exec_env)
{
    WASMAllocationQuotaScope scope;

    if (!exec_env
        || !wasm_allocation_quota_scope_enter_for_allocation(&scope,
                                                             exec_env)) {
        LOG_ERROR("Exec env destruction rejected by allocation owner");
        return;
    }
    if (!exec_env->module_inst
        || !wasm_allocation_quota_allocations_share_owner(
            exec_env, exec_env->module_inst)) {
        LOG_ERROR("Exec env and module instance allocation owners differ");
        wasm_allocation_quota_scope_leave(&scope);
        return;
    }

#if WASM_ENABLE_COMPONENT_MODEL != 0 && WASM_ENABLE_LIBC_WASI_P2 != 0 \
    && WASM_ENABLE_THREAD_MGR != 0
    wasi_p2_interrupt_wait_destroy(exec_env);
#endif
#ifdef OS_ENABLE_HW_BOUND_CHECK
    os_munmap(exec_env->exce_check_guard_page, os_getpagesize());
    wasm_allocation_quota_reservation_release(
        &exec_env->guard_page_reservation);
#endif
#if WASM_ENABLE_THREAD_MGR != 0
    os_mutex_destroy(&exec_env->wait_lock);
    os_cond_destroy(&exec_env->wait_cond);
#if WASM_ENABLE_DEBUG_INTERP != 0
    wasm_cluster_destroy_exenv_status(exec_env->current_status);
#endif
#endif
#if WASM_ENABLE_AOT != 0
    wasm_runtime_free(exec_env->argv_buf);
#endif
    /* Failure paths which never started a thread may still own this charge. */
    wasm_exec_env_release_native_thread_stack(exec_env);
    wasm_runtime_free(exec_env);
    wasm_allocation_quota_scope_leave(&scope);
}

WASMExecEnv *
wasm_exec_env_create(struct WASMModuleInstanceCommon *module_inst,
                     uint32 stack_size)
{
    WASMAllocationQuotaScope scope;
#if WASM_ENABLE_THREAD_MGR != 0
    WASMCluster *cluster;
#endif
    WASMExecEnv *exec_env = NULL;
#if WASM_ENABLE_GC != 0
    void *gc_heap_handle = NULL;
#endif

    if (!module_inst
        || !wasm_allocation_quota_scope_enter_for_allocation(&scope,
                                                             module_inst)) {
        return NULL;
    }

    exec_env = wasm_exec_env_create_internal(module_inst, stack_size);
    if (!exec_env)
        goto done;
    if (!wasm_allocation_quota_allocations_share_owner(module_inst, exec_env)) {
        wasm_exec_env_destroy_internal(exec_env);
        exec_env = NULL;
        goto done;
    }

#if WASM_ENABLE_INTERP != 0
    /* Set the aux_stack_boundary and aux_stack_bottom */
    if (module_inst->module_type == Wasm_Module_Bytecode) {
        WASMModule *module = ((WASMModuleInstance *)module_inst)->module;
        exec_env->aux_stack_bottom = (uintptr_t)module->aux_stack_bottom;
        exec_env->aux_stack_boundary =
            (uintptr_t)module->aux_stack_bottom - module->aux_stack_size;
#if WASM_ENABLE_GC != 0
        gc_heap_handle =
            ((WASMModuleInstance *)module_inst)->e->common.gc_heap_pool;
#endif
    }
#endif
#if WASM_ENABLE_AOT != 0
    /* Set the aux_stack_boundary and aux_stack_bottom */
    if (module_inst->module_type == Wasm_Module_AoT) {
        AOTModule *module =
            (AOTModule *)((AOTModuleInstance *)module_inst)->module;
        exec_env->aux_stack_bottom = (uintptr_t)module->aux_stack_bottom;
        exec_env->aux_stack_boundary =
            (uintptr_t)module->aux_stack_bottom - module->aux_stack_size;
#if WASM_ENABLE_GC != 0
        gc_heap_handle =
            ((AOTModuleInstanceExtra *)((AOTModuleInstance *)module_inst)->e)
                ->common.gc_heap_handle;
#endif
    }
#endif

#if WASM_ENABLE_THREAD_MGR != 0
    /* Create a new cluster for this exec_env */
    if (!(cluster = wasm_cluster_create(exec_env))) {
        wasm_exec_env_destroy_internal(exec_env);
        exec_env = NULL;
        goto done;
    }
#if WASM_ENABLE_GC != 0
    mem_allocator_enable_gc_reclaim(gc_heap_handle, cluster);
#endif
#else
#if WASM_ENABLE_GC != 0
    mem_allocator_enable_gc_reclaim(gc_heap_handle, exec_env);
#endif
#endif /* end of WASM_ENABLE_THREAD_MGR */

done:
    wasm_allocation_quota_scope_leave(&scope);
    return exec_env;
}

void
wasm_exec_env_destroy(WASMExecEnv *exec_env)
{
    WASMAllocationQuotaScope scope;

    if (!exec_env
        || !wasm_allocation_quota_scope_enter_for_allocation(&scope,
                                                             exec_env)) {
        LOG_ERROR("Exec env destruction rejected by allocation owner");
        return;
    }
    if (!exec_env->module_inst
        || !wasm_allocation_quota_allocations_share_owner(
            exec_env, exec_env->module_inst)) {
        LOG_ERROR("Exec env and module instance allocation owners differ");
        wasm_allocation_quota_scope_leave(&scope);
        return;
    }

#if WASM_ENABLE_THREAD_MGR != 0
    /* Wait for all sub-threads */
    WASMCluster *cluster = wasm_exec_env_get_cluster(exec_env);
    if (cluster) {
        wasm_cluster_wait_for_all_except_self(cluster, exec_env);
#if WASM_ENABLE_DEBUG_INTERP != 0
        /* Must fire exit event after other threads exits, otherwise
           the stopped thread will be overridden by other threads */
        wasm_cluster_thread_exited(exec_env);
#endif
        /* We have waited for other threads, this is the only alive thread, so
         * we don't acquire cluster->lock because the cluster will be destroyed
         * inside this function */
        wasm_cluster_del_exec_env(cluster, exec_env);
    }
#endif /* end of WASM_ENABLE_THREAD_MGR */

    wasm_exec_env_destroy_internal(exec_env);
    wasm_allocation_quota_scope_leave(&scope);
}

bool
wasm_exec_env_reserve_native_thread_stack(WASMExecEnv *exec_env,
                                          uint32 stack_size)
{
    if (!exec_env || stack_size == 0
        || exec_env->native_thread_stack_reservation.active) {
        return false;
    }

    return wasm_allocation_quota_reservation_acquire_for_allocation(
        &exec_env->native_thread_stack_reservation, exec_env,
        wasm_exec_env_native_thread_stack_reservation_size(stack_size),
        wasm_exec_env_native_thread_stack_reservation_count());
}

uint64
wasm_exec_env_native_thread_stack_reservation_size(uint32 stack_size)
{
    uint64 size = stack_size;

    /* POSIX HW-bound threads map this alternate signal stack in
       os_thread_signal_init() before entering the runtime start routine. */
#if defined(OS_ENABLE_HW_BOUND_CHECK) \
    && WASM_DISABLE_STACK_HW_BOUND_CHECK == 0 && !defined(BH_PLATFORM_WINDOWS)
    size += OS_THREAD_SIGNAL_ALT_STACK_SIZE;
#endif
    return size;
}

uint32
wasm_exec_env_native_thread_stack_reservation_count(void)
{
#if defined(OS_ENABLE_HW_BOUND_CHECK) \
    && WASM_DISABLE_STACK_HW_BOUND_CHECK == 0 && !defined(BH_PLATFORM_WINDOWS)
    return 2;
#else
    return 1;
#endif
}

void
wasm_exec_env_release_native_thread_stack(WASMExecEnv *exec_env)
{
    if (exec_env) {
        wasm_allocation_quota_reservation_release(
            &exec_env->native_thread_stack_reservation);
    }
}

void
wasm_exec_env_take_native_thread_stack(
    WASMExecEnv *exec_env, WASMAllocationQuotaReservation *reservation)
{
    if (exec_env && reservation) {
        wasm_allocation_quota_reservation_move(
            reservation, &exec_env->native_thread_stack_reservation);
    }
}

WASMModuleInstanceCommon *
wasm_exec_env_get_module_inst(WASMExecEnv *exec_env)
{
    return exec_env->module_inst;
}

void
wasm_exec_env_set_module_inst(WASMExecEnv *exec_env,
                              WASMModuleInstanceCommon *const module_inst)
{
#if WASM_ENABLE_THREAD_MGR != 0
    wasm_cluster_traverse_lock(exec_env);
#endif
    exec_env->module_inst = module_inst;
#if WASM_ENABLE_THREAD_MGR != 0
    wasm_cluster_traverse_unlock(exec_env);
#endif
}

void
wasm_exec_env_restore_module_inst(
    WASMExecEnv *exec_env, WASMModuleInstanceCommon *const module_inst_common)
{
    WASMModuleInstanceCommon *old_module_inst_common = exec_env->module_inst;
    WASMModuleInstance *old_module_inst =
        (WASMModuleInstance *)old_module_inst_common;
    WASMModuleInstance *module_inst = (WASMModuleInstance *)module_inst_common;
    char cur_exception[EXCEPTION_BUF_LEN];

#if WASM_ENABLE_THREAD_MGR != 0
    wasm_cluster_traverse_lock(exec_env);
#endif
    exec_env->module_inst = module_inst_common;
    /*
     * propagate an exception if any.
     */
    exception_lock(old_module_inst);
    if (old_module_inst->cur_exception[0] != '\0') {
        bh_memcpy_s(cur_exception, sizeof(cur_exception),
                    old_module_inst->cur_exception,
                    sizeof(old_module_inst->cur_exception));
    }
    else {
        cur_exception[0] = '\0';
    }
    exception_unlock(old_module_inst);
#if WASM_ENABLE_THREAD_MGR != 0
    wasm_cluster_traverse_unlock(exec_env);
#endif
    if (cur_exception[0] != '\0') {
        exception_lock(module_inst);
        bh_memcpy_s(module_inst->cur_exception,
                    sizeof(module_inst->cur_exception), cur_exception,
                    sizeof(cur_exception));
        exception_unlock(module_inst);
    }
}

void
wasm_exec_env_set_thread_info(WASMExecEnv *exec_env)
{
#if WASM_ENABLE_THREAD_MGR != 0
    os_mutex_lock(&exec_env->wait_lock);
#endif
    exec_env->handle = os_self_thread();
    if (exec_env->user_native_stack_boundary)
        /* WASM_STACK_GUARD_SIZE isn't added for flexibility to developer,
           he must ensure that enough guard bytes are kept. */
        exec_env->native_stack_boundary = exec_env->user_native_stack_boundary;
    else {
        uint8 *stack_boundary = os_thread_get_stack_boundary();
        exec_env->native_stack_boundary =
            stack_boundary ? stack_boundary + WASM_STACK_GUARD_SIZE : NULL;
    }
    exec_env->native_stack_top_min = (void *)UINTPTR_MAX;
#if WASM_ENABLE_THREAD_MGR != 0
    os_mutex_unlock(&exec_env->wait_lock);
#endif
}

#if WASM_ENABLE_THREAD_MGR != 0
void *
wasm_exec_env_get_thread_arg(WASMExecEnv *exec_env)
{
    return exec_env->thread_arg;
}

void
wasm_exec_env_set_thread_arg(WASMExecEnv *exec_env, void *thread_arg)
{
    exec_env->thread_arg = thread_arg;
}
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK
void
wasm_exec_env_push_jmpbuf(WASMExecEnv *exec_env, WASMJmpBuf *jmpbuf)
{
    jmpbuf->prev = exec_env->jmpbuf_stack_top;
    exec_env->jmpbuf_stack_top = jmpbuf;
}

WASMJmpBuf *
wasm_exec_env_pop_jmpbuf(WASMExecEnv *exec_env)
{
    WASMJmpBuf *stack_top = exec_env->jmpbuf_stack_top;

    if (stack_top) {
        exec_env->jmpbuf_stack_top = stack_top->prev;
        return stack_top;
    }

    return NULL;
}
#endif
