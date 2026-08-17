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
#include <limits>
#include <string>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

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
#include "wasm_allocation_quota.h"
#include "wasm_export.h"
}

namespace {

int allocation_fail_after = -1;
std::atomic<uint32_t> host_resource_dtor_count{ 0 };
std::atomic<uint32_t> pipe_resource_dtor_count{ 0 };

struct TransactionQuotaState {
    uint64_t live_bytes = 0;
    uint64_t byte_limit = std::numeric_limits<uint64_t>::max();
    uint32_t live_allocations = 0;
    uint32_t allocation_limit = std::numeric_limits<uint32_t>::max();
    uint32_t attachment_retain_calls = 0;
    uint32_t attachment_release_calls = 0;
};

bool
transaction_quota_reserve(void *attachment, uint64_t bytes,
                          uint32_t allocations)
{
    auto *state = static_cast<TransactionQuotaState *>(attachment);

    if (!state || state->live_bytes > state->byte_limit
        || bytes > state->byte_limit - state->live_bytes
        || state->live_allocations > state->allocation_limit
        || allocations > state->allocation_limit - state->live_allocations) {
        return false;
    }
    state->live_bytes += bytes;
    state->live_allocations += allocations;
    return true;
}

void
transaction_quota_release(void *attachment, uint64_t bytes,
                          uint32_t allocations)
{
    auto *state = static_cast<TransactionQuotaState *>(attachment);

    if (!state || bytes > state->live_bytes
        || allocations > state->live_allocations) {
        return;
    }
    state->live_bytes -= bytes;
    state->live_allocations -= allocations;
}

bool
transaction_quota_attachment_retain(void *attachment)
{
    auto *state = static_cast<TransactionQuotaState *>(attachment);

    if (!state)
        return false;
    state->attachment_retain_calls++;
    return true;
}

void
transaction_quota_attachment_release(void *attachment)
{
    auto *state = static_cast<TransactionQuotaState *>(attachment);

    if (state)
        state->attachment_release_calls++;
}

class TransactionQuotaScope
{
  public:
    explicit TransactionQuotaScope(TransactionQuotaState *state)
      : state_(state)
    {
        LoadArgs2 args = {};

        wasm_runtime_load_args2_init(&args);
        wasm_runtime_load_args2_set_allocation_quota(
            &args, state_, transaction_quota_reserve, transaction_quota_release,
            transaction_quota_attachment_retain,
            transaction_quota_attachment_release);
        token_ = wasm_allocation_quota_token_create(&args);
        if (token_)
            previous_ = wasm_allocation_quota_set_current(token_);
    }

    ~TransactionQuotaScope()
    {
        if (value) {
            free_wit_value(value);
            value = nullptr;
        }
        if (table) {
            (void)wasm_component_table_destroy(table);
            table = nullptr;
        }
        if (token_) {
            (void)wasm_allocation_quota_set_current(previous_);
            wasm_allocation_quota_token_release(token_);
            token_ = nullptr;
        }
    }

    bool active() const { return token_ != nullptr; }

    WASMComponentResourceTable *table = nullptr;
    wit_value_t value = nullptr;

  private:
    TransactionQuotaState *state_ = nullptr;
    WASMAllocationQuotaToken *token_ = nullptr;
    WASMAllocationQuotaToken *previous_ = nullptr;
};

#if !defined(_WIN32)
void
close_pipe_host_resource(void *data)
{
    int fd = data ? *static_cast<int *>(data) : -1;

    if (fd >= 0)
        (void)close(fd);
    pipe_resource_dtor_count.fetch_add(1, std::memory_order_relaxed);
}
#endif

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

TEST(WaveResourcePolicyTest, RejectsNestedResourceValuesBeforeInvocation)
{
    WASMComponentResourceInstance resource{};
    WASMComponentResourceHandleInstance own_handle{ &resource, false };
    WASMComponentTypeInstance own_type{};
    WASMComponentListInstance list{};
    WASMComponentTypeInstance list_type{};
    WASMComponentResultInstance result{};
    WASMComponentTypeInstance result_type{};
    WASMComponentTypeInstance u32_type{};

    own_type.type = COMPONENT_VAL_TYPE_OWN;
    own_type.type_specific.resource_handle = &own_handle;
    list.element_type = &own_type;
    list_type.type = COMPONENT_VAL_TYPE_LIST;
    list_type.type_specific.list = &list;
    result.result_type = &list_type;
    result.error_type = &u32_type;
    result_type.type = COMPONENT_VAL_TYPE_RESULT;
    result_type.type_specific.result = &result;
    u32_type.type = COMPONENT_VAL_TYPE_PRIMVAL;
    u32_type.type_specific.primval = WASM_COMP_PRIMVAL_U32;

    EXPECT_FALSE(wasm_component_application_wave_type_supported(&result_type));
    EXPECT_TRUE(wasm_component_application_wave_type_supported(&u32_type));
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

struct ReentrantTableAddObservation {
    WASMComponentResourceTable *table;
    bool attempted;
    bool succeeded;
};

struct SiblingCoreDropObservation {
    WASMComponentInstance *sibling;
    uint32_t count;
    bool sibling_core_was_alive;
};

bool
observe_sibling_core_alive(void *attachment, uint32_t representation)
{
    auto *observation = static_cast<SiblingCoreDropObservation *>(attachment);

    (void)representation;
    if (!observation)
        return false;
    observation->count++;
    observation->sibling_core_was_alive =
        observation->sibling
        && observation->sibling->defined_core_instances_count == 1
        && observation->sibling->defined_core_instances
        && observation->sibling->defined_core_instances[0] != nullptr;
    return observation->sibling_core_was_alive;
}

bool
try_reentrant_table_add(void *attachment, uint32_t representation)
{
    auto *observation = static_cast<ReentrantTableAddObservation *>(attachment);
    WASMResourceHandle *handle;
    uint32_t index = 0;

    if (!observation) {
        return false;
    }
    observation->attempted = true;
    handle = wasm_create_resource_handle(test_resource_type(),
                                         representation + 1, true);
    if (!handle) {
        return false;
    }
    observation->succeeded = wasm_component_table_add(
        observation->table, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index);
    if (!observation->succeeded) {
        wasm_destroy_resource_handle(handle);
    }
    return true;
}

enum class CanonicalCompositeKind {
    List,
    Record,
    Tuple,
    Variant,
    Option,
    Result,
};

const char *
canonical_composite_name(CanonicalCompositeKind kind)
{
    switch (kind) {
        case CanonicalCompositeKind::List:
            return "list";
        case CanonicalCompositeKind::Record:
            return "record";
        case CanonicalCompositeKind::Tuple:
            return "tuple";
        case CanonicalCompositeKind::Variant:
            return "variant";
        case CanonicalCompositeKind::Option:
            return "option";
        case CanonicalCompositeKind::Result:
            return "result";
    }
    return "unknown";
}

struct TwoFieldRecordInstance {
    uint32_t count;
    WASMComponentLabelValTypeInstance fields[2];
};

struct OneCaseVariantInstance {
    uint32_t count;
    WASMComponentCaseValInstance cases[1];
};

class CanonicalCompositeCase
{
  public:
    explicit CanonicalCompositeCase(CanonicalCompositeKind kind)
      : kind_(kind)
    {
        first_label_.name = const_cast<char *>("first");
        first_label_.name_len = 5;
        second_label_.name = const_cast<char *>("second");
        second_label_.name_len = 6;
        payload_label_.name = const_cast<char *>("payload");
        payload_label_.name_len = 7;

        handle_type_.resource = test_resource_type();
        own_type_.type = COMPONENT_VAL_TYPE_OWN;
        own_type_.alignment = 4;
        own_type_.elem_size = 4;
        own_type_.type_specific.resource_handle = &handle_type_;

        pair_types_[0] = &own_type_;
        pair_types_[1] = &own_type_;
        pair_tuple_.count = 2;
        pair_tuple_.element_types = pair_types_;
        pair_tuple_type_.type = COMPONENT_VAL_TYPE_TUPLE;
        pair_tuple_type_.alignment = 4;
        pair_tuple_type_.elem_size = 8;
        pair_tuple_type_.type_specific.tuple = &pair_tuple_;

        switch (kind_) {
            case CanonicalCompositeKind::List:
                list_.element_type = &own_type_;
                outer_type_.type = COMPONENT_VAL_TYPE_LIST;
                outer_type_.type_specific.list = &list_;
                fixed_list_.len = 2;
                fixed_list_.element_type = &own_type_;
                lower_type_.type = COMPONENT_VAL_TYPE_FIXED_SIZE_LIST;
                lower_type_.type_specific.list_len = &fixed_list_;
                break;
            case CanonicalCompositeKind::Record:
                record_.count = 2;
                record_.fields[0].label = &first_label_;
                record_.fields[0].type = &own_type_;
                record_.fields[1].label = &second_label_;
                record_.fields[1].type = &own_type_;
                outer_type_.type = COMPONENT_VAL_TYPE_RECORD;
                outer_type_.type_specific.record =
                    reinterpret_cast<WASMComponentRecordInstance *>(&record_);
                break;
            case CanonicalCompositeKind::Tuple:
                outer_type_ = pair_tuple_type_;
                break;
            case CanonicalCompositeKind::Variant:
                variant_.count = 1;
                variant_.cases[0].label = &payload_label_;
                variant_.cases[0].value_type = &pair_tuple_type_;
                outer_type_.type = COMPONENT_VAL_TYPE_VARIANT;
                outer_type_.type_specific.variant =
                    reinterpret_cast<WASMComponentVariantInstance *>(&variant_);
                break;
            case CanonicalCompositeKind::Option:
                option_.element_type = &pair_tuple_type_;
                outer_type_.type = COMPONENT_VAL_TYPE_OPTION;
                outer_type_.type_specific.option = &option_;
                break;
            case CanonicalCompositeKind::Result:
                result_.result_type = &pair_tuple_type_;
                result_.error_type = nullptr;
                outer_type_.type = COMPONENT_VAL_TYPE_RESULT;
                outer_type_.type_specific.result = &result_;
                break;
        }
        if (kind_ != CanonicalCompositeKind::List)
            lower_type_ = outer_type_;
    }

    WASMComponentResourceInstance *resource() { return test_resource_type(); }
    WASMComponentTypeInstance *type() { return &outer_type_; }
    WASMComponentTypeInstance *lower_type() { return &lower_type_; }

    uint32_t make_flat_values(uint32_t first_index, uint32_t second_index,
                              CoreValue values[3], uint8_t *memory) const
    {
        uint32_t offset = 0;

        if (kind_ == CanonicalCompositeKind::List) {
            constexpr uint32_t begin = 4;

            memcpy(memory + begin, &first_index, sizeof(first_index));
            memcpy(memory + begin + sizeof(first_index), &second_index,
                   sizeof(second_index));
            values[0].type = CORE_TYPE_I32;
            values[0].val.i32 = begin;
            values[1].type = CORE_TYPE_I32;
            values[1].val.i32 = 2;
            return 2;
        }
        if (kind_ == CanonicalCompositeKind::Variant
            || kind_ == CanonicalCompositeKind::Option
            || kind_ == CanonicalCompositeKind::Result) {
            values[0].type = CORE_TYPE_I32;
            values[0].val.i32 = kind_ == CanonicalCompositeKind::Option ? 1 : 0;
            offset = 1;
        }
        values[offset].type = CORE_TYPE_I32;
        values[offset].val.i32 = first_index;
        values[offset + 1].type = CORE_TYPE_I32;
        values[offset + 1].val.i32 = second_index;
        return offset + 2;
    }

    wit_value_t make_owned_value(uint32_t first_rep, uint32_t second_rep) const
    {
        wit_value_t first = wit_resource_ctor(first_rep);
        wit_value_t second = wit_resource_ctor(second_rep);

        if (!first || !second) {
            free_wit_value(first);
            free_wit_value(second);
            return nullptr;
        }

        if (kind_ == CanonicalCompositeKind::Record) {
            auto *fields = static_cast<ComponentWITRecordField *>(
                wasm_runtime_calloc(2, sizeof(ComponentWITRecordField)));
            if (!fields) {
                free_wit_value(first);
                free_wit_value(second);
                return nullptr;
            }
            init_record_field(&fields[0], first_label_.name,
                              first_label_.name_len, first);
            init_record_field(&fields[1], second_label_.name,
                              second_label_.name_len, second);
            if (!fields[0].key || !fields[1].key) {
                free_wit_value(first);
                free_wit_value(second);
                wasm_runtime_free(fields[0].key);
                wasm_runtime_free(fields[1].key);
                wasm_runtime_free(fields);
                return nullptr;
            }
            return wit_record_ctor(fields, 2);
        }

        auto *elements = static_cast<wit_value_t *>(
            wasm_runtime_malloc(2 * sizeof(wit_value_t)));
        if (!elements) {
            free_wit_value(first);
            free_wit_value(second);
            return nullptr;
        }
        elements[0] = first;
        elements[1] = second;

        if (kind_ == CanonicalCompositeKind::List)
            return wit_list_ctor(elements, 2);
        if (kind_ == CanonicalCompositeKind::Tuple)
            return wit_tuple_ctor(elements, 2);

        wit_value_t pair = wit_tuple_ctor(elements, 2);
        if (!pair) {
            free_wit_value(first);
            free_wit_value(second);
            wasm_runtime_free(elements);
            return nullptr;
        }
        if (kind_ == CanonicalCompositeKind::Variant)
            return wit_variant_ctor(payload_label_.name,
                                    payload_label_.name_len, pair);
        if (kind_ == CanonicalCompositeKind::Option)
            return wit_option_ctor(pair);
        return wit_result_ctor(false, pair);
    }

  private:
    CanonicalCompositeKind kind_;
    WASMComponentResourceHandleInstance handle_type_ = {};
    WASMComponentTypeInstance own_type_ = {};
    WASMComponentTypeInstance outer_type_ = {};
    WASMComponentTypeInstance pair_tuple_type_ = {};
    WASMComponentTypeInstance *pair_types_[2] = {};
    WASMComponentTupleInstance pair_tuple_ = {};
    WASMComponentListInstance list_ = {};
    WASMComponentListLenInstance fixed_list_ = {};
    WASMComponentTypeInstance lower_type_ = {};
    TwoFieldRecordInstance record_ = {};
    OneCaseVariantInstance variant_ = {};
    WASMComponentOptionInstance option_ = {};
    WASMComponentResultInstance result_ = {};
    WASMComponentCoreName first_label_ = {};
    WASMComponentCoreName second_label_ = {};
    WASMComponentCoreName payload_label_ = {};
};

constexpr CanonicalCompositeKind canonical_composite_kinds[] = {
    CanonicalCompositeKind::List,   CanonicalCompositeKind::Record,
    CanonicalCompositeKind::Tuple,  CanonicalCompositeKind::Variant,
    CanonicalCompositeKind::Option, CanonicalCompositeKind::Result,
};

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

TEST_F(ResourceTableTest, LiftTransactionRollbackRestoresExactHandle)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMResourceHandle *handle = createTestResource(101, true);
    uint32_t index = 0;
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LIFT));
    ASSERT_TRUE(
        wasm_component_table_transaction_reserve_lift(&transaction, index));
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_TRUE(wasm_component_table_transaction_can_commit(&transaction));
    ASSERT_TRUE(wasm_component_table_transaction_rollback(&transaction));

    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              handle);
    EXPECT_EQ(handle->rep, 101u);
    EXPECT_TRUE(handle->own);
    EXPECT_EQ(table_->free_count, 0u);
}

TEST_F(ResourceTableTest,
       LiftTransactionRejectsDuplicateIndexAndRestoresOriginalSlot)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMResourceHandle *handle = createTestResource(111, true);
    uint32_t index = 0;
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));
    ASSERT_EQ(index, 1u);

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LIFT));
    ASSERT_TRUE(
        wasm_component_table_transaction_reserve_lift(&transaction, index));
    EXPECT_FALSE(
        wasm_component_table_transaction_reserve_lift(&transaction, index));
    EXPECT_EQ(transaction.count, 1u);
    EXPECT_TRUE(wasm_component_table_transaction_can_commit(&transaction));
    ASSERT_TRUE(wasm_component_table_transaction_rollback(&transaction));

    EXPECT_EQ(table_->array[index]->ptr, handle);
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              handle);
    EXPECT_EQ(handle->rep, 111u);
    EXPECT_TRUE(handle->own);
    EXPECT_EQ(table_->free_count, 0u);
    EXPECT_EQ(table_->next_index, 2u);
}

TEST_F(ResourceTableTest, LowerTransactionRollbackRemovesEveryTentativeEntry)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMComponentInstance instance = {};
    instance.table = table_;
    Task task = {};
    task.inst = &instance;

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LOWER));

    WASMResourceHandle *owned = createTestResource(201, true);
    WASMResourceHandle *borrowed = createTestResource(202, false);
    uint32_t own_index = 0;
    uint32_t borrow_index = 0;
    uint32_t error_index = 0;
    uint32_t error_context = 7;
    ASSERT_NE(owned, nullptr);
    ASSERT_NE(borrowed, nullptr);
    ASSERT_TRUE(wasm_component_table_add(
        table_, owned, WASM_TABLE_ELEM_RESOURCE_HANDLE, &own_index));
    ASSERT_TRUE(
        wasm_component_table_transaction_record_lower(&transaction, own_index));
    ASSERT_TRUE(wasm_component_table_add(
        table_, borrowed, WASM_TABLE_ELEM_RESOURCE_HANDLE, &borrow_index));
    ASSERT_TRUE(wasm_component_table_transaction_record_lower(&transaction,
                                                              borrow_index));
    borrowed->borrow_scope = &task;
    task.num_borrows = 1;
    ASSERT_TRUE(wasm_component_table_add(
        table_, &error_context, WASM_TABLE_ELEM_ERROR_CONTEXT, &error_index));
    ASSERT_TRUE(wasm_component_table_transaction_record_lower(&transaction,
                                                              error_index));

    EXPECT_TRUE(wasm_component_table_transaction_can_commit(&transaction));
    ASSERT_TRUE(wasm_component_table_transaction_rollback(&transaction));
    EXPECT_EQ(task.num_borrows, 0u);
    EXPECT_EQ(wasm_component_table_get(table_, own_index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_EQ(wasm_component_table_get(table_, borrow_index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_EQ(wasm_component_table_get(table_, error_index,
                                       WASM_TABLE_ELEM_ERROR_CONTEXT),
              nullptr);
    EXPECT_EQ(table_->free_count, 3u);
}

TEST_F(ResourceTableTest,
       LowerTransactionRollbackRestoresAggregateBorrowTaskBaseline)
{
    table_ = wasm_component_table_init(8, 50);
    ASSERT_NE(table_, nullptr);
    WASMComponentInstance instance = {};
    instance.table = table_;
    Task task = {};
    task.inst = &instance;
    constexpr uint32_t baseline = 7;
    constexpr uint32_t transaction_borrows = 3;
    task.num_borrows = baseline;

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LOWER));

    uint32_t indices[transaction_borrows] = {};
    for (uint32_t i = 0; i < transaction_borrows; i++) {
        WASMResourceHandle *borrowed = createTestResource(220 + i, false);
        ASSERT_NE(borrowed, nullptr);
        ASSERT_TRUE(wasm_component_table_add(
            table_, borrowed, WASM_TABLE_ELEM_RESOURCE_HANDLE, &indices[i]));
        ASSERT_TRUE(wasm_component_table_transaction_record_lower(&transaction,
                                                                  indices[i]));
        borrowed->borrow_scope = &task;
        task.num_borrows++;
    }

    EXPECT_EQ(task.num_borrows, baseline + transaction_borrows);
    ASSERT_TRUE(wasm_component_table_transaction_rollback(&transaction));
    EXPECT_EQ(task.num_borrows, baseline);
    for (uint32_t index : indices) {
        EXPECT_EQ(wasm_component_table_get(table_, index,
                                           WASM_TABLE_ELEM_RESOURCE_HANDLE),
                  nullptr);
    }
    EXPECT_EQ(table_->free_count, transaction_borrows);
}

TEST_F(ResourceTableTest, LowerTransactionRebindsOnlySoleOwnedPlaceholder)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMResourceHandle *placeholder =
        createTestResource(std::numeric_limits<uint32_t>::max(), true);
    ASSERT_NE(placeholder, nullptr);
    uint32_t index = 0;

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LOWER));
    ASSERT_TRUE(wasm_component_table_add(
        table_, placeholder, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));
    ASSERT_TRUE(
        wasm_component_table_transaction_record_lower(&transaction, index));

    EXPECT_FALSE(wasm_component_table_transaction_rebind_single_owned_rep(
        &transaction, 1, 919));
    ASSERT_TRUE(wasm_component_table_transaction_rebind_single_owned_rep(
        &transaction, std::numeric_limits<uint32_t>::max(), 919));
    EXPECT_EQ(placeholder->rep, 919u);
    EXPECT_TRUE(wasm_component_table_transaction_can_commit(&transaction));
    ASSERT_TRUE(wasm_component_table_transaction_commit(&transaction));
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              placeholder);
}

TEST_F(ResourceTableTest, FailedResultDiscardDropsRepresentationExactlyOnce)
{
    CustomDropObservation observation = {};
    WASMComponentResourceInstance resource = {};
    resource.is_host = true;
    resource.host_drop_callback = observe_custom_resource_drop;
    resource.host_drop_attachment = &observation;
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMResourceHandle *handle =
        wasm_create_resource_handle(&resource, 303, true);
    uint32_t index = 0;
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LIFT));
    ASSERT_TRUE(
        wasm_component_table_transaction_reserve_lift(&transaction, index));
    ASSERT_TRUE(wasm_component_table_transaction_discard_lift(&transaction));

    EXPECT_EQ(observation.count, 1u);
    EXPECT_EQ(observation.representation, 303u);
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    EXPECT_FALSE(wasm_component_table_drop_resource(table_, index));
    EXPECT_EQ(observation.count, 1u);
}

TEST_F(ResourceTableTest, TaskAndSubtaskDestroyUnwindBorrowBookkeeping)
{
    table_ = wasm_component_table_init(4, 50);
    ASSERT_NE(table_, nullptr);
    WASMComponentInstance instance = {};
    instance.table = table_;
    Task *task = task_create(nullptr, &instance, nullptr, nullptr);
    Subtask *subtask = subtask_create();
    WASMResourceHandle *borrowed = createTestResource(401, false);
    WASMResourceHandle *lender = createTestResource(402, true);
    uint32_t index = 0;
    ASSERT_NE(task, nullptr);
    ASSERT_NE(subtask, nullptr);
    ASSERT_NE(borrowed, nullptr);
    ASSERT_NE(lender, nullptr);
    ASSERT_TRUE(wasm_component_table_add(
        table_, borrowed, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));
    borrowed->borrow_scope = task;
    task->num_borrows = 1;
    ASSERT_TRUE(subtask_add_lender(subtask, lender));
    ASSERT_EQ(lender->num_lends, 1u);

    task_destroy(task);
    EXPECT_EQ(wasm_component_table_get(table_, index,
                                       WASM_TABLE_ELEM_RESOURCE_HANDLE),
              nullptr);
    subtask_destroy(subtask);
    EXPECT_EQ(lender->num_lends, 0u);
    wasm_destroy_resource_handle(lender);
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

TEST_F(ResourceTableAllocationFailureTest,
       CompositeOwnLiftLateDenialsRestoreEveryExactSourceSlot)
{
    for (CanonicalCompositeKind kind : canonical_composite_kinds) {
        SCOPED_TRACE(canonical_composite_name(kind));
        CanonicalCompositeCase composite(kind);
        table_ = wasm_component_table_init(8, 50);
        ASSERT_NE(table_, nullptr);

        WASMResourceHandle *first =
            wasm_create_resource_handle(composite.resource(), 501, true);
        WASMResourceHandle *second =
            wasm_create_resource_handle(composite.resource(), 502, true);
        uint32_t first_index = 0;
        uint32_t second_index = 0;
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        ASSERT_TRUE(wasm_component_table_add(
            table_, first, WASM_TABLE_ELEM_RESOURCE_HANDLE, &first_index));
        ASSERT_TRUE(wasm_component_table_add(
            table_, second, WASM_TABLE_ELEM_RESOURCE_HANDLE, &second_index));
        ASSERT_EQ(first_index, 1u);
        ASSERT_EQ(second_index, 2u);

        WASMComponentInstance instance = {};
        instance.table = table_;
        uint8_t memory_bytes[16] = {};
        WASMMemoryInstance memory = {};
        memory.memory_data = memory_bytes;
        memory.memory_data_end = memory_bytes + sizeof(memory_bytes);
        memory.memory_data_size = sizeof(memory_bytes);
        LiftOptions lift_options = {};
        lift_options.memory = &memory;
        LiftLowerOptions lift_lower_options = {};
        lift_lower_options.lift_opts = &lift_options;
        CanonicalOptions canonical_options = {};
        canonical_options.lift_lower_opts = &lift_lower_options;
        LiftLowerContext context = {};
        context.canonical_opts = &canonical_options;
        context.inst = &instance;
        CoreValue flat_values[3] = {};
        uint32_t flat_count = composite.make_flat_values(
            first_index, second_index, flat_values, memory_bytes);
        CoreValueIter iter;
        vi_init(&iter, flat_values, flat_count);
        wit_value_t lifted = nullptr;
        uint32_t baseline_free_count = table_->free_count;
        uint32_t baseline_next_index = table_->next_index;

        /* Each shape allocates its outer storage and first resource value
           before this deterministic denial. The active lift transaction must
           restore the exact source pointers and their original indices. */
        allocation_fail_after = 2;
        EXPECT_FALSE(lift_flat(&context, &iter, composite.type(), &lifted));
        allocation_fail_after = -1;
        EXPECT_EQ(lifted, nullptr);
        ASSERT_NE(table_->array[first_index], nullptr);
        ASSERT_NE(table_->array[second_index], nullptr);
        EXPECT_EQ(table_->array[first_index]->ptr, first);
        EXPECT_EQ(table_->array[second_index]->ptr, second);
        EXPECT_EQ(wasm_component_table_get(table_, first_index,
                                           WASM_TABLE_ELEM_RESOURCE_HANDLE),
                  first);
        EXPECT_EQ(wasm_component_table_get(table_, second_index,
                                           WASM_TABLE_ELEM_RESOURCE_HANDLE),
                  second);
        EXPECT_EQ(first->rep, 501u);
        EXPECT_EQ(second->rep, 502u);
        EXPECT_TRUE(first->own);
        EXPECT_TRUE(second->own);
        EXPECT_EQ(table_->free_count, baseline_free_count);
        EXPECT_EQ(table_->next_index, baseline_next_index);

        ASSERT_TRUE(wasm_component_table_destroy(table_));
        table_ = nullptr;
    }
}

TEST_F(ResourceTableAllocationFailureTest,
       CompositeOwnLowerQuotaDenialsLeaveDestinationAndQuotaAtBaseline)
{
    for (CanonicalCompositeKind kind : canonical_composite_kinds) {
        SCOPED_TRACE(canonical_composite_name(kind));
        CanonicalCompositeCase composite(kind);
        TransactionQuotaState quota;

        {
            TransactionQuotaScope quota_scope(&quota);
            ASSERT_TRUE(quota_scope.active());
            quota_scope.table = wasm_component_table_init(8, 50);
            ASSERT_NE(quota_scope.table, nullptr);

            WASMResourceHandle *sentinel =
                wasm_create_resource_handle(composite.resource(), 700, true);
            uint32_t sentinel_index = 0;
            ASSERT_NE(sentinel, nullptr);
            ASSERT_TRUE(wasm_component_table_add(
                quota_scope.table, sentinel, WASM_TABLE_ELEM_RESOURCE_HANDLE,
                &sentinel_index));
            ASSERT_EQ(sentinel_index, 1u);

            quota_scope.value = composite.make_owned_value(601, 602);
            ASSERT_NE(quota_scope.value, nullptr);
            uint64_t baseline_bytes = quota.live_bytes;
            uint32_t baseline_allocations = quota.live_allocations;
            uint32_t baseline_free_count = quota_scope.table->free_count;

            /* One handle and its table element succeed. The next owned
               handle is denied by the real allocation quota, after the first
               destination slot has been recorded in the lower transaction. */
            ASSERT_LE(baseline_allocations,
                      std::numeric_limits<uint32_t>::max() - 2);
            quota.allocation_limit = baseline_allocations + 2;

            WASMComponentInstance instance = {};
            instance.table = quota_scope.table;
            LiftLowerContext context = {};
            context.inst = &instance;
            CoreValueList lowered;
            cvl_init(&lowered);
            EXPECT_FALSE(lower_flat(&context, composite.lower_type(), &lowered,
                                    quota_scope.value));
            EXPECT_EQ(lowered.count, 0u);
            EXPECT_EQ(quota.live_bytes, baseline_bytes);
            EXPECT_EQ(quota.live_allocations, baseline_allocations);
            EXPECT_EQ(wasm_component_table_get(quota_scope.table,
                                               sentinel_index,
                                               WASM_TABLE_ELEM_RESOURCE_HANDLE),
                      sentinel);
            EXPECT_EQ(sentinel->rep, 700u);
            EXPECT_EQ(quota_scope.table->free_count, baseline_free_count + 1);
            for (uint32_t index = sentinel_index + 1;
                 index < quota_scope.table->next_index; index++) {
                EXPECT_EQ(quota_scope.table->array[index], nullptr);
            }

            quota.allocation_limit = std::numeric_limits<uint32_t>::max();
        }

        EXPECT_EQ(quota.live_bytes, 0u);
        EXPECT_EQ(quota.live_allocations, 0u);
        EXPECT_EQ(quota.attachment_retain_calls, 1u);
        EXPECT_EQ(quota.attachment_release_calls, 1u);
    }
}

TEST_F(ResourceTableTest, DestroySealsTableAgainstDestructorReentry)
{
    WASMComponentResourceInstance custom_resource = {};
    ReentrantTableAddObservation observation = {};

    table_ = wasm_component_table_init(2, 100);
    ASSERT_NE(table_, nullptr);
    observation.table = table_;
    custom_resource.name = (char *)"reentrant-resource";
    custom_resource.interface_name = (char *)"test:reentrant/resources";
    custom_resource.is_host = true;
    custom_resource.host_drop_callback = try_reentrant_table_add;
    custom_resource.host_drop_attachment = &observation;

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 41, true);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        table_, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    wasm_component_table_destroy(table_);
    table_ = nullptr;
    EXPECT_TRUE(observation.attempted);
    EXPECT_FALSE(observation.succeeded);
}

TEST_F(ResourceTableTest,
       ComponentDeinstantiateSealsNestedTablesBeforeCallbacks)
{
    WASMComponentIndexCount root_counts = {};
    WASMComponentIndexCount child_counts = {};
    WASMComponentResourceInstance custom_resource = {};
    ReentrantTableAddObservation observation = {};

    root_counts.instances = 1;
    root_counts.defined_instances = 1;
    WASMComponentInstance *root =
        wasm_component_instance_allocate(&root_counts, nullptr, 0);
    WASMComponentInstance *child =
        wasm_component_instance_allocate(&child_counts, nullptr, 0);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);
    child->parent = root;
    root->component_instances[0] = child;
    root->component_instances_count = 1;
    root->defined_instances[0] = child;
    root->defined_instances_count = 1;

    observation.table = child->table;
    custom_resource.name = (char *)"nested-reentrant-resource";
    custom_resource.interface_name = (char *)"test:reentrant/resources";
    custom_resource.is_host = true;
    custom_resource.host_drop_callback = try_reentrant_table_add;
    custom_resource.host_drop_attachment = &observation;

    WASMResourceHandle *handle =
        wasm_create_resource_handle(&custom_resource, 51, true);
    ASSERT_NE(handle, nullptr);
    uint32_t index = 0;
    ASSERT_TRUE(wasm_component_table_add(
        root->table, handle, WASM_TABLE_ELEM_RESOURCE_HANDLE, &index));

    wasm_component_deinstantiate(root);
    EXPECT_TRUE(observation.attempted);
    EXPECT_FALSE(observation.succeeded);
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

#if !defined(_WIN32)
TEST_F(ResourceDropTest, DiscardedLiftClosesPipeHostResourceExactlyOnce)
{
    int pipe_fds[2] = { -1, -1 };
    ASSERT_EQ(pipe(pipe_fds), 0);
    pipe_resource_dtor_count.store(0, std::memory_order_relaxed);

    HostResource *resource =
        host_resource_create(WASI_P2_IO_INPUT_STREAM, sizeof(int));
    ASSERT_NE(resource, nullptr);
    *static_cast<int *>(resource->data) = pipe_fds[0];
    host_resource_set_dtor(resource, close_pipe_host_resource);
    uint32_t rep = host_resource_table_add(host_table_, resource);
    ASSERT_NE(rep, 0u);
    uint32_t index = addHandle(rep, true);
    ASSERT_NE(index, 0u);

    WASMComponentTableTransaction transaction = {};
    ASSERT_TRUE(wasm_component_table_transaction_begin(
        &transaction, table_, WASM_COMPONENT_TABLE_TRANSACTION_LIFT));
    ASSERT_TRUE(
        wasm_component_table_transaction_reserve_lift(&transaction, index));
    ASSERT_TRUE(wasm_component_table_transaction_discard_lift(&transaction));

    errno = 0;
    EXPECT_EQ(fcntl(pipe_fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(pipe_resource_dtor_count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(host_resource_table_get(host_table_, rep), nullptr);
    EXPECT_FALSE(wasm_component_table_transaction_discard_lift(&transaction));
    EXPECT_EQ(pipe_resource_dtor_count.load(std::memory_order_relaxed), 1u);

    EXPECT_EQ(close(pipe_fds[1]), 0);
}
#endif

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

    WASMComponentIndexCount index_count = {};
    WASMComponentInstance *component_instance =
        wasm_component_instance_allocate(&index_count, nullptr, 0);
    ASSERT_NE(component_instance, nullptr);
    wasm_component_table_destroy(component_instance->table);
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
    WASMComponent *component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    wasm_component_host_resource_drop_callback_t callback = nullptr;
    ASSERT_NE(component, nullptr);
    memset(component, 0, sizeof(*component));

    ASSERT_TRUE(wasm_component_register_host_resource_drop_callback(
        component, "rebeckerspecialties:web-audio/graph@0.1.0", "audio-node",
        observe_custom_resource_drop));
    EXPECT_FALSE(wasm_component_find_host_resource_drop_callback(
        component, "rebeckerspecialties:web-audio/graph@0.2.0", "audio-node",
        &callback));
    EXPECT_FALSE(wasm_component_find_host_resource_drop_callback(
        component, "rebeckerspecialties:web-audio/graph@0.1.0", "gain-node",
        &callback));
    ASSERT_TRUE(wasm_component_find_host_resource_drop_callback(
        component, "rebeckerspecialties:web-audio/graph@0.1.0", "audio-node",
        &callback));
    EXPECT_EQ(callback, observe_custom_resource_drop);

    wasm_component_free(component);
    EXPECT_EQ(component->host_resource_drops, nullptr);
    EXPECT_EQ(component->host_resource_drop_count, 0u);
    wasm_runtime_free(component);
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

    WASMComponentIndexCount index_count = {};
    WASMComponentInstance *component_instance =
        wasm_component_instance_allocate(&index_count, nullptr, 0);
    ASSERT_NE(component_instance, nullptr);
    wasm_component_table_destroy(component_instance->table);
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

TEST_F(ResourceDropTest, ComponentTeardownDrainsAllSiblingTablesBeforeCores)
{
    WASMComponentIndexCount root_counts = {};
    WASMComponentIndexCount child_counts = {};
    root_counts.defined_instances = 2;
    child_counts.defined_core_instances = 1;

    WASMComponentInstance *root =
        wasm_component_instance_allocate(&root_counts, nullptr, 0);
    WASMComponentInstance *left =
        wasm_component_instance_allocate(&child_counts, nullptr, 0);
    WASMComponentInstance *right =
        wasm_component_instance_allocate(&child_counts, nullptr, 0);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    left->defined_core_instances[0] = static_cast<WASMModuleInstance *>(
        wasm_runtime_malloc(sizeof(WASMModuleInstance)));
    right->defined_core_instances[0] = static_cast<WASMModuleInstance *>(
        wasm_runtime_malloc(sizeof(WASMModuleInstance)));
    ASSERT_NE(left->defined_core_instances[0], nullptr);
    ASSERT_NE(right->defined_core_instances[0], nullptr);
    memset(left->defined_core_instances[0], 0, sizeof(WASMModuleInstance));
    memset(right->defined_core_instances[0], 0, sizeof(WASMModuleInstance));
    left->defined_core_instances_count = 1;
    right->defined_core_instances_count = 1;
    left->parent = root;
    right->parent = root;
    root->defined_instances[0] = left;
    root->defined_instances[1] = right;
    root->defined_instances_count = 2;

    SiblingCoreDropObservation left_observation = { right, 0, false };
    SiblingCoreDropObservation right_observation = { left, 0, false };
    WASMComponentResourceInstance left_resource = {};
    WASMComponentResourceInstance right_resource = {};
    left_resource.is_host = true;
    left_resource.host_drop_callback = observe_sibling_core_alive;
    left_resource.host_drop_attachment = &left_observation;
    right_resource.is_host = true;
    right_resource.host_drop_callback = observe_sibling_core_alive;
    right_resource.host_drop_attachment = &right_observation;

    WASMResourceHandle *left_handle =
        wasm_create_resource_handle(&left_resource, 1, true);
    WASMResourceHandle *right_handle =
        wasm_create_resource_handle(&right_resource, 2, true);
    uint32_t left_index = 0;
    uint32_t right_index = 0;
    ASSERT_NE(left_handle, nullptr);
    ASSERT_NE(right_handle, nullptr);
    ASSERT_TRUE(wasm_component_table_add(left->table, left_handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &left_index));
    ASSERT_TRUE(wasm_component_table_add(right->table, right_handle,
                                         WASM_TABLE_ELEM_RESOURCE_HANDLE,
                                         &right_index));

    wasm_component_deinstantiate(root);

    EXPECT_EQ(left_observation.count, 1u);
    EXPECT_EQ(right_observation.count, 1u);
    EXPECT_TRUE(left_observation.sibling_core_was_alive);
    EXPECT_TRUE(right_observation.sibling_core_was_alive);
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
