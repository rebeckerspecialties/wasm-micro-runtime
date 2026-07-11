/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "wasm_component_resource.h"
#include "wasm_component_resource_table.h"
#include "wasm_component_host_resource.h"
#include "wasm_component_canon.h"
#include "wasm_component_task.h"
#include "wasm_component_canonical.h"
#include "wasm_runtime_common.h"
#include "wasm_component.h"
#include "wasm_component_runtime.h"
}

namespace {

int allocation_fail_after = -1;
std::atomic<uint32_t> host_resource_dtor_count{ 0 };

struct CustomDropObservation {
    uint32_t count;
    uint32_t representation;
};

bool
observe_custom_resource_drop(void *attachment, uint32_t representation)
{
    CustomDropObservation *observation =
        static_cast<CustomDropObservation *>(attachment);
    if (!observation) {
        return false;
    }
    observation->count++;
    observation->representation = representation;
    return true;
}

void *
failure_injecting_malloc(unsigned int size)
{
    if (allocation_fail_after == 0) {
        return nullptr;
    }
    if (allocation_fail_after > 0) {
        allocation_fail_after--;
    }
    return std::malloc(size);
}

void *
failure_injecting_realloc(void *ptr, unsigned int size)
{
    return std::realloc(ptr, size);
}

void
failure_injecting_free(void *ptr)
{
    std::free(ptr);
}

void
count_host_resource_dtor(void *)
{
    host_resource_dtor_count.fetch_add(1, std::memory_order_relaxed);
}

WASMComponentResourceInstance *
test_resource_type()
{
    static WASMComponentResourceInstance resource_type = {
        .name = (char *)"test_resource",
        .interface_name = (char *)"test_interface",
        .impl = NULL,
        .drop_method = NULL,
        .new_method = NULL,
        .rep_method = NULL,
        .dtor_method = NULL,
        .ctor_method = NULL,
    };
    return &resource_type;
}

} // namespace

class ResourceTableTest : public testing::Test
{
  public:
    ResourceTableTest()
      : table_(nullptr)
    {
    }
    ~ResourceTableTest() {}

    virtual void SetUp() { wasm_runtime_init(); }

    virtual void TearDown()
    {
        if (table_) {
            wasm_component_table_destroy(table_);
            table_ = nullptr;
        }
        wasm_runtime_destroy();
    }

  protected:
    WASMComponentResourceTable *table_;

    // Helper function to create a test resource handle
    WASMResourceHandle *createTestResource(uint32_t rep = 42, bool own = true)
    {
        static WASMComponentResourceInstance dummy_rt = {
            .name = (char *)"test_resource",
            .interface_name = (char *)"test_interface",
            .impl = NULL,
            .drop_method = NULL,
            .new_method = NULL,
            .rep_method = NULL,
            .dtor_method = NULL,
            .ctor_method = NULL
        };

        return wasm_create_resource_handle(&dummy_rt, rep, own);
    }
};

// Test resource handle creation and destruction
TEST_F(ResourceTableTest, ResourceHandle_CreateDestroy)
{
    uint32_t test_rep = 456;
    WASMResourceHandle *handle = createTestResource(test_rep, true);

    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(handle->own);
    EXPECT_EQ(handle->rep, test_rep);
    EXPECT_EQ(handle->num_lends, 0u);

    wasm_destroy_resource_handle(handle);
}

// Test wrapper for i32 and guard conditions
TEST_F(ResourceTableTest, ResourceHandle_ConvenienceWrapper)
{
    static WASMComponentResourceInstance dummy_rt = {
        .name = (char *)"test_resource",
        .interface_name = (char *)"test_interface",
        .impl = NULL,
        .drop_method = NULL,
        .new_method = NULL,
        .rep_method = NULL,
        .dtor_method = NULL,
        .ctor_method = NULL
    };

    WASMResourceHandle *handle = createTestResource(123, true);

    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(handle->rep, 123u);
    EXPECT_EQ(wasm_resource_handle_get_rep_i32(handle), 123u);

    wasm_destroy_resource_handle(handle);

    // Test guard conditions
    EXPECT_EQ(wasm_create_resource_handle(nullptr, 42, true),
              nullptr); // null rt
    EXPECT_EQ(wasm_create_resource_handle(&dummy_rt, 0, true),
              nullptr); // zero rep (invalid)

    // Test wrapper with null handle
    EXPECT_EQ(wasm_resource_handle_get_rep_i32(nullptr), 0u);
}

TEST_F(ResourceTableTest, Table_InitDestroy)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);

    EXPECT_EQ(table_->array_size, 4u);
    EXPECT_EQ(table_->free_count, 0u);
    EXPECT_EQ(table_->next_index, 1u); // Index 0 is reserved
    EXPECT_EQ(table_->resize_percent, 50u);
    EXPECT_NE(table_->array, nullptr);
    EXPECT_NE(table_->free_list, nullptr);
}

// Test adding and removing a single resource
TEST_F(ResourceTableTest, Table_AddRemoveSingle)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);

    // Create and add a resource
    WASMResourceHandle *handle = createTestResource();
    ASSERT_NE(handle, nullptr);

    uint32_t index;
    bool result = wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index);

    EXPECT_TRUE(result);
    EXPECT_EQ(index, 1u); // First valid index should be 1
    EXPECT_EQ(table_->next_index, 2u);

    // Retrieve the resource
    WASMResourceHandle *retrieved =
        (WASMResourceHandle *)wasm_component_table_get(
            table_, index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    EXPECT_EQ(retrieved, handle);

    // Remove the resource
    result = wasm_component_table_remove(table_, index);
    EXPECT_TRUE(result);
    EXPECT_EQ(table_->free_count, 1u);

    // Verify it's gone
    retrieved = (WASMResourceHandle *)wasm_component_table_get(
        table_, index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    EXPECT_EQ(retrieved, nullptr);
}

// Test free list functionality: add 3, remove 1, check free list, add 1 more
TEST_F(ResourceTableTest, Table_FreeListReuse)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);

    // Add 3 resources
    std::vector<uint32_t> indices;

    for (int i = 0; i < 3; i++) {
        WASMResourceHandle *handle = createTestResource();
        ASSERT_NE(handle, nullptr);

        uint32_t index;
        bool result = wasm_component_table_add(
            table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index);
        EXPECT_TRUE(result);
        EXPECT_EQ(index, static_cast<uint32_t>(i + 1)); // Should be 1, 2, 3
        indices.push_back(index);
    }

    EXPECT_EQ(table_->next_index, 4u);
    EXPECT_EQ(table_->free_count, 0u);

    // Remove the middle resource (index 2)
    uint32_t removed_index = indices[1]; // index 2
    bool result = wasm_component_table_remove(table_, removed_index);
    EXPECT_TRUE(result);
    EXPECT_EQ(table_->free_count, 1u);

    // Check that the free list contains the removed index
    EXPECT_EQ(table_->free_list[0], removed_index);

    // Add a new resource - it should reuse the freed index
    WASMResourceHandle *new_handle = createTestResource();
    ASSERT_NE(new_handle, nullptr);

    uint32_t new_index;
    result = wasm_component_table_add(
        table_, new_handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &new_index);
    EXPECT_TRUE(result);
    EXPECT_EQ(new_index, removed_index); // Should reuse the freed index
    EXPECT_EQ(table_->free_count, 0u);   // Free list should be empty again

    // Verify the new resource is accessible
    WASMResourceHandle *retrieved =
        (WASMResourceHandle *)wasm_component_table_get(
            table_, new_index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    EXPECT_EQ(retrieved, new_handle);
}

// Test table resizing
TEST_F(ResourceTableTest, Table_Resize)
{
    table_ = wasm_component_table_init(2, 100); // 2 initial size, 100% growth
    ASSERT_NE(table_, nullptr);

    EXPECT_EQ(table_->array_size, 2u);

    std::vector<uint32_t> indices;

    // Add resources until we exceed initial capacity and trigger resize
    // index 0 is reserved, so it can initially fit 1 resource
    // Adding 2+ resources should trigger resize
    for (int i = 0; i < 4; i++) {
        WASMResourceHandle *handle = createTestResource();
        ASSERT_NE(handle, nullptr);

        uint32_t index;
        bool result = wasm_component_table_add(
            table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index);
        EXPECT_TRUE(result);
        indices.push_back(index);

        // After adding the 2nd resource, table should have grown
        if (i == 1) {
            EXPECT_GT(table_->array_size, 2u); // Should have grown
        }
    }

    EXPECT_GT(table_->array_size,
              2u); // Final size should be larger than initial
    EXPECT_EQ(table_->next_index,
              5u); // Should have 4 resources at indices 1,2,3,4

    // Verify all resources are still accessible after resize
    for (size_t i = 0; i < indices.size(); i++) {
        WASMResourceHandle *retrieved =
            (WASMResourceHandle *)wasm_component_table_get(
                table_, indices[i], WASM_TABLE_ELEM_RESOURCE_HANDLE);
        EXPECT_NE(retrieved, nullptr); // Just check it exists
    }

    // Test that free_list is also resized properly by removing and re-adding
    uint32_t removed_index = indices[1]; // Remove second resource
    bool result = wasm_component_table_remove(table_, removed_index);
    EXPECT_TRUE(result);
    EXPECT_EQ(table_->free_count, 1u);

    // Add a new resource - should reuse the freed slot
    WASMResourceHandle *new_handle = createTestResource();
    uint32_t new_index;
    result = wasm_component_table_add(
        table_, new_handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &new_index);
    EXPECT_TRUE(result);
    EXPECT_EQ(new_index, removed_index); // Should reuse freed index
}

class ResourceTableAllocationFailureTest : public testing::Test
{
  public:
    void SetUp() override
    {
        RuntimeInitArgs init_args = {};
        allocation_fail_after = -1;
        init_args.mem_alloc_type = Alloc_With_Allocator;
        init_args.mem_alloc_option.allocator.malloc_func =
            (void *)failure_injecting_malloc;
        init_args.mem_alloc_option.allocator.realloc_func =
            (void *)failure_injecting_realloc;
        init_args.mem_alloc_option.allocator.free_func =
            (void *)failure_injecting_free;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    }

    void TearDown() override
    {
        allocation_fail_after = -1;
        if (table_) {
            wasm_component_table_destroy(table_);
        }
        wasm_runtime_destroy();
    }

  protected:
    WASMComponentResourceTable *table_ = nullptr;
};

TEST_F(ResourceTableAllocationFailureTest,
       ResizeFailureLeavesBothOldBuffersAndContentsIntact)
{
    table_ = wasm_component_table_init(2, 100);
    ASSERT_NE(table_, nullptr);

    WASMResourceHandle *first =
        wasm_create_resource_handle(test_resource_type(), 100, true);
    ASSERT_NE(first, nullptr);
    uint32_t first_index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        table_, first, WASM_TABLE_ELEM_RESOURCE_HANDLE, &first_index));

    WASMResourceHandle *second =
        wasm_create_resource_handle(test_resource_type(), 200, true);
    ASSERT_NE(second, nullptr);

    WASMTableElement **old_array = table_->array;
    uint32_t *old_free_list = table_->free_list;
    uint32_t old_size = table_->array_size;
    uint32_t old_next_index = table_->next_index;

    /* table_add allocates its element wrapper, then resize allocates the new
     * element array and free list. Fail the latter allocation, after a new
     * element array has already been allocated. */
    allocation_fail_after = 2;
    uint32_t second_index = 0;
    EXPECT_FALSE(wasm_component_table_add(
        table_, second, WASM_TABLE_ELEM_RESOURCE_HANDLE, &second_index));
    allocation_fail_after = -1;

    EXPECT_EQ(table_->array, old_array);
    EXPECT_EQ(table_->free_list, old_free_list);
    EXPECT_EQ(table_->array_size, old_size);
    EXPECT_EQ(table_->next_index, old_next_index);
    EXPECT_EQ(wasm_component_table_get(table_, first_index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              first);

    wasm_destroy_resource_handle(second);

    WASMResourceHandle *third =
        wasm_create_resource_handle(test_resource_type(), 300, true);
    ASSERT_NE(third, nullptr);
    uint32_t third_index = 0;
    EXPECT_TRUE(wasm_component_table_add(
        table_, third, WASM_TABLE_ELEM_RESOURCE_HANDLE, &third_index));
    EXPECT_EQ(third_index, 2u);
}

class ResourceDropTest : public testing::Test
{
  public:
    void SetUp() override
    {
        runtime_initialized_ = wasm_runtime_init();
        ASSERT_TRUE(runtime_initialized_);
        ASSERT_TRUE(instantiate_host_resource_table());
        host_table_ = get_global_host_resource_table();
        ASSERT_NE(host_table_, nullptr);

        memset(&wasi_resource_type_, 0, sizeof(wasi_resource_type_));
        wasi_resource_type_.name = (char *)"test-host-resource";
        wasi_resource_type_.interface_name = (char *)"wasi:test/resource";
        wasi_resource_type_.is_builtin_wasi = true;

        table_ = wasm_component_table_init(4, 100);
        ASSERT_NE(table_, nullptr);
        host_resource_dtor_count.store(0, std::memory_order_relaxed);
    }

    void TearDown() override
    {
        if (table_) {
            wasm_component_table_destroy(table_);
            table_ = nullptr;
        }
        if (get_global_host_resource_table()) {
            destroy_host_resource_table();
        }
        if (runtime_initialized_) {
            wasm_runtime_destroy();
        }
    }

  protected:
    uint32_t addHostResource()
    {
        HostResource *resource =
            host_resource_create(WASI_P2_TCP_SOCKET, sizeof(uint32_t));
        EXPECT_NE(resource, nullptr);
        if (!resource) {
            return 0;
        }

        host_resource_set_dtor(resource, count_host_resource_dtor);
        uint32_t id = host_resource_table_add(host_table_, resource);
        EXPECT_NE(id, 0u);
        if (id == 0) {
            destroy_host_resource(resource);
        }
        return id;
    }

    uint32_t addHandle(uint32_t rep, bool own)
    {
        WASMResourceHandle *handle =
            wasm_create_resource_handle(&wasi_resource_type_, rep, own);
        EXPECT_NE(handle, nullptr);
        if (!handle) {
            return 0;
        }

        uint32_t index = 0;
        if (!wasm_component_table_add(
                table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index)) {
            wasm_destroy_resource_handle(handle);
            ADD_FAILURE() << "failed to add resource handle";
            return 0;
        }
        return index;
    }

    bool runtime_initialized_ = false;
    HostResourceTable *host_table_ = nullptr;
    WASMComponentResourceTable *table_ = nullptr;
    WASMComponentResourceInstance wasi_resource_type_ = {};
};

TEST_F(ResourceDropTest, ExplicitDropDestroysOwnedRepresentationExactlyOnce)
{
    uint32_t rep = addHostResource();
    ASSERT_NE(rep, 0u);
    uint32_t index = addHandle(rep, true);
    ASSERT_NE(index, 0u);

    EXPECT_TRUE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(host_resource_table_get(host_table_, rep), nullptr);

    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(ResourceDropTest, FailedTeardownStillConsumesHandleExactlyOnce)
{
    uint32_t missing_rep = host_resource_table_get_next_id(WASI_P2_TCP_SOCKET);
    ASSERT_NE(missing_rep, 0u);
    uint32_t index = addHandle(missing_rep, true);
    ASSERT_NE(index, 0u);

    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 0u);
}

TEST_F(ResourceDropTest, RemoveTransfersOwnershipWithoutDroppingRepresentation)
{
    uint32_t rep = addHostResource();
    ASSERT_NE(rep, 0u);
    uint32_t index = addHandle(rep, true);
    ASSERT_NE(index, 0u);

    EXPECT_TRUE(wasm_component_table_remove(table_, index));
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 0u);
    EXPECT_NE(host_resource_table_get(host_table_, rep), nullptr);

    EXPECT_EQ(host_resource_table_delete(host_table_, rep), 1u);
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(ResourceDropTest, LiftOwnRejectsDifferentNominalResourceType)
{
    WASMComponentResourceInstance other_resource_type = {};
    WASMComponentResourceHandleInstance expected_handle_type = {};
    WASMComponentInstance component_instance = {};
    LiftLowerContext context = {};
    wit_value_t lifted = nullptr;
    uint32_t rep = addHostResource();
    ASSERT_NE(rep, 0u);
    uint32_t index = addHandle(rep, true);
    ASSERT_NE(index, 0u);

    other_resource_type.name = (char *)"different-host-resource";
    other_resource_type.interface_name = (char *)"wasi:test/other-resource";
    other_resource_type.is_builtin_wasi = true;
    expected_handle_type.resource = &other_resource_type;
    component_instance.table = table_;
    context.inst = &component_instance;

    EXPECT_FALSE(lift_own(&context, index, &expected_handle_type, &lifted));
    EXPECT_EQ(lifted, nullptr);
    EXPECT_NE(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_NE(host_resource_table_get(host_table_, rep), nullptr);

    EXPECT_TRUE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(ResourceDropTest, TableDestroyDropsOwnedButNotBorrowedRepresentations)
{
    uint32_t owned_rep = addHostResource();
    uint32_t borrowed_rep = addHostResource();
    ASSERT_NE(owned_rep, 0u);
    ASSERT_NE(borrowed_rep, 0u);
    ASSERT_NE(addHandle(owned_rep, true), 0u);
    ASSERT_NE(addHandle(borrowed_rep, false), 0u);

    wasm_component_table_destroy(table_);
    table_ = nullptr;

    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(host_resource_table_get(host_table_, owned_rep), nullptr);
    EXPECT_NE(host_resource_table_get(host_table_, borrowed_rep), nullptr);

    EXPECT_EQ(host_resource_table_delete(host_table_, borrowed_rep), 1u);
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 2u);
}

TEST_F(ResourceDropTest, CanonicalBorrowDropUpdatesTaskWithoutDestroyingRep)
{
    uint32_t rep = addHostResource();
    ASSERT_NE(rep, 0u);
    uint32_t index = addHandle(rep, false);
    ASSERT_NE(index, 0u);

    Task task = {};
    task.num_borrows = 1;
    WASMResourceHandle *handle = (WASMResourceHandle *)wasm_component_table_get(
        table_, index, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    ASSERT_NE(handle, nullptr);
    handle->borrow_scope = &task;

    WASMComponentInstance component_instance = {};
    component_instance.table = table_;
    component_instance.may_leave = true;

    EXPECT_TRUE(
        canon_resource_drop(&wasi_resource_type_, &component_instance, index));
    EXPECT_EQ(task.num_borrows, 0u);
    EXPECT_NE(host_resource_table_get(host_table_, rep), nullptr);
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 0u);

    EXPECT_EQ(host_resource_table_delete(host_table_, rep), 1u);
    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
}

TEST_F(ResourceDropTest, ComponentDeinstantiateDropsOwnedResources)
{
    uint32_t rep = addHostResource();
    ASSERT_NE(rep, 0u);
    ASSERT_NE(addHandle(rep, true), 0u);

    WASMComponentInstance *component_instance =
        (WASMComponentInstance *)wasm_runtime_malloc(
            sizeof(WASMComponentInstance));
    ASSERT_NE(component_instance, nullptr);
    memset(component_instance, 0, sizeof(WASMComponentInstance));
    component_instance->table = table_;
    table_ = nullptr;

    wasm_component_deinstantiate(component_instance);

    EXPECT_EQ(host_resource_dtor_count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(host_resource_table_get(host_table_, rep), nullptr);
}

TEST_F(ResourceDropTest, CustomImportedResourceUsesPerInstanceDropCallback)
{
    WASMComponentResourceInstance custom_resource = {};
    custom_resource.name = (char *)"audio-node";
    custom_resource.interface_name =
        (char *)"rebeckerspecialties:web-audio/graph@0.1.0";
    custom_resource.is_host = true;

    WASMComponentTypeInstance resource_type = {};
    resource_type.type = COMPONENT_VAL_TYPE_RESOURCE_SYNC;
    resource_type.type_specific.resource = &custom_resource;
    WASMComponentTypeInstance *types[] = { &resource_type };

    WASMComponentInstance component_instance = {};
    component_instance.types = types;
    component_instance.types_count = 1;
    component_instance.table = table_;
    component_instance.may_leave = true;

    CustomDropObservation observation = {};
    EXPECT_FALSE(wasm_component_set_host_resource_drop_callback(
        &component_instance, "rebeckerspecialties:web-audio/graph@0.2.0",
        "audio-node", observe_custom_resource_drop, &observation));
    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        &component_instance, "rebeckerspecialties:web-audio/graph@0.1.0",
        "audio-node", observe_custom_resource_drop, &observation));

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 0x1234, true);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    EXPECT_TRUE(
        canon_resource_drop(&custom_resource, &component_instance, index));
    EXPECT_EQ(observation.count, 1u);
    EXPECT_EQ(observation.representation, 0x1234u);
    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(observation.count, 1u);
}

TEST_F(ResourceDropTest, PreInstantiationDropRegistrationIsExactAndOwned)
{
    WASMComponent component = {};
    wasm_component_host_resource_drop_callback_t callback = nullptr;

    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        &component, "rebeckerspecialties:web-audio/graph@0.1.0", "audio-node",
        observe_custom_resource_drop));
    EXPECT_FALSE(wasm_component_find_host_resource_drop_callback(
        &component, "rebeckerspecialties:web-audio/graph@0.2.0", "audio-node",
        &callback));
    EXPECT_FALSE(wasm_component_find_host_resource_drop_callback(
        &component, "rebeckerspecialties:web-audio/graph@0.1.0", "gain-node",
        &callback));
    ASSERT_TRUE(wasm_component_find_host_resource_drop_callback(
        &component, "rebeckerspecialties:web-audio/graph@0.1.0", "audio-node",
        &callback));
    EXPECT_EQ(callback, observe_custom_resource_drop);

    wasm_component_free(&component);
    EXPECT_EQ(component.host_resource_drops, nullptr);
    EXPECT_EQ(component.host_resource_drop_count, 0u);
}

TEST_F(ResourceDropTest, ComponentTeardownDropsCustomOwnedResourceExactlyOnce)
{
    WASMComponentResourceInstance custom_resource = {};
    custom_resource.name = (char *)"audio-node";
    custom_resource.interface_name =
        (char *)"rebeckerspecialties:web-audio/graph@0.1.0";
    custom_resource.is_host = true;

    WASMComponentTypeInstance resource_type = {};
    resource_type.type = COMPONENT_VAL_TYPE_RESOURCE_SYNC;
    resource_type.type_specific.resource = &custom_resource;
    WASMComponentTypeInstance *types[] = { &resource_type };

    WASMComponentInstance *component_instance =
        (WASMComponentInstance *)wasm_runtime_malloc(
            sizeof(WASMComponentInstance));
    ASSERT_NE(component_instance, nullptr);
    memset(component_instance, 0, sizeof(*component_instance));
    component_instance->types = types;
    component_instance->types_count = 1;
    component_instance->table = table_;
    table_ = nullptr;

    CustomDropObservation observation = {};
    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        component_instance, "rebeckerspecialties:web-audio/graph@0.1.0",
        "audio-node", observe_custom_resource_drop, &observation));

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 77, true);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(component_instance->table, handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &index));

    wasm_component_deinstantiate(component_instance);
    EXPECT_EQ(observation.count, 1u);
    EXPECT_EQ(observation.representation, 77u);
}

TEST_F(ResourceDropTest, BorrowedCustomResourceNeverInvokesOwnerDrop)
{
    WASMComponentResourceInstance custom_resource = {};
    custom_resource.name = (char *)"audio-node";
    custom_resource.interface_name =
        (char *)"rebeckerspecialties:web-audio/graph@0.1.0";
    custom_resource.is_host = true;

    WASMComponentTypeInstance resource_type = {};
    resource_type.type = COMPONENT_VAL_TYPE_RESOURCE_SYNC;
    resource_type.type_specific.resource = &custom_resource;
    WASMComponentTypeInstance *types[] = { &resource_type };

    WASMComponentInstance component_instance = {};
    component_instance.types = types;
    component_instance.types_count = 1;
    component_instance.table = table_;

    CustomDropObservation observation = {};
    ASSERT_TRUE(wasm_component_set_host_resource_drop_callback(
        &component_instance, "rebeckerspecialties:web-audio/graph@0.1.0",
        "audio-node", observe_custom_resource_drop, &observation));

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 91, false);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    wasm_component_table_destroy(table_);
    table_ = nullptr;
    EXPECT_EQ(observation.count, 0u);
}

TEST_F(ResourceDropTest, CustomImportedResourceWithoutCallbackFailsClosed)
{
    WASMComponentResourceInstance custom_resource = {};
    custom_resource.name = (char *)"gpu-buffer";
    custom_resource.interface_name =
        (char *)"rebeckerspecialties:webgpu-p2/gpu@0.1.0";
    custom_resource.is_host = true;

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 9, true);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
}

// Test edge cases and error conditions
TEST_F(ResourceTableTest, Table_EdgeCases)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);

    // Test adding NULL handle
    uint32_t index;
    bool result = wasm_component_table_add(
        table_, nullptr, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index);
    EXPECT_FALSE(result);

    // Test adding with NULL out_index
    WASMResourceHandle *handle = createTestResource();
    ASSERT_NE(handle, nullptr);
    result = wasm_component_table_add(table_, handle,
                                      WASM_TABLE_ELEM_RESOURCE_HANDLE, nullptr);
    EXPECT_FALSE(result);
    wasm_destroy_resource_handle(handle); // Clean up since add failed

    // Test removing invalid indices
    result = wasm_component_table_remove(table_, 0); // Reserved index
    EXPECT_FALSE(result);

    result = wasm_component_table_remove(table_, 999); // Out of bounds
    EXPECT_FALSE(result);

    // Test getting from invalid indices
    WASMResourceHandle *retrieved =
        (WASMResourceHandle *)wasm_component_table_get(
            table_, 0, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    EXPECT_EQ(retrieved, nullptr);

    retrieved = (WASMResourceHandle *)wasm_component_table_get(
        table_, 999, WASM_TABLE_ELEM_RESOURCE_HANDLE);
    EXPECT_EQ(retrieved, nullptr);

    // Test initialization with invalid parameters
    WASMComponentResourceTable *bad_table = wasm_component_table_init(0, 50);
    EXPECT_EQ(bad_table, nullptr);

    bad_table = wasm_component_table_init(4, 0);
    EXPECT_EQ(bad_table, nullptr);
}
