/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include "bh_platform.h"
#include "wasm_c_api.h"
#include "wasm_c_api_internal.h"
#include "wasm_runtime_common.h"

#ifndef own
#define own
#endif

class CApiTests : public ::testing::Test
{
  protected:
    void SetUp()
    {
        bh_log_set_verbose_level(5);
        engine = nullptr;
        engine = wasm_engine_new();
        ASSERT_NE(nullptr, engine);
    }

    void TearDown() { wasm_engine_delete(engine); }

    wasm_engine_t *engine;
};

TEST_F(CApiTests, empty) {}

TEST_F(CApiTests, wasm_engine_t)
{
    wasm_engine_t *engine1 = wasm_engine_new();
    wasm_engine_t *engine2 = wasm_engine_new();
    EXPECT_EQ(engine1, engine2);
    /* TearDown() will delete it */
}

TEST_F(CApiTests, wasm_store_t)
{
    wasm_engine_t *engine = wasm_engine_new();
    wasm_store_t *store1 = wasm_store_new(engine);
    EXPECT_NE(nullptr, store1);
    EXPECT_NE(nullptr, store1->modules->data);

    wasm_store_t *store2 = wasm_store_new(engine);
    EXPECT_NE(store1, store2);
    EXPECT_NE(store1->modules->data, store2->modules->data);

    wasm_store_delete(store1);
    wasm_store_delete(store2);

    store1 = wasm_store_new(engine);
    EXPECT_NE(nullptr, store1);
    wasm_store_delete(store1);
}

static void
count_foreign_finalizer(void *value)
{
    int *count = reinterpret_cast<int *>(value);
    (*count)++;
}

TEST_F(CApiTests, wasm_foreign_lifetime)
{
    wasm_store_t *store = wasm_store_new(engine);
    ASSERT_NE(nullptr, store);

    int finalized = 0;
    wasm_foreign_t *foreign = wasm_foreign_new(store);
    ASSERT_NE(nullptr, foreign);
    EXPECT_EQ(1, foreign->ref_cnt);
    EXPECT_EQ(0u, foreign->foreign_idx_rt);
    wasm_foreign_set_host_info_with_finalizer(foreign, &finalized,
                                              count_foreign_finalizer);

    wasm_ref_t *ref = wasm_foreign_as_ref(foreign);
    ASSERT_NE(nullptr, ref);
    EXPECT_EQ(2, foreign->ref_cnt);

    wasm_foreign_delete(foreign);
    EXPECT_EQ(0, finalized);
    EXPECT_EQ(1, foreign->ref_cnt);

    wasm_ref_delete(ref);
    EXPECT_EQ(1, finalized);

    /* The dead registry slot is reused, and store teardown releases a live
       host handle exactly once. */
    foreign = wasm_foreign_new(store);
    ASSERT_NE(nullptr, foreign);
    EXPECT_EQ(0u, foreign->foreign_idx_rt);
    wasm_foreign_set_host_info_with_finalizer(foreign, &finalized,
                                              count_foreign_finalizer);
    wasm_store_delete(store);
    EXPECT_EQ(2, finalized);
}

TEST_F(CApiTests, wasm_externref_table_owns_private_reference)
{
    static wasm_byte_t module_bytes[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        /* (table 1 externref) */
        0x04, 0x04, 0x01, 0x6f, 0x00, 0x01,
        /* (global (mut externref) (ref.null extern)) */
        0x06, 0x06, 0x01, 0x6f, 0x01, (wasm_byte_t)0xd0, 0x6f, 0x0b,
        /* table/global exports */
        0x07, 0x12, 0x02, 0x05, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x01, 0x00, 0x06,
        0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x03, 0x00
    };
    wasm_store_t *store = wasm_store_new(engine);
    ASSERT_NE(nullptr, store);
    wasm_byte_vec_t binary = WASM_ARRAY_VEC(module_bytes);
    wasm_module_t *module = wasm_module_new(store, &binary);
    ASSERT_NE(nullptr, module);
    wasm_extern_vec_t no_imports = WASM_EMPTY_VEC;
    wasm_instance_t *instance =
        wasm_instance_new(store, module, &no_imports, nullptr);
    ASSERT_NE(nullptr, instance);
    wasm_extern_vec_t exports = WASM_EMPTY_VEC;
    wasm_instance_exports(instance, &exports);
    ASSERT_EQ(2u, exports.num_elems);
    wasm_table_t *table = wasm_extern_as_table(exports.data[0]);
    wasm_global_t *global = wasm_extern_as_global(exports.data[1]);
    ASSERT_NE(nullptr, table);
    ASSERT_NE(nullptr, global);

    int finalized = 0;
    wasm_foreign_t *foreign = wasm_foreign_new(store);
    ASSERT_NE(nullptr, foreign);
    wasm_foreign_set_host_info_with_finalizer(foreign, &finalized,
                                              count_foreign_finalizer);
    wasm_ref_t *caller_ref = wasm_foreign_as_ref(foreign);
    ASSERT_NE(nullptr, caller_ref);
    ASSERT_TRUE(wasm_table_set(table, 0, caller_ref));

    /* wasm_table_set borrows its argument.  Deleting every caller handle must
       not invalidate the table's private map reference. */
    wasm_ref_delete(caller_ref);
    wasm_foreign_delete(foreign);
    EXPECT_EQ(0, finalized);

    wasm_ref_t *from_table = wasm_table_get(table, 0);
    ASSERT_NE(nullptr, from_table);
    wasm_ref_delete(from_table);
    EXPECT_EQ(0, finalized);

    ASSERT_TRUE(wasm_table_set(table, 0, nullptr));
    wasm_externref_reclaim(instance->inst_comm_rt);
    EXPECT_EQ(1, finalized);
    EXPECT_EQ(nullptr, wasm_table_get(table, 0));

    foreign = wasm_foreign_new(store);
    ASSERT_NE(nullptr, foreign);
    wasm_foreign_set_host_info_with_finalizer(foreign, &finalized,
                                              count_foreign_finalizer);
    caller_ref = wasm_foreign_as_ref(foreign);
    ASSERT_NE(nullptr, caller_ref);
    wasm_val_t value = WASM_REF_VAL(caller_ref);
    wasm_global_set(global, &value);
    wasm_ref_delete(caller_ref);
    wasm_foreign_delete(foreign);
    EXPECT_EQ(1, finalized);

    wasm_val_t from_global = WASM_INIT_VAL;
    wasm_global_get(global, &from_global);
    ASSERT_EQ(WASM_EXTERNREF, from_global.kind);
    ASSERT_NE(nullptr, from_global.of.ref);
    wasm_ref_delete(from_global.of.ref);
    value = WASM_REF_VAL(nullptr);
    wasm_global_set(global, &value);
    wasm_externref_reclaim(instance->inst_comm_rt);
    EXPECT_EQ(2, finalized);

    wasm_extern_vec_delete(&exports);
    wasm_store_delete(store);
}

TEST_F(CApiTests, wasm_byte_vec_t)
{
    wasm_byte_vec_t byte_vec = { 0 };
    wasm_byte_vec_new_uninitialized(&byte_vec, 10);
    EXPECT_NE(nullptr, byte_vec.data);
    EXPECT_EQ(10, byte_vec.size);

    byte_vec.data[0] = (wasm_byte_t)'a';
    byte_vec.data[1] = (wasm_byte_t)'b';
    byte_vec.data[2] = (wasm_byte_t)'c';
    EXPECT_STREQ("abc", (char *)byte_vec.data);

    byte_vec.data[5] = (wasm_byte_t)'d';
    byte_vec.data[6] = (wasm_byte_t)'e';
    byte_vec.data[7] = (wasm_byte_t)'f';
    EXPECT_STREQ("def", (char *)(byte_vec.data + 5));

    wasm_byte_vec_delete(&byte_vec);
    EXPECT_EQ(nullptr, byte_vec.data);
    EXPECT_EQ(0, byte_vec.size);
}

TEST_F(CApiTests, wasm_valtype_vec_t)
{
    wasm_valtype_vec_t tuple1 = { 0 };
    wasm_valtype_vec_new_uninitialized(&tuple1, 128);
    EXPECT_NE(nullptr, tuple1.data);
    EXPECT_EQ(128, tuple1.size);

    wasm_valtype_t *val_type_1 = wasm_valtype_new_i32();
    tuple1.data[0] = val_type_1;
    EXPECT_EQ(WASM_I32, wasm_valtype_kind(*(tuple1.data + 0)));
    wasm_valtype_vec_delete(&tuple1);
    wasm_valtype_delete(val_type_1);

    wasm_valtype_t *val_types[5] = {
        wasm_valtype_new_i32(), wasm_valtype_new_i64(), wasm_valtype_new_f32(),
        wasm_valtype_new_f64(), wasm_valtype_new_funcref()
    };

    wasm_valtype_vec_t tuple2 = { 0 };
    wasm_valtype_vec_new(&tuple2, 5, val_types);
    EXPECT_NE(nullptr, tuple2.data);
    EXPECT_EQ(WASM_F32, wasm_valtype_kind(*(tuple2.data + 2)));
    EXPECT_EQ(WASM_FUNCREF, wasm_valtype_kind(*(tuple2.data + 4)));

    wasm_valtype_vec_t tuple3 = { 0 };
    wasm_valtype_vec_copy(&tuple3, &tuple2);

    wasm_valtype_vec_delete(&tuple2);

    EXPECT_EQ(WASM_I64, wasm_valtype_kind(*(tuple3.data + 1)));
    EXPECT_EQ(WASM_F64, wasm_valtype_kind(*(tuple3.data + 3)));
    wasm_valtype_vec_delete(&tuple3);
}

TEST_F(CApiTests, wasm_functype_t)
{
    wasm_functype_t *callback_type = wasm_functype_new_1_1(
        wasm_valtype_new(WASM_EXTERNREF), wasm_valtype_new(WASM_FUNCREF));
    EXPECT_EQ(WASM_EXTERNREF,
              wasm_valtype_kind(*(wasm_functype_params(callback_type)->data)));
    wasm_functype_delete(callback_type);

    callback_type = wasm_functype_new_0_0();
    wasm_functype_delete(callback_type);

    callback_type = wasm_functype_new_0_1(wasm_valtype_new(WASM_EXTERNREF));
    const wasm_valtype_vec_t *results = wasm_functype_results(callback_type);
    EXPECT_EQ(WASM_EXTERNREF, wasm_valtype_kind(*(results->data)));
    wasm_functype_delete(callback_type);

    callback_type = wasm_functype_new_1_0(wasm_valtype_new(WASM_EXTERNREF));
    EXPECT_EQ(WASM_EXTERNREF,
              wasm_valtype_kind(*(wasm_functype_params(callback_type)->data)));
    wasm_functype_delete(callback_type);

    wasm_functype_t *func_type1 = wasm_functype_new_2_2(
        wasm_valtype_new(WASM_I32), wasm_valtype_new(WASM_I64),
        wasm_valtype_new(WASM_I32), wasm_valtype_new(WASM_I64));
    wasm_functype_t *func_type2 = wasm_functype_copy(func_type1);
    wasm_functype_delete(func_type1);

    EXPECT_EQ(WASM_I64, wasm_valtype_kind(
                            *(wasm_functype_results(func_type2)->data + 1)));
    wasm_functype_delete(func_type2);
}

TEST_F(CApiTests, wasm_globaltype_t)
{
    wasm_globaltype_t *const_f32_type =
        wasm_globaltype_new(wasm_valtype_new(WASM_F32), WASM_CONST);
    EXPECT_EQ(WASM_F32,
              wasm_valtype_kind(wasm_globaltype_content(const_f32_type)));

    wasm_globaltype_t *cloned = wasm_globaltype_copy(const_f32_type);
    wasm_globaltype_delete(const_f32_type);

    EXPECT_EQ(WASM_F32, wasm_valtype_kind(wasm_globaltype_content(cloned)));

    wasm_globaltype_delete(cloned);
}

static wasm_trap_t *
test_func(const wasm_val_vec_t *args, own wasm_val_vec_t *results)
{
    return NULL;
}

TEST_F(CApiTests, wasm_func_t)
{
    wasm_valtype_t *types[4] = { wasm_valtype_new_i32(), wasm_valtype_new_i64(),
                                 wasm_valtype_new_i64(),
                                 wasm_valtype_new_i32() };
    wasm_valtype_vec_t tuple1 = { 0 }, tuple2 = { 0 };
    wasm_valtype_vec_new(&tuple1, 4, types);
    wasm_valtype_vec_copy(&tuple2, &tuple1);
    wasm_functype_t *callback_type = wasm_functype_new(&tuple1, &tuple2);

    wasm_store_t *store = wasm_store_new(engine);
    wasm_func_t *callback_func = wasm_func_new(store, callback_type, test_func);
    wasm_functype_delete(callback_type);

    callback_type = callback_func->type;
    EXPECT_EQ(WASM_I32, wasm_valtype_kind(
                            *(wasm_functype_params(callback_type)->data + 0)));
    EXPECT_EQ(WASM_I32, wasm_valtype_kind(
                            *(wasm_functype_results(callback_type)->data + 3)));
    wasm_func_delete(callback_func);
    wasm_store_delete(store);
}
