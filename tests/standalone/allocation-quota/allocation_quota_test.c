/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_allocation_quota.h"
#include "wasm_export.h"

bool
wasm_runtime_memory_init(mem_alloc_type_t mem_alloc_type,
                         const MemAllocOption *alloc_option);

void
wasm_runtime_memory_destroy(void);

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "allocation quota check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition);                        \
            abort();                                                        \
        }                                                                   \
    } while (0)

typedef struct AllocatorState {
    pthread_mutex_t lock;
    uint32_t malloc_calls;
    uint32_t realloc_calls;
    uint32_t free_calls;
    uint32_t last_malloc_size;
    uint32_t last_realloc_size;
    bool fail_next_malloc;
    bool fail_next_realloc;
    bool user_data_alive;
} AllocatorState;

typedef struct QuotaState {
    pthread_mutex_t lock;
    uint64_t live_bytes;
    uint64_t reserved_bytes;
    uint64_t released_bytes;
    uint64_t byte_limit;
    uint32_t live_allocations;
    uint32_t reserved_allocations;
    uint32_t released_allocations;
    uint32_t allocation_limit;
    uint32_t reserve_calls;
    uint32_t release_calls;
    uint32_t denied_calls;
    uint32_t attachment_retain_calls;
    uint32_t attachment_release_calls;
    bool allow_attachment_retain;
    AllocatorState *allocator_user_data_to_release;
} QuotaState;

static AllocatorState allocator_state;

static void *
test_malloc(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    unsigned int size)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    AllocatorState *state = &allocator_state;
#endif
    bool fail;
    void *ptr;

    CHECK(state != NULL);
    pthread_mutex_lock(&state->lock);
    CHECK(state->user_data_alive);
    state->malloc_calls++;
    state->last_malloc_size = size;
    fail = state->fail_next_malloc;
    state->fail_next_malloc = false;
    pthread_mutex_unlock(&state->lock);
    if (fail)
        return NULL;
    ptr = malloc(size);
    return ptr;
}

static void *
test_realloc(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    void *ptr, unsigned int size)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    AllocatorState *state = &allocator_state;
#endif
    bool fail;

    CHECK(state != NULL);
    pthread_mutex_lock(&state->lock);
    CHECK(state->user_data_alive);
    state->realloc_calls++;
    state->last_realloc_size = size;
    fail = state->fail_next_realloc;
    state->fail_next_realloc = false;
    pthread_mutex_unlock(&state->lock);
    if (fail)
        return NULL;
    return realloc(ptr, size);
}

static void
test_free(
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void *user_data,
#endif
    void *ptr)
{
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    AllocatorState *state = user_data;
#else
    AllocatorState *state = &allocator_state;
#endif

    CHECK(state != NULL);
    pthread_mutex_lock(&state->lock);
    CHECK(state->user_data_alive);
    state->free_calls++;
    pthread_mutex_unlock(&state->lock);
    free(ptr);
}

static uint32_t
allocator_malloc_calls(void)
{
    uint32_t calls;

    pthread_mutex_lock(&allocator_state.lock);
    calls = allocator_state.malloc_calls;
    pthread_mutex_unlock(&allocator_state.lock);
    return calls;
}

static uint32_t
allocator_realloc_calls(void)
{
    uint32_t calls;

    pthread_mutex_lock(&allocator_state.lock);
    calls = allocator_state.realloc_calls;
    pthread_mutex_unlock(&allocator_state.lock);
    return calls;
}

static uint32_t
allocator_free_calls(void)
{
    uint32_t calls;

    pthread_mutex_lock(&allocator_state.lock);
    calls = allocator_state.free_calls;
    pthread_mutex_unlock(&allocator_state.lock);
    return calls;
}

static uint32_t
allocator_last_malloc_size(void)
{
    uint32_t size;

    pthread_mutex_lock(&allocator_state.lock);
    size = allocator_state.last_malloc_size;
    pthread_mutex_unlock(&allocator_state.lock);
    return size;
}

static uint32_t
allocator_last_realloc_size(void)
{
    uint32_t size;

    pthread_mutex_lock(&allocator_state.lock);
    size = allocator_state.last_realloc_size;
    pthread_mutex_unlock(&allocator_state.lock);
    return size;
}

#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
static bool
allocator_user_data_alive(void)
{
    bool alive;

    pthread_mutex_lock(&allocator_state.lock);
    alive = allocator_state.user_data_alive;
    pthread_mutex_unlock(&allocator_state.lock);
    return alive;
}
#endif

static void
allocator_fail_next_realloc(void)
{
    pthread_mutex_lock(&allocator_state.lock);
    allocator_state.fail_next_realloc = true;
    pthread_mutex_unlock(&allocator_state.lock);
}

static void
allocator_fail_next_malloc(void)
{
    pthread_mutex_lock(&allocator_state.lock);
    allocator_state.fail_next_malloc = true;
    pthread_mutex_unlock(&allocator_state.lock);
}

static void
quota_init(QuotaState *quota)
{
    memset(quota, 0, sizeof(*quota));
    CHECK(pthread_mutex_init(&quota->lock, NULL) == 0);
    quota->byte_limit = UINT64_MAX;
    quota->allocation_limit = UINT32_MAX;
    quota->allow_attachment_retain = true;
}

static void
quota_destroy(QuotaState *quota)
{
    CHECK(quota->live_bytes == 0);
    CHECK(quota->live_allocations == 0);
    CHECK(pthread_mutex_destroy(&quota->lock) == 0);
}

static bool
quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;
    bool allowed;

    pthread_mutex_lock(&quota->lock);
    quota->reserve_calls++;
    allowed =
        bytes <= quota->byte_limit - quota->live_bytes
        && allocations <= quota->allocation_limit - quota->live_allocations;
    if (allowed) {
        quota->live_bytes += bytes;
        quota->live_allocations += allocations;
        quota->reserved_bytes += bytes;
        quota->reserved_allocations += allocations;
    }
    else {
        quota->denied_calls++;
    }
    pthread_mutex_unlock(&quota->lock);
    return allowed;
}

static void
quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    QuotaState *quota = attachment;

    pthread_mutex_lock(&quota->lock);
    CHECK(bytes <= quota->live_bytes);
    CHECK(allocations <= quota->live_allocations);
    quota->live_bytes -= bytes;
    quota->live_allocations -= allocations;
    quota->released_bytes += bytes;
    quota->released_allocations += allocations;
    quota->release_calls++;
    pthread_mutex_unlock(&quota->lock);
}

static bool
quota_attachment_retain(void *attachment)
{
    QuotaState *quota = attachment;
    bool retained;

    pthread_mutex_lock(&quota->lock);
    quota->attachment_retain_calls++;
    retained = quota->allow_attachment_retain;
    pthread_mutex_unlock(&quota->lock);
    return retained;
}

static void
quota_attachment_release(void *attachment)
{
    QuotaState *quota = attachment;
    AllocatorState *allocator_user_data;

    pthread_mutex_lock(&quota->lock);
    quota->attachment_release_calls++;
    allocator_user_data = quota->allocator_user_data_to_release;
    pthread_mutex_unlock(&quota->lock);

    if (allocator_user_data) {
        pthread_mutex_lock(&allocator_user_data->lock);
        CHECK(allocator_user_data->user_data_alive);
        allocator_user_data->user_data_alive = false;
        pthread_mutex_unlock(&allocator_user_data->lock);
    }
}

static LoadArgs2
quota_load_args(QuotaState *quota)
{
    LoadArgs2 args;

    wasm_runtime_load_args2_init(&args);
    wasm_runtime_load_args2_set_allocation_quota(
        &args, quota, quota_reserve, quota_release, quota_attachment_retain,
        quota_attachment_release);
    return args;
}

static uint32_t
raw_size_for(uint32_t user_size, uint32_t alignment)
{
    uint32_t raw_size = 0;

    CHECK(
        wasm_allocation_header_calculate_size(user_size, alignment, &raw_size));
    CHECK(raw_size % alignment == 0);
    CHECK(raw_size >= sizeof(WASMAllocationHeader) + user_size);
    CHECK(raw_size - sizeof(WASMAllocationHeader) - user_size
          < 2 * (uint64_t)alignment);
    return raw_size;
}

static void
test_unowned_common_allocations(void)
{
    uint8_t *ptr;
    uint8_t *zeroed;
    uint32_t index;

    CHECK(wasm_allocation_quota_get_current() == NULL);
    ptr = wasm_runtime_malloc(31);
    CHECK(ptr != NULL);
    CHECK((uintptr_t)ptr % WASM_ALLOCATION_MAX_ALIGNMENT == 0);
    for (index = 0; index < 31; index++)
        ptr[index] = (uint8_t)(index + 1);
    ptr = wasm_runtime_realloc(ptr, 257);
    CHECK(ptr != NULL);
    for (index = 0; index < 31; index++)
        CHECK(ptr[index] == (uint8_t)(index + 1));

    zeroed = wasm_runtime_calloc(17, 3);
    CHECK(zeroed != NULL);
    for (index = 0; index < 51; index++)
        CHECK(zeroed[index] == 0);
    wasm_runtime_free(zeroed);
    wasm_runtime_free(ptr);
}

static void
test_token_retain_failure_is_inert(void)
{
    QuotaState quota;
    LoadArgs2 args;
    uint32_t malloc_calls;
    uint32_t free_calls;

    quota_init(&quota);
    quota.allow_attachment_retain = false;
    args = quota_load_args(&quota);
    malloc_calls = allocator_malloc_calls();
    free_calls = allocator_free_calls();
    CHECK(wasm_allocation_quota_token_create(&args) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls);
    CHECK(allocator_free_calls() == free_calls);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 0);
    CHECK(quota.reserve_calls == 0);
    quota_destroy(&quota);
}

static void
test_token_storage_reservation_failures_are_refunded(void)
{
    QuotaState quota;
    LoadArgs2 args;
    uint32_t malloc_calls;
    uint32_t free_calls;

    quota_init(&quota);
    quota.allocation_limit = 0;
    args = quota_load_args(&quota);
    malloc_calls = allocator_malloc_calls();
    CHECK(wasm_allocation_quota_token_create(&args) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls);
    CHECK(quota.denied_calls == 1);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);

    quota_init(&quota);
    args = quota_load_args(&quota);
    malloc_calls = allocator_malloc_calls();
    free_calls = allocator_free_calls();
    allocator_fail_next_malloc();
    CHECK(wasm_allocation_quota_token_create(&args) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls + 1);
    CHECK(allocator_free_calls() == free_calls);
    CHECK(quota.reserved_allocations == 1);
    CHECK(quota.released_allocations == 1);
    CHECK(quota.reserved_bytes == quota.released_bytes);
    CHECK(quota.attachment_retain_calls == 1);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);
}

static void
test_nested_scope_calloc_and_overflow(void)
{
    QuotaState first;
    QuotaState second;
    LoadArgs2 first_args;
    LoadArgs2 second_args;
    WASMAllocationQuotaToken *first_token;
    WASMAllocationQuotaToken *second_token;
    WASMAllocationQuotaToken *previous;
    uint8_t *first_ptr;
    uint8_t *second_ptr;
    uint8_t *null_realloc_ptr;
    uint32_t first_raw = raw_size_for(21, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t second_raw = raw_size_for(13, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t null_realloc_raw = raw_size_for(5, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t malloc_calls;
    uint32_t free_calls;
    uint32_t reserve_calls;
    uint64_t released_bytes;
    uint64_t first_token_bytes;
    uint64_t second_token_bytes;
    uint32_t index;

    quota_init(&first);
    quota_init(&second);
    first_args = quota_load_args(&first);
    second_args = quota_load_args(&second);
    first_token = wasm_allocation_quota_token_create(&first_args);
    second_token = wasm_allocation_quota_token_create(&second_args);
    CHECK(first_token != NULL);
    CHECK(second_token != NULL);
    first_token_bytes = first.live_bytes;
    second_token_bytes = second.live_bytes;
    CHECK(first_token_bytes > 0);
    CHECK(first_token_bytes % WASM_ALLOCATION_MAX_ALIGNMENT == 0);
    CHECK(second_token_bytes == first_token_bytes);
    CHECK(first.live_allocations == 1);
    CHECK(second.live_allocations == 1);
    CHECK(first.attachment_retain_calls == 1);
    CHECK(second.attachment_retain_calls == 1);

    CHECK(wasm_allocation_quota_get_current() == NULL);
    previous = wasm_allocation_quota_set_current(first_token);
    CHECK(previous == NULL);
    previous = wasm_allocation_quota_set_current(second_token);
    CHECK(previous == first_token);

    second_ptr = wasm_runtime_malloc(13);
    CHECK(second_ptr != NULL);
    CHECK(wasm_allocation_quota_allocation_matches_current(second_ptr));
    CHECK((uintptr_t)second_ptr % WASM_ALLOCATION_MAX_ALIGNMENT == 0);
    CHECK(second.live_bytes == second_token_bytes + second_raw);
    CHECK(second.live_allocations == 2);

    CHECK(wasm_allocation_quota_set_current(previous) == second_token);
    CHECK(!wasm_allocation_quota_allocation_matches_current(second_ptr));
    first_ptr = wasm_runtime_calloc(3, 7);
    CHECK(first_ptr != NULL);
    CHECK(wasm_allocation_quota_allocation_matches_current(first_ptr));
    CHECK(
        !wasm_allocation_quota_allocations_share_owner(first_ptr, second_ptr));
    for (index = 0; index < 21; index++)
        CHECK(first_ptr[index] == 0);
    CHECK(first.live_bytes == first_token_bytes + first_raw);
    CHECK(first.live_allocations == 2);

    first.byte_limit = first.live_bytes;
    malloc_calls = allocator_malloc_calls();
    CHECK(wasm_runtime_malloc(33) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls);
    CHECK(first.denied_calls == 1);
    first.byte_limit = UINT64_MAX;

    first.allocation_limit = first.live_allocations;
    CHECK(wasm_runtime_malloc(33) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls);
    CHECK(first.denied_calls == 2);
    first.allocation_limit = UINT32_MAX;

    allocator_fail_next_malloc();
    released_bytes = first.released_bytes;
    CHECK(wasm_runtime_malloc(33) == NULL);
    CHECK(allocator_last_malloc_size()
          == raw_size_for(33, WASM_ALLOCATION_MAX_ALIGNMENT));
    CHECK(first.live_bytes == first_token_bytes + first_raw);
    CHECK(first.live_allocations == 2);
    CHECK(first.released_bytes
          == released_bytes + raw_size_for(33, WASM_ALLOCATION_MAX_ALIGNMENT));

    malloc_calls = allocator_malloc_calls();
    reserve_calls = first.reserve_calls;
    CHECK(wasm_runtime_calloc(UINT64_MAX, 2) == NULL);
    CHECK(wasm_runtime_malloc(UINT32_MAX) == NULL);
    CHECK(allocator_malloc_calls() == malloc_calls);
    CHECK(first.reserve_calls == reserve_calls);
    CHECK(!wasm_allocation_header_calculate_size(
        UINT32_MAX, WASM_ALLOCATION_MAX_ALIGNMENT, &malloc_calls));

    null_realloc_ptr = wasm_runtime_realloc(NULL, 5);
    CHECK(null_realloc_ptr != NULL);
    CHECK(wasm_allocation_quota_allocations_share_owner(first_ptr,
                                                        null_realloc_ptr));
    CHECK(first.live_bytes == first_token_bytes + first_raw + null_realloc_raw);
    CHECK(first.live_allocations == 3);

    CHECK(wasm_allocation_quota_set_current(NULL) == first_token);
    CHECK(!wasm_allocation_quota_allocation_matches_current(first_ptr));
    free_calls = allocator_free_calls();
    wasm_runtime_free(NULL);
    CHECK(allocator_free_calls() == free_calls);

    wasm_runtime_free(second_ptr);
    wasm_runtime_free(first_ptr);
    CHECK(wasm_runtime_realloc(null_realloc_ptr, 0) == NULL);
    CHECK(first.live_bytes == first_token_bytes);
    CHECK(first.live_allocations == 1);
    CHECK(second.live_bytes == second_token_bytes);
    CHECK(second.live_allocations == 1);

    wasm_allocation_quota_token_release(second_token);
    wasm_allocation_quota_token_release(first_token);
    CHECK(first.live_bytes == 0);
    CHECK(first.live_allocations == 0);
    CHECK(second.live_bytes == 0);
    CHECK(second.live_allocations == 0);
    CHECK(first.attachment_release_calls == 1);
    CHECK(second.attachment_release_calls == 1);
    quota_destroy(&second);
    quota_destroy(&first);
}

typedef struct CrossThreadFreeArgs {
    void *ptr;
} CrossThreadFreeArgs;

static void *
cross_thread_free(void *arg)
{
    CrossThreadFreeArgs *args = arg;

    CHECK(wasm_allocation_quota_get_current() == NULL);
    wasm_runtime_free(args->ptr);
    return NULL;
}

static void
test_realloc_accounting_and_cross_thread_free(void)
{
    QuotaState quota;
    LoadArgs2 args;
    WASMAllocationQuotaToken *token;
    WASMAllocationQuotaToken *previous;
    CrossThreadFreeArgs free_args;
    pthread_t free_thread;
    uint8_t *ptr;
    uint8_t *new_ptr;
    uint32_t raw_17 = raw_size_for(17, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t raw_100 = raw_size_for(100, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t raw_500 = raw_size_for(500, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t raw_9 = raw_size_for(9, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t realloc_calls;
    uint64_t released_bytes;
    uint64_t token_bytes;
    uint32_t index;

    quota_init(&quota);
    args = quota_load_args(&quota);
    token = wasm_allocation_quota_token_create(&args);
    CHECK(token != NULL);
    token_bytes = quota.live_bytes;
    CHECK(quota.live_allocations == 1);
    previous = wasm_allocation_quota_set_current(token);
    CHECK(previous == NULL);

    ptr = wasm_runtime_malloc(17);
    CHECK(ptr != NULL);
    CHECK(allocator_last_malloc_size() == raw_17);
    for (index = 0; index < 17; index++)
        ptr[index] = (uint8_t)(index + 1);
    CHECK(quota.live_bytes == token_bytes + raw_17);
    CHECK(quota.live_allocations == 2);
    CHECK(wasm_allocation_quota_set_current(previous) == token);

    new_ptr = wasm_runtime_realloc(ptr, 100);
    CHECK(new_ptr != NULL);
    CHECK(allocator_last_realloc_size() == raw_100);
    ptr = new_ptr;
    CHECK(quota.live_bytes == token_bytes + raw_100);
    CHECK(quota.live_allocations == 2);
    for (index = 0; index < 17; index++)
        CHECK(ptr[index] == (uint8_t)(index + 1));

    released_bytes = quota.released_bytes;
    allocator_fail_next_realloc();
    CHECK(wasm_runtime_realloc(ptr, 500) == NULL);
    CHECK(allocator_last_realloc_size() == raw_500);
    CHECK(quota.live_bytes == token_bytes + raw_100);
    CHECK(quota.released_bytes
          == released_bytes + (uint64_t)(raw_500 - raw_100));
    for (index = 0; index < 17; index++)
        CHECK(ptr[index] == (uint8_t)(index + 1));

    quota.byte_limit = quota.live_bytes;
    realloc_calls = allocator_realloc_calls();
    CHECK(wasm_runtime_realloc(ptr, 500) == NULL);
    CHECK(allocator_realloc_calls() == realloc_calls);
    CHECK(quota.denied_calls == 1);
    CHECK(quota.live_bytes == token_bytes + raw_100);
    quota.byte_limit = UINT64_MAX;

    released_bytes = quota.released_bytes;
    allocator_fail_next_realloc();
    CHECK(wasm_runtime_realloc(ptr, 9) == NULL);
    CHECK(allocator_last_realloc_size() == raw_9);
    CHECK(quota.live_bytes == token_bytes + raw_100);
    CHECK(quota.released_bytes == released_bytes);
    for (index = 0; index < 17; index++)
        CHECK(ptr[index] == (uint8_t)(index + 1));

    new_ptr = wasm_runtime_realloc(ptr, 9);
    CHECK(new_ptr != NULL);
    CHECK(allocator_last_realloc_size() == raw_9);
    ptr = new_ptr;
    CHECK(quota.live_bytes == token_bytes + raw_9);
    CHECK(quota.live_allocations == 2);
    for (index = 0; index < 9; index++)
        CHECK(ptr[index] == (uint8_t)(index + 1));

    wasm_allocation_quota_token_release(token);
    CHECK(quota.attachment_release_calls == 0);
    free_args.ptr = ptr;
    CHECK(pthread_create(&free_thread, NULL, cross_thread_free, &free_args)
          == 0);
    CHECK(pthread_join(free_thread, NULL) == 0);
    CHECK(quota.live_bytes == 0);
    CHECK(quota.live_allocations == 0);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);
}

static void
test_pool_aligned_allocation_keeps_ownership_metadata(void)
{
    const uint32_t alignment = 64;
    const uint32_t user_size = 128;
    uint8_t *heap;
    MemAllocOption option;
    QuotaState quota;
    LoadArgs2 args;
    WASMAllocationQuotaToken *token;
    void *ptr;
    void *small_alignment_ptr;
    uint8_t *common_ptr;
    uint32_t raw_size = raw_size_for(user_size, alignment);
    uint32_t small_alignment_raw_size =
        raw_size_for(24, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint32_t common_raw_size = raw_size_for(257, WASM_ALLOCATION_MAX_ALIGNMENT);
    uint64_t token_bytes;
    uint32_t index;

    heap = malloc(256 * 1024);
    CHECK(heap != NULL);
    memset(&option, 0, sizeof(option));
    option.pool.heap_buf = heap;
    option.pool.heap_size = 256 * 1024;
    CHECK(wasm_runtime_memory_init(Alloc_With_Pool, &option));

    quota_init(&quota);
    args = quota_load_args(&quota);
    token = wasm_allocation_quota_token_create(&args);
    CHECK(token != NULL);
    token_bytes = quota.live_bytes;
    CHECK(quota.live_allocations == 1);
    CHECK(wasm_allocation_quota_set_current(token) == NULL);
    ptr = wasm_runtime_aligned_alloc(user_size, alignment);
    CHECK(ptr != NULL);
    CHECK((uintptr_t)ptr % alignment == 0);
    CHECK(quota.live_bytes == token_bytes + raw_size);
    CHECK(quota.live_allocations == 2);
    CHECK(wasm_runtime_realloc(ptr, user_size * 2) == NULL);
    CHECK(quota.live_bytes == token_bytes + raw_size);

    common_ptr = wasm_runtime_malloc(17);
    CHECK(common_ptr != NULL);
    for (index = 0; index < 17; index++)
        common_ptr[index] = (uint8_t)(index + 1);
    common_ptr = wasm_runtime_realloc(common_ptr, 257);
    CHECK(common_ptr != NULL);
    for (index = 0; index < 17; index++)
        CHECK(common_ptr[index] == (uint8_t)(index + 1));
    small_alignment_ptr = wasm_runtime_aligned_alloc(24, 8);
    CHECK(small_alignment_ptr != NULL);
    CHECK((uintptr_t)small_alignment_ptr % WASM_ALLOCATION_MAX_ALIGNMENT == 0);
    CHECK(quota.live_bytes
          == token_bytes + raw_size + common_raw_size
                 + small_alignment_raw_size);
    CHECK(quota.live_allocations == 4);
    CHECK(wasm_allocation_quota_set_current(NULL) == token);

    wasm_allocation_quota_token_release(token);
    CHECK(quota.attachment_release_calls == 0);
    wasm_runtime_free(ptr);
    CHECK(quota.attachment_release_calls == 0);
    wasm_runtime_free(small_alignment_ptr);
    CHECK(quota.attachment_release_calls == 0);
    wasm_runtime_free(common_ptr);
    CHECK(quota.live_bytes == 0);
    CHECK(quota.live_allocations == 0);
    CHECK(quota.attachment_release_calls == 1);
    quota_destroy(&quota);

    wasm_runtime_memory_destroy();
    free(heap);
}

static void
test_system_allocator_mode(void)
{
    MemAllocOption option;

    memset(&option, 0, sizeof(option));
    CHECK(wasm_runtime_memory_init(Alloc_With_System_Allocator, &option));
    test_unowned_common_allocations();
    wasm_runtime_memory_destroy();
}

#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
static void
test_token_storage_is_freed_before_allocator_user_data_release(void)
{
    QuotaState quota;
    LoadArgs2 args;
    WASMAllocationQuotaToken *token;
    uint32_t free_calls;

    quota_init(&quota);
    quota.allocator_user_data_to_release = &allocator_state;
    args = quota_load_args(&quota);
    token = wasm_allocation_quota_token_create(&args);
    CHECK(token != NULL);
    free_calls = allocator_free_calls();

    wasm_allocation_quota_token_release(token);

    CHECK(allocator_free_calls() == free_calls + 1);
    CHECK(quota.attachment_release_calls == 1);
    CHECK(!allocator_user_data_alive());
    quota_destroy(&quota);
}
#endif

int
main(void)
{
    MemAllocOption option;

    memset(&allocator_state, 0, sizeof(allocator_state));
    CHECK(pthread_mutex_init(&allocator_state.lock, NULL) == 0);
    allocator_state.user_data_alive = true;
    memset(&option, 0, sizeof(option));
    option.allocator.malloc_func = (void *)test_malloc;
    option.allocator.realloc_func = (void *)test_realloc;
    option.allocator.free_func = (void *)test_free;
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    option.allocator.user_data = &allocator_state;
#endif
    CHECK(wasm_runtime_memory_init(Alloc_With_Allocator, &option));

    test_unowned_common_allocations();
    test_token_retain_failure_is_inert();
    test_token_storage_reservation_failures_are_refunded();
    test_nested_scope_calloc_and_overflow();
    test_realloc_accounting_and_cross_thread_free();
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    test_token_storage_is_freed_before_allocator_user_data_release();
#endif
    CHECK(wasm_allocation_quota_get_current() == NULL);
    wasm_runtime_memory_destroy();

    test_system_allocator_mode();
    test_pool_aligned_allocation_keeps_ownership_metadata();
    CHECK(pthread_mutex_destroy(&allocator_state.lock) == 0);
    puts("allocation quota allocator-mode checks passed");
    return 0;
}
