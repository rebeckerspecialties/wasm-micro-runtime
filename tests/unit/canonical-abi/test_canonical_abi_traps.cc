/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Canonical-ABI trap coverage.
 *
 * The existing canonical-abi unit tests only lift values from in-bounds,
 * correctly-aligned addresses, so the trap_if() guards in
 * load_string_from_range / load_list_from_range / load_int (out-of-bounds,
 * ptr+len overflow, misalignment) are never exercised. For an interpreter-only
 * on-device runtime those guards are the line between a clean trap and a wild
 * read of linear memory, so they need explicit coverage.
 *
 * These tests drive the public range-load entry points with malformed
 * arguments and assert the load TRAPS (returns false) rather than reading out
 * of bounds. The *_PtrLenOverflow cases are regression tests for the 32-bit
 * wrap in the bounds checks (begin + byte_length / ptr + length*elem_sz were
 * added in 32-bit before the uint64 cast, so a pointer near UINT32_MAX could
 * bypass the check).
 */

#include <gtest/gtest.h>
#include "../component/helpers.h"
#include "wasm_memory.h"
#include "wasm_component_canonical.h"
#include "wasm_component_runtime.h"
#include <cstring>

class CanonicalAbiTrapTest : public testing::Test
{
  public:
    std::unique_ptr<ComponentHelper> helper;
    WASMMemoryInstance *mem = nullptr;
    uint32_t mem_size = 0;

    // cx and the option structs it points at must outlive each load() call.
    LiftOptions lift_opts;
    LiftLowerOptions lift_lower_opts;
    CanonicalOptions canonical_opts;
    LiftLowerContext cx;

    virtual void SetUp()
    {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();

        ASSERT_TRUE(helper->read_wasm_file("loaded.wasm"));
        ASSERT_TRUE(helper->component_raw != nullptr);
        ASSERT_TRUE(helper->load_component());
        ASSERT_TRUE(helper->instantiate_component());

        auto memories = helper->get_memories();
        ASSERT_GT(memories.size(), 0u);
        mem = memories[0];
        ASSERT_NE(mem, nullptr);
        mem_size = (uint32_t)mem->memory_data_size;
        ASSERT_GT(mem_size, 0u);
    }

    virtual void TearDown()
    {
        helper->do_teardown();
        helper = nullptr;
    }

    // Build a lift context bound to this component's memory + the given
    // string encoding. Returns &cx (whose backing structs are members).
    LiftLowerContext *build_cx(StringEncoding encoding)
    {
        memset(&lift_opts, 0, sizeof(lift_opts));
        lift_opts.string_encoding = encoding;
        lift_opts.memory = mem;

        memset(&lift_lower_opts, 0, sizeof(lift_lower_opts));
        lift_lower_opts.lift_opts = &lift_opts;
        lift_lower_opts.realloc_func = nullptr;

        memset(&canonical_opts, 0, sizeof(canonical_opts));
        canonical_opts.lift_lower_opts = &lift_lower_opts;
        canonical_opts.post_return_func = nullptr;
        canonical_opts.async = true;
        canonical_opts.callback_func = nullptr;

        memset(&cx, 0, sizeof(cx));
        cx.canonical_opts = &canonical_opts;
        cx.inst = helper->component_inst;
        cx.borrow_scope.task = nullptr;
        return &cx;
    }

    WASMComponentTypeInstance prim_type(WASMComponentPrimValType prim)
    {
        WASMComponentTypeInstance t;
        memset(&t, 0, sizeof(t));
        t.type = COMPONENT_VAL_TYPE_PRIMVAL;
        t.type_specific.primval = prim;
        t.alignment = compute_alignment(&t);
        t.elem_size = compute_elem_size(&t);
        return t;
    }
};

// --- string range traps ------------------------------------------------------

TEST_F(CanonicalAbiTrapTest, StringRangeCleanOutOfBoundsTraps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_8);
    wit_value_t out = nullptr;
    // begin near the end of memory, length runs past it.
    bool ok = load_string_from_range(c, mem_size - 2, /*code_units=*/64, &out);
    EXPECT_FALSE(ok) << "string lift past end of memory must trap";
    if (out)
        free_wit_value(out);
}

TEST_F(CanonicalAbiTrapTest, StringRangePtrLenOverflowTraps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_8);
    wit_value_t out = nullptr;
    // begin + byte_length wraps mod 2^32 (0xFFFFFFF0 + 0x20 == 0x10); the
    // bounds check must widen before adding, otherwise this reads at
    // memory_data + 0xFFFFFFF0.
    bool ok = load_string_from_range(c, 0xFFFFFFF0u, /*code_units=*/0x20, &out);
    EXPECT_FALSE(ok) << "ptr+len overflow must trap, not wrap past the check";
    if (out)
        free_wit_value(out);
}

TEST_F(CanonicalAbiTrapTest, StringRangeMisalignedUtf16Traps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_16);
    wit_value_t out = nullptr;
    // UTF-16 requires 2-byte alignment; an odd begin must trap.
    bool ok = load_string_from_range(c, /*begin=*/3, /*code_units=*/1, &out);
    EXPECT_FALSE(ok) << "misaligned UTF-16 string pointer must trap";
    if (out)
        free_wit_value(out);
}

// --- list range traps --------------------------------------------------------

TEST_F(CanonicalAbiTrapTest, ListRangeCleanOutOfBoundsTraps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_8);
    WASMComponentTypeInstance u32 = prim_type(WASM_COMP_PRIMVAL_U32);
    wit_value_t out = nullptr;
    // 4-byte elements; a length that runs past memory must trap.
    uint32_t length = (mem_size / 4) + 16;
    bool ok = load_list_from_range(c, /*ptr=*/0, length, &u32, &out);
    EXPECT_FALSE(ok) << "list lift past end of memory must trap";
    if (out)
        free_wit_value(out);
}

TEST_F(CanonicalAbiTrapTest, ListRangePtrLenOverflowTraps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_8);
    WASMComponentTypeInstance u32 = prim_type(WASM_COMP_PRIMVAL_U32);
    wit_value_t out = nullptr;
    // length * elem_sz wraps in 32-bit (0x40000000 * 4 == 0); the bounds
    // check must widen the multiply, otherwise it reads way out of bounds.
    bool ok =
        load_list_from_range(c, /*ptr=*/0, /*length=*/0x40000000u, &u32, &out);
    EXPECT_FALSE(ok) << "list length*elem_size overflow must trap";
    if (out)
        free_wit_value(out);
}

TEST_F(CanonicalAbiTrapTest, ListRangeMisalignedTraps)
{
    LiftLowerContext *c = build_cx(ENCODING_UTF_8);
    WASMComponentTypeInstance u32 = prim_type(WASM_COMP_PRIMVAL_U32);
    wit_value_t out = nullptr;
    // u32 list requires 4-byte alignment; ptr=2 must trap.
    bool ok = load_list_from_range(c, /*ptr=*/2, /*length=*/1, &u32, &out);
    EXPECT_FALSE(ok) << "misaligned list pointer must trap";
    if (out)
        free_wit_value(out);
}
