/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

extern "C" {
#include "component-model/wasm_component_host_resource.h"
#include "wasm_allocation_quota.h"
#include "wasm_runtime_common.h"
}

// Helper functions for testing ID encoding/decoding
#define HOST_RESOURCE_TYPE_SHIFT 24
#define HOST_RESOURCE_ID_MASK 0x00FFFFFF

static inline HostResourceType
test_get_type_from_id(int32_t id)
{
    return (HostResourceType)((id >> HOST_RESOURCE_TYPE_SHIFT) & 0xFF);
}

static inline uint32_t
test_get_counter_from_id(int32_t id)
{
    return (uint32_t)(id & HOST_RESOURCE_ID_MASK);
}

namespace {

std::atomic<uint32_t> pin_test_dtor_count{ 0 };

void
count_pin_test_dtor(void *)
{
    pin_test_dtor_count.fetch_add(1, std::memory_order_relaxed);
}

struct ReentrantHostResourceDtorData {
    HostResourceTable *table;
    bool *attempted;
    uint32_t *new_id;
};

void
reenter_host_resource_table(void *data)
{
    auto *reentry = static_cast<ReentrantHostResourceDtorData *>(data);
    HostResource *resource;

    *reentry->attempted = true;
    resource = host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    if (!resource)
        return;
    *reentry->new_id = host_resource_table_add(reentry->table, resource);
    if (*reentry->new_id == 0)
        destroy_host_resource(resource);
}

struct TestQuotaState {
    uint64_t live_bytes = 0;
    uint32_t live_allocations = 0;
    uint32_t attachment_retain_count = 0;
    uint32_t attachment_release_count = 0;
};

bool
test_quota_reserve(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *quota = static_cast<TestQuotaState *>(attachment);
    quota->live_bytes += bytes;
    quota->live_allocations += allocations;
    return true;
}

void
test_quota_release(void *attachment, uint64_t bytes, uint32_t allocations)
{
    auto *quota = static_cast<TestQuotaState *>(attachment);
    EXPECT_LE(bytes, quota->live_bytes);
    EXPECT_LE(allocations, quota->live_allocations);
    quota->live_bytes -= bytes;
    quota->live_allocations -= allocations;
}

bool
test_quota_attachment_retain(void *attachment)
{
    auto *quota = static_cast<TestQuotaState *>(attachment);
    quota->attachment_retain_count++;
    return true;
}

void
test_quota_attachment_release(void *attachment)
{
    auto *quota = static_cast<TestQuotaState *>(attachment);
    quota->attachment_release_count++;
}

LoadArgs2
test_quota_args(TestQuotaState *quota)
{
    LoadArgs2 args = {};

    args.allocation_quota_attachment = quota;
    args.allocation_quota_reserve = test_quota_reserve;
    args.allocation_quota_release = test_quota_release;
    args.allocation_quota_attachment_retain = test_quota_attachment_retain;
    args.allocation_quota_attachment_release = test_quota_attachment_release;
    return args;
}

} // namespace

class HostResourceTableTest : public testing::Test
{
  public:
    HostResourceTableTest() : table_(nullptr) {}
    ~HostResourceTableTest() {}

    virtual void SetUp() {
        RuntimeInitArgs init_args = {};
        init_args.mem_alloc_type = Alloc_With_Pool;
        init_args.mem_alloc_option.pool.heap_buf = runtime_heap_;
        init_args.mem_alloc_option.pool.heap_size = sizeof(runtime_heap_);
        runtime_initialized_ = wasm_runtime_full_init(&init_args);
        ASSERT_TRUE(runtime_initialized_);

        bool success = instantiate_host_resource_table();
        ASSERT_TRUE(success);
        table_ = get_global_host_resource_table();
        ASSERT_NE(table_, nullptr);
    }

    virtual void TearDown() {
        if (table_) {
            destroy_host_resource_table();
            table_ = nullptr;
        }
        if (runtime_initialized_) {
            wasm_runtime_destroy();
            runtime_initialized_ = false;
        }
    }

  protected:
    static constexpr size_t RUNTIME_HEAP_SIZE = 1024 * 1024;
    alignas(8) char runtime_heap_[RUNTIME_HEAP_SIZE];
    bool runtime_initialized_ = false;
    HostResourceTable *table_;

    // Helper function to create a test host resource
    HostResource* createTestResource(HostResourceType type, void *data = nullptr) {
        HostResource *hr = (HostResource *)wasm_runtime_malloc(sizeof(HostResource));
        if (!hr) return nullptr;

        std::memset(hr, 0, sizeof(*hr));
        hr->type = type;
        hr->data = data;
        hr->dtor = NULL;
        return hr;
    }

    // Helper to create test data
    void* createTestData(int value) {
        int *data = (int *)wasm_runtime_malloc(sizeof(int));
        if (data) *data = value;
        return data;
    }
};

// Test table creation and destruction
TEST_F(HostResourceTableTest, Table_CreateDestroy)
{
    // Table is created in SetUp and destroyed in TearDown
    EXPECT_NE(table_, nullptr);
}

// Test adding a single resource and getting it back
TEST_F(HostResourceTableTest, Table_AddGetSingle)
{
    void *data = createTestData(42);
    HostResource *hr = createTestResource(WASI_P2_TCP_SOCKET, data);
    ASSERT_NE(hr, nullptr);

    // Add resource to table
    int32_t id = host_resource_table_add(table_, hr);
    EXPECT_GT(id, 0);

    // Get resource back
    HostResource *retrieved = host_resource_table_get(table_, id);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved, hr);
    EXPECT_EQ(retrieved->type, WASI_P2_TCP_SOCKET);
    EXPECT_EQ(retrieved->data, data);
    EXPECT_EQ(*(int*)retrieved->data, 42);
}

// Test adding multiple resources
TEST_F(HostResourceTableTest, Table_AddMultiple)
{
    std::vector<int32_t> ids;

    // Add 5 TCP sockets
    for (int i = 0; i < 5; i++) {
        void *data = createTestData(100 + i);
        HostResource *hr = createTestResource(WASI_P2_TCP_SOCKET, data);
        ASSERT_NE(hr, nullptr);

        int32_t id = host_resource_table_add(table_, hr);
        EXPECT_NE(id, 0);
        ids.push_back(id);
    }

    // Verify all IDs are unique
    for (size_t i = 0; i < ids.size(); i++) {
        for (size_t j = i + 1; j < ids.size(); j++) {
            EXPECT_NE(ids[i], ids[j]);
        }
    }

    // Verify all resources can be retrieved
    for (size_t i = 0; i < ids.size(); i++) {
        HostResource *retrieved = host_resource_table_get(table_, ids[i]);
        ASSERT_NE(retrieved, nullptr);
        EXPECT_EQ(retrieved->type, WASI_P2_TCP_SOCKET);
        EXPECT_EQ(*(int*)retrieved->data, 100 + (int)i);
    }
}

// Test adding resources with different types
TEST_F(HostResourceTableTest, Table_DifferentTypes)
{
    void *data1 = createTestData(1);
    void *data2 = createTestData(2);
    void *data3 = createTestData(3);

    HostResource *tcp = createTestResource(WASI_P2_TCP_SOCKET, data1);
    HostResource *udp = createTestResource(WASI_P2_UDP_SOCKET, data2);
    HostResource *net = createTestResource(WASI_P2_NETWORK, data3);

    int32_t tcp_id = host_resource_table_add(table_, tcp);
    int32_t udp_id = host_resource_table_add(table_, udp);
    int32_t net_id = host_resource_table_add(table_, net);

    EXPECT_NE(tcp_id, 0);
    EXPECT_NE(udp_id, 0);
    EXPECT_NE(net_id, 0);

    // IDs should be different
    EXPECT_NE(tcp_id, udp_id);
    EXPECT_NE(tcp_id, net_id);
    EXPECT_NE(udp_id, net_id);

    // Verify types are encoded correctly (upper 8 bits)
    EXPECT_EQ(test_get_type_from_id(tcp_id), WASI_P2_TCP_SOCKET);
    EXPECT_EQ(test_get_type_from_id(udp_id), WASI_P2_UDP_SOCKET);
    EXPECT_EQ(test_get_type_from_id(net_id), WASI_P2_NETWORK);

    // Verify resources can be retrieved with correct types
    HostResource *retrieved_tcp = host_resource_table_get(table_, tcp_id);
    HostResource *retrieved_udp = host_resource_table_get(table_, udp_id);
    HostResource *retrieved_net = host_resource_table_get(table_, net_id);

    ASSERT_NE(retrieved_tcp, nullptr);
    ASSERT_NE(retrieved_udp, nullptr);
    ASSERT_NE(retrieved_net, nullptr);

    EXPECT_EQ(retrieved_tcp->type, WASI_P2_TCP_SOCKET);
    EXPECT_EQ(retrieved_udp->type, WASI_P2_UDP_SOCKET);
    EXPECT_EQ(retrieved_net->type, WASI_P2_NETWORK);
}

// Test deleting a resource
TEST_F(HostResourceTableTest, Table_Delete)
{
    void *data = createTestData(99);
    HostResource *hr = createTestResource(WASI_P2_TCP_SOCKET, data);

    int32_t id = host_resource_table_add(table_, hr);
    EXPECT_NE(id, 0);

    // Verify it exists
    HostResource *retrieved = host_resource_table_get(table_, id);
    ASSERT_NE(retrieved, nullptr);

    // Delete it
    int32_t result = host_resource_table_delete(table_, id);
    EXPECT_EQ(result, 1);

    // Verify it's gone
    retrieved = host_resource_table_get(table_, id);
    EXPECT_EQ(retrieved, nullptr);

    // Try to delete again - should fail
    result = host_resource_table_delete(table_, id);
    EXPECT_EQ(result, 0); // Not found
}

TEST_F(HostResourceTableTest, PinnedLookupDefersDeleteUntilScopeLeave)
{
    HostResourcePinScope scope = {};
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    *static_cast<uint32_t *>(resource->data) = 37;
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);

    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(host_resource_pin_scope_enter(&scope));
    HostResource *borrowed = host_resource_table_get(table_, id);
    ASSERT_EQ(borrowed, resource);

    EXPECT_EQ(host_resource_table_delete(table_, id), 1u);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(host_resource_table_get(table_, id), nullptr);
    EXPECT_EQ(*static_cast<uint32_t *>(borrowed->data), 37u);

    host_resource_pin_scope_leave(&scope);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(HostResourceTableTest, PinnedLookupDefersTableTeardownUntilScopeLeave)
{
    HostResourcePinScope scope = {};
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    *static_cast<uint32_t *>(resource->data) = 73;
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);

    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(host_resource_pin_scope_enter(&scope));
    HostResource *borrowed = host_resource_table_get(table_, id);
    ASSERT_EQ(borrowed, resource);

    destroy_host_resource_table();
    table_ = nullptr;
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(*static_cast<uint32_t *>(borrowed->data), 73u);

    host_resource_pin_scope_leave(&scope);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(HostResourceTableTest,
       ConcurrentTableTeardownDefersPinnedResourceDestruction)
{
    HostResourcePinScope scope = {};
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    *static_cast<uint32_t *>(resource->data) = 79;
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);

    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(host_resource_pin_scope_enter(&scope));
    HostResource *borrowed = host_resource_table_get(table_, id);
    ASSERT_EQ(borrowed, resource);

    std::thread teardown([]() { destroy_host_resource_table(); });
    teardown.join();
    table_ = nullptr;
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(*static_cast<uint32_t *>(borrowed->data), 79u);

    host_resource_pin_scope_leave(&scope);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(HostResourceTableTest, DeleteDestructorCanReenterTable)
{
    bool attempted = false;
    uint32_t new_id = 0;
    HostResource *resource = host_resource_create(
        WASI_P2_TCP_SOCKET, sizeof(ReentrantHostResourceDtorData));
    ASSERT_NE(resource, nullptr);
    auto *reentry =
        static_cast<ReentrantHostResourceDtorData *>(resource->data);
    reentry->table = table_;
    reentry->attempted = &attempted;
    reentry->new_id = &new_id;
    host_resource_set_dtor(resource, reenter_host_resource_table);

    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);
    ASSERT_EQ(host_resource_table_delete(table_, id), 1u);
    EXPECT_TRUE(attempted);
    ASSERT_NE(new_id, 0u);
    EXPECT_EQ(host_resource_table_delete(table_, new_id), 1u);
}

TEST_F(HostResourceTableTest, ConcurrentDeleteDefersUntilPinnedScopeRelease)
{
    struct Coordination {
        std::mutex mutex;
        std::condition_variable condition;
        bool pinned = false;
        bool deleted = false;
        bool lookup_succeeded = false;
        bool data_survived = false;
        uint32_t delete_result = 0;
        uint32_t dtor_count_before_release = UINT32_MAX;
    } coordination;

    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    *static_cast<uint32_t *>(resource->data) = 91;
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);
    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);

    std::thread borrower([&]() {
        HostResourcePinScope scope = {};
        host_resource_pin_scope_enter(&scope);
        HostResource *borrowed = host_resource_table_get(table_, id);
        {
            std::unique_lock<std::mutex> lock(coordination.mutex);
            coordination.lookup_succeeded = borrowed == resource;
            coordination.pinned = true;
            coordination.condition.notify_all();
            coordination.condition.wait(lock,
                                        [&]() { return coordination.deleted; });
        }
        coordination.data_survived =
            borrowed
            && *static_cast<uint32_t *>(borrowed->data) == UINT32_C(91);
        host_resource_pin_scope_leave(&scope);
    });
    std::thread deleter([&]() {
        {
            std::unique_lock<std::mutex> lock(coordination.mutex);
            coordination.condition.wait(lock,
                                        [&]() { return coordination.pinned; });
        }
        coordination.delete_result = host_resource_table_delete(table_, id);
        coordination.dtor_count_before_release =
            pin_test_dtor_count.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(coordination.mutex);
            coordination.deleted = true;
        }
        coordination.condition.notify_all();
    });

    borrower.join();
    deleter.join();
    EXPECT_TRUE(coordination.lookup_succeeded);
    EXPECT_TRUE(coordination.data_survived);
    EXPECT_EQ(coordination.delete_result, 1u);
    EXPECT_EQ(coordination.dtor_count_before_release, 0u);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(HostResourceTableTest, PinScopeCapacityFailsClosedAndReleasesAllPins)
{
    HostResourcePinScope scope = {};
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);
    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);

    ASSERT_TRUE(host_resource_pin_scope_enter(&scope));
    for (uint32_t i = 0; i < HOST_RESOURCE_PIN_SCOPE_CAPACITY; i++) {
        ASSERT_EQ(host_resource_table_get(table_, id), resource);
    }
    EXPECT_EQ(host_resource_table_get(table_, id), nullptr);
    EXPECT_EQ(host_resource_table_delete(table_, id), 1u);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 0u);
    host_resource_pin_scope_leave(&scope);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(HostResourceTableTest,
       NonlocalUnwindReleasesOnlyScopesForTheCaughtTarget)
{
    HostResourcePinScope outer_scope = {};
    HostResourcePinScope inner_scope = {};
    int outer_target = 0;
    int inner_target = 0;
    HostResource *outer =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    HostResource *inner =
        host_resource_create(WASI_P2_UDP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    *static_cast<uint32_t *>(outer->data) = 101;
    *static_cast<uint32_t *>(inner->data) = 202;
    host_resource_set_dtor(outer, count_pin_test_dtor);
    host_resource_set_dtor(inner, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);

    const uint32_t outer_id = host_resource_table_add(table_, outer);
    const uint32_t inner_id = host_resource_table_add(table_, inner);
    ASSERT_NE(outer_id, 0u);
    ASSERT_NE(inner_id, 0u);
    ASSERT_TRUE(host_resource_pin_scope_enter_for_unwind_target(&outer_scope,
                                                                &outer_target));
    ASSERT_EQ(host_resource_table_get(table_, outer_id), outer);
    ASSERT_TRUE(host_resource_pin_scope_enter_for_unwind_target(&inner_scope,
                                                                &inner_target));
    ASSERT_EQ(host_resource_table_get(table_, inner_id), inner);
    ASSERT_EQ(host_resource_table_delete(table_, outer_id), 1u);
    ASSERT_EQ(host_resource_table_delete(table_, inner_id), 1u);

    host_resource_pin_scope_unwind_to(&inner_target);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(*static_cast<uint32_t *>(outer->data), 101u);

    host_resource_pin_scope_unwind_to(&outer_target);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 2u);
}

TEST_F(HostResourceTableTest, WrongOwnerCannotAcquireOrDelete)
{
    TestQuotaState owner_quota = {};
    TestQuotaState wrong_quota = {};
    LoadArgs2 owner_args = test_quota_args(&owner_quota);
    LoadArgs2 wrong_args = test_quota_args(&wrong_quota);
    WASMAllocationQuotaScope owner_creation_scope = {};
    WASMAllocationQuotaScope wrong_owner_scope = {};
    WASMAllocationQuotaScope owner_access_scope = {};
    HostResourcePinScope pin_scope = {};

    ASSERT_TRUE(wasm_allocation_quota_scope_enter_for_load(
        &owner_creation_scope, &owner_args));
    HostResource *resource =
        host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
    ASSERT_NE(resource, nullptr);
    host_resource_set_dtor(resource, count_pin_test_dtor);
    pin_test_dtor_count.store(0, std::memory_order_relaxed);
    uint32_t id = host_resource_table_add(table_, resource);
    ASSERT_NE(id, 0u);
    wasm_allocation_quota_scope_leave(&owner_creation_scope);

    ASSERT_TRUE(wasm_allocation_quota_scope_enter_for_load(&wrong_owner_scope,
                                                           &wrong_args));
    ASSERT_TRUE(host_resource_pin_scope_enter(&pin_scope));
    EXPECT_EQ(host_resource_table_get(table_, id), nullptr);
    EXPECT_EQ(host_resource_table_delete(table_, id), 0u);
    host_resource_pin_scope_leave(&pin_scope);
    wasm_allocation_quota_scope_leave(&wrong_owner_scope);

    ASSERT_TRUE(wasm_allocation_quota_scope_enter_for_allocation(
        &owner_access_scope, resource));
    ASSERT_TRUE(host_resource_pin_scope_enter(&pin_scope));
    EXPECT_EQ(host_resource_table_get(table_, id), resource);
    EXPECT_EQ(host_resource_table_delete(table_, id), 1u);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 0u);
    host_resource_pin_scope_leave(&pin_scope);
    EXPECT_EQ(pin_test_dtor_count.load(std::memory_order_relaxed), 1u);
    wasm_allocation_quota_scope_leave(&owner_access_scope);

    EXPECT_EQ(owner_quota.live_bytes, 0u);
    EXPECT_EQ(owner_quota.live_allocations, 0u);
    EXPECT_EQ(owner_quota.attachment_retain_count,
              owner_quota.attachment_release_count);
    EXPECT_EQ(wrong_quota.live_bytes, 0u);
    EXPECT_EQ(wrong_quota.live_allocations, 0u);
    EXPECT_EQ(wrong_quota.attachment_retain_count,
              wrong_quota.attachment_release_count);
}

TEST_F(HostResourceTableTest, OfflineCommonsDroppedHandlesRemainStale)
{
    void *first_data = createTestData(7);
    HostResource *first = createTestResource(WASI_P2_NETWORK, first_data);
    ASSERT_NE(first, nullptr);

    const uint32_t stale_id = host_resource_table_add(table_, first);
    ASSERT_NE(stale_id, 0u);
    ASSERT_EQ(host_resource_table_delete(table_, stale_id), 1u);
    ASSERT_EQ(host_resource_table_get(table_, stale_id), nullptr);

    void *replacement_data = createTestData(8);
    HostResource *replacement =
        createTestResource(WASI_P2_NETWORK, replacement_data);
    ASSERT_NE(replacement, nullptr);
    const uint32_t replacement_id =
        host_resource_table_add(table_, replacement);
    ASSERT_NE(replacement_id, 0u);
    ASSERT_NE(replacement_id, stale_id);
    ASSERT_EQ(host_resource_table_get(table_, stale_id), nullptr);
}

// Test get_next_id function
TEST_F(HostResourceTableTest, Table_GetNextId)
{
    const uint32_t first_counter = test_get_counter_from_id(
        host_resource_table_get_next_id(WASI_P2_NETWORK));
    // Get IDs for different types
    int32_t id1 = host_resource_table_get_next_id(WASI_P2_TCP_SOCKET);
    int32_t id2 = host_resource_table_get_next_id(WASI_P2_UDP_SOCKET);
    int32_t id3 = host_resource_table_get_next_id(WASI_P2_TCP_SOCKET);

    EXPECT_NE(id1, 0);
    EXPECT_NE(id2, 0);
    EXPECT_NE(id3, 0);

    // All IDs should be unique
    EXPECT_NE(id1, id2);
    EXPECT_NE(id1, id3);
    EXPECT_NE(id2, id3);

    // Verify types are encoded correctly
    EXPECT_EQ(test_get_type_from_id(id1), WASI_P2_TCP_SOCKET);
    EXPECT_EQ(test_get_type_from_id(id2), WASI_P2_UDP_SOCKET);
    EXPECT_EQ(test_get_type_from_id(id3), WASI_P2_TCP_SOCKET);

    // Counters should increment
    EXPECT_EQ(test_get_counter_from_id(id1), first_counter + 1u);
    EXPECT_EQ(test_get_counter_from_id(id2), first_counter + 2u);
    EXPECT_EQ(test_get_counter_from_id(id3), first_counter + 3u);
}

TEST_F(HostResourceTableTest, CounterWrapSkipsLiveRepresentations)
{
    const uint32_t previous =
        host_resource_table_set_next_id_for_test(HOST_RESOURCE_ID_MASK);
    ASSERT_NE(previous, 0u);

    HostResource *last =
        host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    HostResource *first =
        host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    HostResource *next =
        host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    ASSERT_NE(last, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(next, nullptr);

    const uint32_t last_id = host_resource_table_add(table_, last);
    const uint32_t first_id = host_resource_table_add(table_, first);
    EXPECT_EQ(test_get_counter_from_id(last_id), HOST_RESOURCE_ID_MASK);
    EXPECT_EQ(test_get_counter_from_id(first_id), 1u);

    ASSERT_NE(host_resource_table_set_next_id_for_test(HOST_RESOURCE_ID_MASK),
              0u);
    const uint32_t next_id = host_resource_table_add(table_, next);
    EXPECT_EQ(test_get_counter_from_id(next_id), 2u);
    EXPECT_NE(next_id, last_id);
    EXPECT_NE(next_id, first_id);

    EXPECT_EQ(host_resource_table_delete(table_, last_id), 1u);
    EXPECT_EQ(host_resource_table_delete(table_, first_id), 1u);
    EXPECT_EQ(host_resource_table_delete(table_, next_id), 1u);
    EXPECT_NE(host_resource_table_set_next_id_for_test(previous), 0u);
}

TEST_F(HostResourceTableTest, StaleHandleCannotCrossTableReinstantiation)
{
    HostResourceTable *stale_table = table_;
    HostResource *first =
        host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    ASSERT_NE(first, nullptr);
    const uint32_t stale_id = host_resource_table_add(stale_table, first);
    ASSERT_NE(stale_id, 0u);

    destroy_host_resource_table();
    ASSERT_TRUE(instantiate_host_resource_table());
    table_ = get_global_host_resource_table();
    ASSERT_NE(table_, nullptr);

    HostResource *replacement =
        host_resource_create(WASI_P2_NETWORK, sizeof(uint32_t));
    ASSERT_NE(replacement, nullptr);
    const uint32_t replacement_id =
        host_resource_table_add(table_, replacement);
    ASSERT_NE(replacement_id, 0u);
    EXPECT_NE(replacement_id, stale_id);

    EXPECT_EQ(host_resource_table_get(table_, stale_id), nullptr);
    EXPECT_EQ(host_resource_table_delete(table_, stale_id), 0u);
    EXPECT_EQ(host_resource_table_get(stale_table, stale_id), nullptr);
    EXPECT_EQ(host_resource_table_delete(stale_table, stale_id), 0u);
}

TEST_F(HostResourceTableTest, Table_GetNextIdConcurrent)
{
    constexpr size_t thread_count = 8;
    constexpr size_t ids_per_thread = 256;
    std::vector<std::vector<uint32_t>> ids(
        thread_count, std::vector<uint32_t>(ids_per_thread));
    std::vector<std::thread> threads;

    for (size_t thread_index = 0; thread_index < thread_count; thread_index++) {
        threads.emplace_back([thread_index, &ids]() {
            for (size_t id_index = 0; id_index < ids_per_thread; id_index++) {
                ids[thread_index][id_index] =
                    host_resource_table_get_next_id(WASI_P2_TCP_SOCKET);
            }
        });
    }

    for (std::thread &thread : threads) {
        thread.join();
    }

    std::unordered_set<uint32_t> unique_ids;
    for (const auto &thread_ids : ids) {
        for (uint32_t id : thread_ids) {
            EXPECT_NE(id, 0u);
            EXPECT_EQ(test_get_type_from_id(id), WASI_P2_TCP_SOCKET);
            unique_ids.insert(id);
        }
    }

    EXPECT_EQ(unique_ids.size(), thread_count * ids_per_thread);
}

// Test error conditions
TEST_F(HostResourceTableTest, Table_ErrorConditions)
{
    // Test add with NULL resource
    int32_t id = host_resource_table_add(table_, nullptr);
    EXPECT_EQ(id, 0); // Should fail

    // Test get with invalid ID
    HostResource *retrieved = host_resource_table_get(table_, 0);
    EXPECT_EQ(retrieved, nullptr);

    retrieved = host_resource_table_get(table_, 99999);
    EXPECT_EQ(retrieved, nullptr);

    // Test delete with invalid ID
    int32_t result = host_resource_table_delete(table_, 0);
    EXPECT_EQ(result, 0);

    result = host_resource_table_delete(table_, 99999);
    EXPECT_EQ(result, 0);

    // Test with NULL table
    retrieved = host_resource_table_get(nullptr, 1);
    EXPECT_EQ(retrieved, nullptr);

    result = host_resource_table_delete(nullptr, 1);
    EXPECT_EQ(result, 0);

    void *data = createTestData(1);
    HostResource *hr = createTestResource(WASI_P2_TCP_SOCKET, data);
    id = host_resource_table_add(nullptr, hr);
    EXPECT_EQ(id, 0);

    // Clean up since add failed
    wasm_runtime_free(data);
    wasm_runtime_free(hr);
}

// Test ID encoding/decoding helpers
TEST_F(HostResourceTableTest, IDEncoding)
{
    // Manually create IDs with known type/counter values
    int32_t id = (WASI_P2_TCP_SOCKET << 24) | 42;

    EXPECT_EQ(test_get_type_from_id(id), WASI_P2_TCP_SOCKET);
    EXPECT_EQ(test_get_counter_from_id(id), 42u);

    id = (WASI_P2_UDP_SOCKET << 24) | 12345;
    EXPECT_EQ(test_get_type_from_id(id), WASI_P2_UDP_SOCKET);
    EXPECT_EQ(test_get_counter_from_id(id), 12345u);
}

// Test that table properly cleans up on destroy
TEST_F(HostResourceTableTest, Table_CleanupOnDestroy)
{
    // Add several resources
    for (int i = 0; i < 10; i++) {
        void *data = createTestData(i);
        HostResource *hr = createTestResource(WASI_P2_TCP_SOCKET, data);
        int32_t id = host_resource_table_add(table_, hr);
        EXPECT_NE(id, 0);
    }
}
