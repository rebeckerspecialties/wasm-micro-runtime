/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "component-model/wasm_component_host_resource.h"
#include "bh_platform.h"
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
#include <string.h>

HostResourceTable *g_host_resource_table = NULL;

// ID encoding:
// Upper 8 bits: HostResourceType
// Lower 24 bits: Counter value

#define HOST_RESOURCE_TYPE_SHIFT 24
#define HOST_RESOURCE_ID_MASK 0x00FFFFFF
#define HOST_RESOURCE_TABLE_SIZE 256

/* Convert an integer to a void* hash map key without triggering
   performance-no-int-to-ptr. The intermediate uintptr_t cast is
   required by the C standard. */
#define INT_TO_PTR(i) \
    ((void *)(uintptr_t)(i)) /* NOLINT(performance-no-int-to-ptr) */

// Counter starts at 1
static uint32_t next_id_counter = 1;
/* Number of representations currently registered in the global table.  It
 * bounds collision probing when the 24-bit counter wraps: with N live IDs,
 * at most N + 1 consecutive candidates can be occupied for one resource
 * type.  Host-resource representations are internal table keys; guest-visible
 * resource handles carry their own component-table lifetime and are never raw
 * representation IDs. */
static uint32_t host_resource_live_count = 0;
/* Table initialization/destruction require a quiescent runtime. This lock
 * protects ID allocation while component instances are running. */
static korp_mutex host_resource_id_lock;
static bool host_resource_id_lock_inited = false;

/*
 * The HashMap type intentionally hides its lock, so lifecycle operations use
 * one external lock and create the map without an internal lock.  This makes
 * find-and-pin and find-and-remove atomic with respect to each other.  The
 * lock remains initialized after table teardown because a pin acquired before
 * teardown may be the last reference released afterwards.
 */
static korp_mutex host_resource_table_lock;
static bool host_resource_table_lock_inited = false;

typedef struct HostResourcePinState {
    const HostResourcePinScope *token_address;
    const void *unwind_target;
    HostResource *resources[HOST_RESOURCE_PIN_SCOPE_CAPACITY];
    uint64_t cookie;
    uint32_t count;
} HostResourcePinState;

typedef struct HostResourcePinContext {
    HostResourcePinState states[HOST_RESOURCE_PIN_SCOPE_MAX_DEPTH];
    uint64_t next_cookie;
    uint32_t depth;
} HostResourcePinContext;

#if defined(os_thread_local_attribute)
static os_thread_local_attribute HostResourcePinContext
    current_host_resource_pin_context;
#elif defined(_MSC_VER)
static __declspec(thread)
    HostResourcePinContext current_host_resource_pin_context;
#elif defined(__GNUC__) || defined(__clang__)
static __thread HostResourcePinContext current_host_resource_pin_context;
#else
static _Thread_local HostResourcePinContext current_host_resource_pin_context;
#endif

typedef struct HostResourceCloseContext {
    HostResource *destroy_list;
} HostResourceCloseContext;

// Hash function for uint32_t keys
static uint32
hash_u32_hash_map(const void *key)
{
    return (uint32)(uintptr_t)key;
}

// Key equality function for uint32_t keys
static bool
u32_equal_hash_map(void *key1, void *key2)
{
    return (uint32_t)(uintptr_t)key1 == (uint32_t)(uintptr_t)key2;
}

// Key destroy function
static void
destroy_u32_key_hash_map(void *key)
{
    (void)key;
}

static HostResourceType
host_resource_type_from_id(uint32_t id)
{
    return (HostResourceType)((id >> HOST_RESOURCE_TYPE_SHIFT)
                              & UINT32_C(0xFF));
}

static void
mark_host_resource_closing(void *key, void *value, void *user_data)
{
    HostResource *hr = (HostResource *)value;
    HostResourceCloseContext *context = (HostResourceCloseContext *)user_data;

    (void)key;
    if (!hr || !context)
        return;

    hr->registered = false;
    hr->closing = true;
    if (hr->pin_count == 0) {
        hr->deferred_next = context->destroy_list;
        context->destroy_list = hr;
    }
}

static void
destroy_host_resource_list(HostResource *hr)
{
    while (hr) {
        HostResource *next = hr->deferred_next;

        hr->deferred_next = NULL;
        destroy_host_resource(hr);
        hr = next;
    }
}

static void
host_resource_release(HostResource *hr)
{
    bool destroy = false;

    if (!hr || !host_resource_table_lock_inited)
        return;

    os_mutex_lock(&host_resource_table_lock);
    if (hr->pin_count == 0) {
        LOG_ERROR("Host resource release observed an unpinned resource.\n");
    }
    else {
        hr->pin_count--;
        destroy = hr->closing && hr->pin_count == 0;
    }
    os_mutex_unlock(&host_resource_table_lock);

    if (destroy)
        destroy_host_resource(hr);
}

bool
instantiate_host_resource_table()
{
    /* The global map is owner-neutral. Its per-entry allocator observes the
       active owner when a guest inserts an entry. */
    if (wasm_allocation_quota_get_current() != NULL) {
        LOG_ERROR("Host resource table initialization requires no allocation "
                  "owner.\n");
        return false;
    }

    if (!host_resource_table_lock_inited) {
        if (os_mutex_init(&host_resource_table_lock) != BHT_OK) {
            LOG_ERROR("Failed to create host resource table lock.\n");
            return false;
        }
        host_resource_table_lock_inited = true;
    }

    os_mutex_lock(&host_resource_table_lock);
    if (g_host_resource_table != NULL) {
        os_mutex_unlock(&host_resource_table_lock);
        return true;
    }

    if (!host_resource_id_lock_inited) {
        if (os_mutex_init(&host_resource_id_lock) != BHT_OK) {
            LOG_ERROR("Failed to create host resource ID lock.\n");
            os_mutex_unlock(&host_resource_table_lock);
            return false;
        }
        host_resource_id_lock_inited = true;
    }
    // Create HashMap: size, with lock for thread safety, hash_func, key_equal,
    // key_destroy, value_destroy
    g_host_resource_table = bh_hash_map_create_with_allocator(
        HOST_RESOURCE_TABLE_SIZE, false, hash_u32_hash_map, u32_equal_hash_map,
        destroy_u32_key_hash_map, NULL, wasm_runtime_malloc, wasm_runtime_free);

    if (!g_host_resource_table) {
        LOG_ERROR("Failed to create host resource table.\n");
        os_mutex_unlock(&host_resource_table_lock);
        return false;
    }
    host_resource_live_count = 0;

    os_mutex_unlock(&host_resource_table_lock);
    return true;
}

void
destroy_host_resource_table()
{
    HostResourceTable *table;
    HostResourceCloseContext close_context = { 0 };

    if (wasm_allocation_quota_get_current() != NULL) {
        LOG_ERROR("Host resource table destruction requires no allocation "
                  "owner.\n");
        return;
    }
    if (!host_resource_table_lock_inited)
        return;

    os_mutex_lock(&host_resource_table_lock);
    table = g_host_resource_table;
    if (!table) {
        os_mutex_unlock(&host_resource_table_lock);
        return;
    }
    g_host_resource_table = NULL;

    /* Detach every resource while new acquisitions are excluded.  Pinned
       resources outlive the map and are destroyed by their last release. */
    bh_hash_map_traverse(table, mark_host_resource_closing, &close_context);
    bh_hash_map_destroy(table);
    host_resource_live_count = 0;

    os_mutex_unlock(&host_resource_table_lock);

    destroy_host_resource_list(close_context.destroy_list);
}

HostResourceTable *
get_global_host_resource_table()
{
    HostResourceTable *table;

    if (!host_resource_table_lock_inited)
        return NULL;
    os_mutex_lock(&host_resource_table_lock);
    table = g_host_resource_table;
    os_mutex_unlock(&host_resource_table_lock);
    return table;
}

static void
host_resource_pin_state_pop_and_release(HostResourcePinContext *context)
{
    HostResource *resources[HOST_RESOURCE_PIN_SCOPE_CAPACITY];
    HostResourcePinState *state;
    uint32_t count;

    if (!context || context->depth == 0)
        return;

    state = &context->states[context->depth - 1];
    count = state->count;
    if (count > HOST_RESOURCE_PIN_SCOPE_CAPACITY) {
        LOG_ERROR("Host resource pin state is corrupt.\n");
        count = HOST_RESOURCE_PIN_SCOPE_CAPACITY;
    }
    if (count > 0)
        memcpy(resources, state->resources,
               (size_t)count * sizeof(resources[0]));

    /* Pop and clear the persistent state before running any destructor.  A
       last-release destructor may reenter and reuse this depth safely. */
    memset(state, 0, sizeof(*state));
    context->depth--;
    while (count > 0)
        host_resource_release(resources[--count]);
}

bool
host_resource_pin_scope_enter_for_unwind_target(HostResourcePinScope *scope,
                                                const void *unwind_target)
{
    HostResourcePinContext *context = &current_host_resource_pin_context;
    HostResourcePinState *state;
    uint32_t i;

    if (!scope || context->depth >= HOST_RESOURCE_PIN_SCOPE_MAX_DEPTH)
        return false;
    for (i = 0; i < context->depth; i++) {
        if (context->states[i].token_address == scope)
            return false;
    }

    memset(scope, 0, sizeof(*scope));
    state = &context->states[context->depth];
    memset(state, 0, sizeof(*state));
    context->next_cookie++;
    if (context->next_cookie == 0)
        context->next_cookie++;
    state->token_address = scope;
    state->unwind_target = unwind_target;
    state->cookie = context->next_cookie;
    scope->cookie = state->cookie;
    scope->depth = context->depth;
    scope->active = true;
    context->depth++;
    return true;
}

bool
host_resource_pin_scope_enter(HostResourcePinScope *scope)
{
    return host_resource_pin_scope_enter_for_unwind_target(scope, NULL);
}

void
host_resource_pin_scope_leave(HostResourcePinScope *scope)
{
    HostResourcePinContext *context = &current_host_resource_pin_context;
    HostResourcePinState *state;

    if (!scope || !scope->active)
        return;
    if (context->depth == 0 || scope->depth != context->depth - 1) {
        LOG_ERROR("Host resource pin scopes must leave in LIFO order.\n");
        return;
    }
    state = &context->states[context->depth - 1];
    if (state->cookie != scope->cookie || state->token_address != scope) {
        LOG_ERROR("Host resource pin scope token is stale.\n");
        memset(scope, 0, sizeof(*scope));
        return;
    }

    memset(scope, 0, sizeof(*scope));
    host_resource_pin_state_pop_and_release(context);
}

void
host_resource_pin_scope_unwind_to(const void *unwind_target)
{
    HostResourcePinContext *context = &current_host_resource_pin_context;

    if (!unwind_target)
        return;
    /* A runtime catches a nonlocal exit at its current jmpbuf.  Release every
       native-call scope associated with that same frame, but preserve an
       enclosing scope while a nested guest callback handles its own trap. */
    while (context->depth > 0
           && context->states[context->depth - 1].unwind_target
                  == unwind_target) {
        host_resource_pin_state_pop_and_release(context);
    }
}

void
host_resource_pin_scope_unwind_current(void)
{
    HostResourcePinContext *context = &current_host_resource_pin_context;

    while (context->depth > 0)
        host_resource_pin_state_pop_and_release(context);
}

uint32_t
host_resource_table_get_next_id(HostResourceType type)
{
    uint32_t id = 0;

    if (!host_resource_id_lock_inited) {
        LOG_ERROR("Host resource table is not initialized.\n");
        return 0;
    }

    os_mutex_lock(&host_resource_id_lock);

    if (next_id_counter == 0 || next_id_counter > HOST_RESOURCE_ID_MASK)
        next_id_counter = 1;

    // Encode ID (type << 24) | counter
    id = (type << HOST_RESOURCE_TYPE_SHIFT) | next_id_counter;

    next_id_counter++;
    if (next_id_counter > HOST_RESOURCE_ID_MASK)
        next_id_counter = 1;

    os_mutex_unlock(&host_resource_id_lock);

    return id;
}

HostResource *
host_resource_create(HostResourceType type, uint32_t data_size)
{
    HostResource *hr =
        (HostResource *)wasm_runtime_malloc(sizeof(HostResource));
    if (!hr) {
        LOG_ERROR("Failed to allocate HostResource\n");
        return NULL;
    }

    hr->data = wasm_runtime_malloc(data_size);
    if (!hr->data) {
        LOG_ERROR("Failed to allocate HostResource data\n");
        wasm_runtime_free(hr);
        return NULL;
    }
    if (!wasm_allocation_quota_allocations_share_owner(hr, hr->data)) {
        LOG_ERROR("HostResource and data allocation owners differ\n");
        wasm_runtime_free(hr->data);
        wasm_runtime_free(hr);
        return NULL;
    }

    hr->type = type;
    hr->dtor = NULL;
    hr->native_fd_quota_attachment = NULL;
    hr->native_fd_quota_release = NULL;
    hr->native_fd_quota_count = 0;
    hr->pin_count = 0;
    hr->registered = false;
    hr->closing = false;
    hr->deferred_next = NULL;

    return hr;
}

void
host_resource_set_dtor(HostResource *hr, host_resource_dtor_t dtor)
{
    if (hr && wasm_allocation_quota_allocation_matches_current(hr)) {
        hr->dtor = dtor;
    }
}

void
host_resource_set_native_fd_quota(
    HostResource *hr, void *attachment,
    host_resource_native_fd_release_t release_callback, uint32_t fd_count)
{
    if (hr && release_callback && fd_count > 0) {
        bh_assert(hr->native_fd_quota_count == 0);
        hr->native_fd_quota_attachment = attachment;
        hr->native_fd_quota_release = release_callback;
        hr->native_fd_quota_count = fd_count;
    }
}

void
destroy_host_resource(HostResource *hr)
{
    WASMAllocationQuotaScope scope;
    bool live = false;

    if (!hr)
        return;
    if (!wasm_allocation_quota_scope_enter_for_allocation(&scope, hr)) {
        LOG_ERROR("HostResource destruction rejected by allocation owner\n");
        return;
    }
    if (host_resource_table_lock_inited) {
        os_mutex_lock(&host_resource_table_lock);
        live = hr->registered || hr->pin_count != 0;
        os_mutex_unlock(&host_resource_table_lock);
        if (live) {
            LOG_ERROR("HostResource destruction rejected while live\n");
            wasm_allocation_quota_scope_leave(&scope);
            return;
        }
    }
    if (hr->data
        && !wasm_allocation_quota_allocations_share_owner(hr, hr->data)) {
        LOG_ERROR("HostResource and data allocation owners differ\n");
        wasm_allocation_quota_scope_leave(&scope);
        return;
    }

    if (hr->data) {
        if (hr->dtor) {
            hr->dtor(hr->data);
        }
        wasm_runtime_free(hr->data);
    }
    if (hr->native_fd_quota_release && hr->native_fd_quota_count > 0) {
        hr->native_fd_quota_release(hr->native_fd_quota_attachment,
                                    hr->native_fd_quota_count);
    }
    wasm_runtime_free(hr);
    wasm_allocation_quota_scope_leave(&scope);
}

uint32_t
host_resource_table_add(HostResourceTable *table, HostResource *hr)
{
    uint32_t id = 0;
    void *key = NULL;
    uint64_t candidates;

    if (!table || !hr) {
        LOG_ERROR(
            "Host resource table add failed: table or resource is NULL.\n");
        return 0;
    }
    if (!wasm_allocation_quota_allocation_matches_current(hr)
        || (hr->data
            && !wasm_allocation_quota_allocations_share_owner(hr, hr->data))) {
        LOG_ERROR("Host resource table add rejected by allocation owner.\n");
        return 0;
    }

    if (!host_resource_table_lock_inited)
        return 0;

    os_mutex_lock(&host_resource_table_lock);
    if (table != g_host_resource_table) {
        os_mutex_unlock(&host_resource_table_lock);
        return 0;
    }
    if (hr->registered || hr->closing || hr->pin_count != 0) {
        os_mutex_unlock(&host_resource_table_lock);
        LOG_ERROR("Host resource table add rejected a live resource.\n");
        return 0;
    }

    /* Probe past every live representation after counter wrap.  Checking for
       a collision separately also distinguishes an occupied ID from an
       allocator failure inside bh_hash_map_insert(). */
    candidates = (uint64_t)host_resource_live_count + 1;
    while (candidates-- > 0) {
        id = host_resource_table_get_next_id(hr->type);
        if (id == 0)
            break;

        key = INT_TO_PTR(id);
        if (bh_hash_map_find(table, key) != NULL)
            continue;

        if (bh_hash_map_insert(table, key, hr)) {
            hr->registered = true;
            host_resource_live_count++;
            os_mutex_unlock(&host_resource_table_lock);
            return id;
        }
        break;
    }

    LOG_ERROR("Host resource table add failed: no representation available.\n");
    os_mutex_unlock(&host_resource_table_lock);
    return 0;
}

HostResource *
host_resource_table_get(HostResourceTable *table, uint32_t id)
{
    void *key = NULL;
    HostResource *hr;
    HostResourcePinContext *context = &current_host_resource_pin_context;
    HostResourcePinState *scope =
        context->depth > 0 ? &context->states[context->depth - 1] : NULL;
    bool legacy_unpinned_get = false;

    if (!table) {
        LOG_ERROR("Host resource table get failed: table is NULL.\n");
        return NULL;
    }

    if (id == 0) {
        LOG_ERROR("Host resource table get failed: invalid ID 0.\n");
        return NULL;
    }

    /* Keep direct owner-neutral test/embedding probes compatible. Quota-owned
       execution must always use the native-call pin scope. */
    if (!scope) {
        if (wasm_allocation_quota_get_current() != NULL)
            return NULL;
        legacy_unpinned_get = true;
    }
    else if (scope->count >= HOST_RESOURCE_PIN_SCOPE_CAPACITY) {
        LOG_ERROR("Host resource pin scope capacity exhausted.\n");
        return NULL;
    }
    if (!host_resource_table_lock_inited)
        return NULL;

    key = INT_TO_PTR(id);
    os_mutex_lock(&host_resource_table_lock);
    if (table != g_host_resource_table) {
        os_mutex_unlock(&host_resource_table_lock);
        return NULL;
    }
    hr = (HostResource *)bh_hash_map_find(table, key);
    if (!hr || !hr->registered || hr->closing
        || hr->type != host_resource_type_from_id(id)
        || !wasm_allocation_quota_allocation_matches_current(hr)
        || (hr->data
            && !wasm_allocation_quota_allocations_share_owner(hr, hr->data))) {
        os_mutex_unlock(&host_resource_table_lock);
        return NULL;
    }

    if (!legacy_unpinned_get) {
        if (hr->pin_count == UINT32_MAX) {
            os_mutex_unlock(&host_resource_table_lock);
            return NULL;
        }
        hr->pin_count++;
        scope->resources[scope->count++] = hr;
    }
    os_mutex_unlock(&host_resource_table_lock);
    return hr;
}

uint32_t
host_resource_table_delete(HostResourceTable *table, uint32_t id)
{
    void *key = NULL;
    void *old_key = NULL;
    void *old_value = NULL;
    HostResource *hr;
    bool destroy = false;

    if (!table) {
        LOG_ERROR("Host resource table delete failed: table is NULL.\n");
        return 0;
    }

    if (id == 0) {
        LOG_ERROR("Host resource table delete failed: invalid ID 0.\n");
        return 0;
    }
    if (!host_resource_table_lock_inited)
        return 0;

    key = INT_TO_PTR(id);

    os_mutex_lock(&host_resource_table_lock);
    if (table != g_host_resource_table) {
        os_mutex_unlock(&host_resource_table_lock);
        return 0;
    }
    hr = (HostResource *)bh_hash_map_find(table, key);
    if (!hr || !hr->registered || hr->closing
        || hr->type != host_resource_type_from_id(id)
        || !wasm_allocation_quota_allocation_matches_current(hr)) {
        os_mutex_unlock(&host_resource_table_lock);
        return 0;
    }

    hr->closing = true;
    if (!bh_hash_map_remove(table, key, &old_key, &old_value)) {
        hr->closing = false;
        os_mutex_unlock(&host_resource_table_lock);
        return 0;
    }

    if (old_value != hr) {
        LOG_ERROR("Host resource table delete observed a replaced value.\n");
        os_mutex_unlock(&host_resource_table_lock);
        return 0;
    }
    hr->registered = false;
    if (host_resource_live_count > 0)
        host_resource_live_count--;
    else
        LOG_ERROR("Host resource table live count underflow.\n");
    destroy = hr->pin_count == 0;
    os_mutex_unlock(&host_resource_table_lock);

    if (destroy)
        destroy_host_resource(hr);

    return 1;
}

#if defined(WAMR_HOST_RESOURCE_ID_TESTING)
uint32_t
host_resource_table_set_next_id_for_test(uint32_t next_id)
{
    uint32_t previous;

    if (!host_resource_id_lock_inited || next_id == 0
        || next_id > HOST_RESOURCE_ID_MASK)
        return 0;
    os_mutex_lock(&host_resource_id_lock);
    previous = next_id_counter;
    next_id_counter = next_id;
    os_mutex_unlock(&host_resource_id_lock);
    return previous;
}
#endif
