/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include "wasm_component.h"
#include "wasm_component_runtime.h"
#include "wasm_export.h"

bool
parse_core_limits(const uint8_t **payload, const uint8_t *end,
                  WASMComponentCoreLimits *out, char *error_buf,
                  uint32_t error_buf_size);
bool
parse_core_import_desc(const uint8_t **payload, const uint8_t *end,
                       WASMComponentCoreImportDesc *out, char *error_buf,
                       uint32_t error_buf_size);
bool
parse_core_module_decl(const uint8_t **payload, const uint8_t *end,
                       WASMComponentCoreModuleDecl *out, char *error_buf,
                       uint32_t error_buf_size);
}

namespace {

constexpr size_t kNoFailure = std::numeric_limits<size_t>::max();

struct AllocationTracker {
    std::mutex mutex;
    std::unordered_set<void *> live;
    size_t attempt = 0;
    size_t fail_at = kNoFailure;
    size_t invalid_frees = 0;
    bool injected_failure = false;
};

AllocationTracker allocation_tracker;

bool
should_fail_allocation()
{
    if (allocation_tracker.attempt++ == allocation_tracker.fail_at) {
        allocation_tracker.injected_failure = true;
        return true;
    }
    return false;
}

void *
tracking_malloc(unsigned int size)
{
    std::lock_guard<std::mutex> lock(allocation_tracker.mutex);
    if (should_fail_allocation()) {
        return nullptr;
    }
    void *result = std::malloc(size ? size : 1);
    if (result) {
        allocation_tracker.live.insert(result);
    }
    return result;
}

void *
tracking_realloc(void *ptr, unsigned int size)
{
    std::lock_guard<std::mutex> lock(allocation_tracker.mutex);
    if (!ptr) {
        if (should_fail_allocation()) {
            return nullptr;
        }
        void *result = std::malloc(size ? size : 1);
        if (result) {
            allocation_tracker.live.insert(result);
        }
        return result;
    }
    if (allocation_tracker.live.find(ptr) == allocation_tracker.live.end()) {
        allocation_tracker.invalid_frees++;
        return nullptr;
    }
    if (should_fail_allocation()) {
        return nullptr;
    }

    allocation_tracker.live.erase(ptr);
    void *result = std::realloc(ptr, size ? size : 1);
    if (!result) {
        allocation_tracker.live.insert(ptr);
        return nullptr;
    }
    allocation_tracker.live.insert(result);
    return result;
}

void
tracking_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(allocation_tracker.mutex);
    if (allocation_tracker.live.erase(ptr) == 0) {
        allocation_tracker.invalid_frees++;
        return;
    }
    std::free(ptr);
}

std::vector<uint8_t>
read_binary(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.good()) << path;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}

LoadArgs
component_load_args()
{
    static char component_name[] = "parser-cleanup-test";
    LoadArgs args = {};
    args.name = component_name;
    args.is_component = true;
    return args;
}

bool
read_u32_leb(const std::vector<uint8_t> &bytes, size_t *offset, uint32_t *value)
{
    uint32_t result = 0;
    uint32_t shift = 0;
    while (*offset < bytes.size() && shift < 35) {
        uint8_t byte = bytes[(*offset)++];
        result |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

void
append_u32_leb(std::vector<uint8_t> *bytes, uint32_t value)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        bytes->push_back(byte);
    } while (value);
}

struct SectionSlice {
    uint8_t id;
    const uint8_t *payload;
    uint32_t payload_size;
};

std::vector<SectionSlice>
top_level_sections(const std::vector<uint8_t> &component)
{
    std::vector<SectionSlice> sections;
    size_t offset = 8;
    while (offset < component.size()) {
        uint8_t id = component[offset++];
        uint32_t payload_size = 0;
        if (!read_u32_leb(component, &offset, &payload_size)
            || payload_size > component.size() - offset) {
            ADD_FAILURE() << "valid test component has malformed section table";
            return {};
        }
        sections.push_back({ id, component.data() + offset, payload_size });
        offset += payload_size;
    }
    return sections;
}

std::vector<uint8_t>
single_section_component(const std::vector<uint8_t> &source,
                         const SectionSlice &section, uint32_t prefix_size)
{
    std::vector<uint8_t> result(source.begin(), source.begin() + 8);
    result.push_back(section.id);
    append_u32_leb(&result, prefix_size);
    result.insert(result.end(), section.payload, section.payload + prefix_size);
    return result;
}

void
load_and_release_if_valid(std::vector<uint8_t> *bytes)
{
    char error[256] = {};
    LoadArgs args = component_load_args();
    WASMComponent *component =
        wasm_component_load(bytes->data(), static_cast<uint32_t>(bytes->size()),
                            &args, error, sizeof(error));
    if (component) {
        wasm_component_unload(component);
    }
    else {
        EXPECT_NE(error[0], '\0');
    }
}

TEST(ComponentParserFailureCleanupTest,
     TruncatedSectionPayloadsAreAlwaysSafelyReleasable)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(tracking_malloc);
    init_args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(tracking_realloc);
    init_args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(tracking_free);
    allocation_tracker.attempt = 0;
    allocation_tracker.fail_at = kNoFailure;
    allocation_tracker.invalid_frees = 0;
    allocation_tracker.injected_failure = false;
    allocation_tracker.live.clear();
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    const size_t runtime_baseline = allocation_tracker.live.size();

    std::vector<uint8_t> source = read_binary("add.wasm");
    ASSERT_GE(source.size(), 8u);
    std::set<uint8_t> tested_ids;
    for (const SectionSlice &section : top_level_sections(source)) {
        if (!tested_ids.insert(section.id).second) {
            continue;
        }

        std::set<uint32_t> cuts;
        uint32_t dense_end = std::min<uint32_t>(section.payload_size, 64);
        for (uint32_t cut = 0; cut <= dense_end; cut++) {
            cuts.insert(cut);
        }
        for (uint32_t cut = 128; cut < section.payload_size; cut *= 2) {
            cuts.insert(cut);
            if (cut > UINT32_MAX / 2) {
                break;
            }
        }
        if (section.payload_size > 0) {
            cuts.insert(section.payload_size / 4);
            cuts.insert(section.payload_size / 2);
            cuts.insert(section.payload_size * 3 / 4);
            cuts.insert(section.payload_size - 1);
        }

        for (uint32_t cut : cuts) {
            if (cut >= section.payload_size) {
                continue;
            }
            std::vector<uint8_t> truncated =
                single_section_component(source, section, cut);
            load_and_release_if_valid(&truncated);
            ASSERT_EQ(allocation_tracker.invalid_frees, 0u)
                << "section " << section.id << ", cut " << cut;
            ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline)
                << "section " << section.id << ", cut " << cut;
        }
    }

    const std::vector<std::pair<uint8_t, std::vector<uint8_t>>> synthetic = {
        { WASM_COMP_SECTION_CORE_TYPE, { 1, 0x50, 0 } },
        { WASM_COMP_SECTION_START, { 0, 2, 0, 1, 0 } },
        { WASM_COMP_SECTION_IMPORTS, { 1, 0, 1, 'x', 1, 0 } },
        { WASM_COMP_SECTION_VALUES, { 1, WASM_COMP_PRIMVAL_U8, 1, 42 } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_RESULT, WASM_COMP_OPTIONAL_FALSE,
            WASM_COMP_OPTIONAL_FALSE } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_STREAM, WASM_COMP_OPTIONAL_FALSE } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_FUTURE, WASM_COMP_OPTIONAL_FALSE } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_RESOURCE_TYPE_SYNC, WASM_COMP_RESOURCE_REP_I32,
            WASM_COMP_OPTIONAL_FALSE } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_RESOURCE_TYPE_ASYNC, WASM_COMP_RESOURCE_REP_I32, 0,
            WASM_COMP_OPTIONAL_FALSE } },
        { WASM_COMP_SECTION_TYPE, { 1, WASM_COMP_COMPONENT_TYPE, 0 } },
        { WASM_COMP_SECTION_TYPE, { 1, WASM_COMP_INSTANCE_TYPE, 0 } },
        /* Fail after allocating the current record field/case. */
        { WASM_COMP_SECTION_TYPE, { 1, WASM_COMP_DEF_VAL_RECORD, 1, 1, 'x' } },
        { WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_VARIANT, 1, 1, 'x',
            WASM_COMP_OPTIONAL_TRUE } },
    };
    for (const auto &[id, payload] : synthetic) {
        SectionSlice section = { id, payload.data(),
                                 static_cast<uint32_t>(payload.size()) };
        for (uint32_t cut = 0; cut <= section.payload_size; cut++) {
            std::vector<uint8_t> truncated =
                single_section_component(source, section, cut);
            load_and_release_if_valid(&truncated);
            ASSERT_EQ(allocation_tracker.invalid_frees, 0u)
                << "synthetic section " << section.id << ", cut " << cut;
            ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline)
                << "synthetic section " << section.id << ", cut " << cut;
        }
    }

    wasm_runtime_destroy();
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_TRUE(allocation_tracker.live.empty());
}

TEST(ComponentParserFailureCleanupTest,
     ScalarCoreTypeParsersRejectTruncatedInput)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(tracking_malloc);
    init_args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(tracking_realloc);
    init_args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(tracking_free);
    allocation_tracker.attempt = 0;
    allocation_tracker.fail_at = kNoFailure;
    allocation_tracker.invalid_frees = 0;
    allocation_tracker.injected_failure = false;
    allocation_tracker.live.clear();
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    const size_t runtime_baseline = allocation_tracker.live.size();

    uint8_t empty_storage = 0;
    const uint8_t *p = &empty_storage;
    const uint8_t *end = p;
    char error[256] = {};
    WASMComponentCoreLimits limits = {};
    EXPECT_FALSE(parse_core_limits(&p, end, &limits, error, sizeof(error)));
    EXPECT_NE(error[0], '\0');

    p = &empty_storage;
    error[0] = '\0';
    WASMComponentCoreValType val_type = {};
    EXPECT_FALSE(parse_core_valtype(&p, end, &val_type, error, sizeof(error)));
    EXPECT_NE(error[0], '\0');

    const uint8_t truncated_heap_type[] = { 0x63 };
    p = truncated_heap_type;
    error[0] = '\0';
    EXPECT_FALSE(parse_core_valtype(&p, truncated_heap_type + 1, &val_type,
                                    error, sizeof(error)));
    EXPECT_NE(error[0], '\0');

    const std::vector<std::vector<uint8_t>> import_desc_cases = {
        {},
        { WASM_CORE_IMPORTDESC_TABLE },
        { WASM_CORE_IMPORTDESC_TABLE, WASM_CORE_REFTYPE_FUNC_REF },
        { WASM_CORE_IMPORTDESC_GLOBAL, WASM_CORE_NUM_TYPE_I32 },
    };
    for (const std::vector<uint8_t> &bytes : import_desc_cases) {
        p = bytes.empty() ? &empty_storage : bytes.data();
        end = p + bytes.size();
        error[0] = '\0';
        WASMComponentCoreImportDesc import_desc = {};
        EXPECT_FALSE(parse_core_import_desc(&p, end, &import_desc, error,
                                            sizeof(error)));
        EXPECT_NE(error[0], '\0');
        EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
        EXPECT_EQ(allocation_tracker.live.size(), runtime_baseline);
    }

    p = &empty_storage;
    end = p;
    error[0] = '\0';
    WASMComponentCoreAliasTarget alias_target = {};
    EXPECT_FALSE(
        parse_alias_target(&p, end, &alias_target, error, sizeof(error)));
    EXPECT_NE(error[0], '\0');

    p = &empty_storage;
    error[0] = '\0';
    WASMComponentCoreModuleDecl module_decl = {};
    EXPECT_FALSE(
        parse_core_module_decl(&p, end, &module_decl, error, sizeof(error)));
    EXPECT_NE(error[0], '\0');
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_EQ(allocation_tracker.live.size(), runtime_baseline);

    wasm_runtime_destroy();
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_TRUE(allocation_tracker.live.empty());
}

TEST(ComponentParserFailureCleanupTest, PublicLoadPreservesParserDiagnostic)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));

    std::vector<uint8_t> malformed = read_binary("add.wasm");
    ASSERT_GE(malformed.size(), 8u);
    malformed.resize(8);
    malformed.push_back(WASM_COMP_SECTION_TYPE);
    append_u32_leb(&malformed, 10);
    malformed.push_back(0);

    char error[256] = {};
    LoadArgs args = component_load_args();
    EXPECT_EQ(wasm_component_load(malformed.data(), malformed.size(), &args,
                                  error, sizeof(error)),
              nullptr);
    EXPECT_NE(std::string(error).find("payload exceeds input"),
              std::string::npos)
        << error;

    wasm_runtime_destroy();
}

TEST(ComponentParserFailureCleanupTest,
     InstanceSectionsReportTheirActualConsumedLength)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));

    const std::vector<uint8_t> source = read_binary("add.wasm");
    ASSERT_GE(source.size(), 8u);
    for (uint8_t section_id :
         { WASM_COMP_SECTION_CORE_INSTANCE, WASM_COMP_SECTION_INSTANCES }) {
        const std::vector<uint8_t> payload = { 0, 0 };
        const SectionSlice section = {
            section_id,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
        };
        std::vector<uint8_t> malformed = single_section_component(
            source, section, static_cast<uint32_t>(payload.size()));
        char error[256] = {};
        LoadArgs args = component_load_args();
        WASMComponent *component = wasm_component_load(
            malformed.data(), static_cast<uint32_t>(malformed.size()), &args,
            error, sizeof(error));
        EXPECT_EQ(component, nullptr);
        if (component) {
            wasm_component_unload(component);
        }
        EXPECT_NE(std::string(error).find("consumed 1 of 2 bytes"),
                  std::string::npos)
            << error;
    }

    wasm_runtime_destroy();
}

TEST(ComponentParserFailureCleanupTest,
     OversizedVectorCountsAreRejectedBeforeArrayAllocation)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(tracking_malloc);
    init_args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(tracking_realloc);
    init_args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(tracking_free);
    allocation_tracker.attempt = 0;
    allocation_tracker.fail_at = kNoFailure;
    allocation_tracker.invalid_frees = 0;
    allocation_tracker.injected_failure = false;
    allocation_tracker.live.clear();
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    const size_t runtime_baseline = allocation_tracker.live.size();

    const std::vector<uint8_t> source = read_binary("add.wasm");
    ASSERT_GE(source.size(), 8u);
    char direct_error[256] = {};
    EXPECT_EQ(wasm_component_checked_calloc(0x10000001u, 16, nullptr, nullptr,
                                            0, "direct oversized regression",
                                            direct_error, sizeof(direct_error)),
              nullptr);
    EXPECT_NE(std::string(direct_error).find("too large"), std::string::npos)
        << direct_error;
    EXPECT_EQ(allocation_tracker.live.size(), runtime_baseline);

    struct OversizedCountCase {
        const char *description;
        uint8_t section_id;
        std::vector<uint8_t> prefix;
    };
    const std::vector<OversizedCountCase> cases = {
        { "core instances", WASM_COMP_SECTION_CORE_INSTANCE, {} },
        { "core types", WASM_COMP_SECTION_CORE_TYPE, {} },
        { "component instances", WASM_COMP_SECTION_INSTANCES, {} },
        { "aliases", WASM_COMP_SECTION_ALIASES, {} },
        { "types", WASM_COMP_SECTION_TYPE, {} },
        { "canonical definitions", WASM_COMP_SECTION_CANONS, {} },
        { "start arguments", WASM_COMP_SECTION_START, { 0 } },
        { "imports", WASM_COMP_SECTION_IMPORTS, {} },
        { "exports", WASM_COMP_SECTION_EXPORTS, {} },
        { "values", WASM_COMP_SECTION_VALUES, {} },
        { "core instantiate arguments",
          WASM_COMP_SECTION_CORE_INSTANCE,
          { 1, WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS, 0 } },
        { "core inline exports",
          WASM_COMP_SECTION_CORE_INSTANCE,
          { 1, WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS } },
        { "component instantiate arguments",
          WASM_COMP_SECTION_INSTANCES,
          { 1, WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS, 0 } },
        { "component inline exports",
          WASM_COMP_SECTION_INSTANCES,
          { 1, WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS } },
        { "core module declarations",
          WASM_COMP_SECTION_CORE_TYPE,
          { 1, 0x50 } },
        { "record fields",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_RECORD } },
        { "variant cases",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_VARIANT } },
        { "tuple elements",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_TUPLE } },
        { "enum labels",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_DEF_VAL_ENUM } },
        { "function parameters",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_FUNC_TYPE } },
        { "component type declarations",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_COMPONENT_TYPE } },
        { "component instance declarations",
          WASM_COMP_SECTION_TYPE,
          { 1, WASM_COMP_INSTANCE_TYPE } },
        { "canonical options",
          WASM_COMP_SECTION_CANONS,
          { 1, WASM_COMP_CANON_LIFT, 0, 0 } },
    };

    for (const OversizedCountCase &test_case : cases) {
        std::vector<uint8_t> payload = test_case.prefix;
        append_u32_leb(&payload, 0x10000001u);
        SectionSlice section = {
            test_case.section_id,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
        };
        std::vector<uint8_t> malformed = single_section_component(
            source, section, static_cast<uint32_t>(payload.size()));
        char error[256] = {};
        LoadArgs args = component_load_args();
        WASMComponent *component = wasm_component_load(
            malformed.data(), static_cast<uint32_t>(malformed.size()), &args,
            error, sizeof(error));

        EXPECT_EQ(component, nullptr) << test_case.description;
        if (component) {
            wasm_component_unload(component);
        }
        EXPECT_NE(std::string(error).find("exceeds remaining payload"),
                  std::string::npos)
            << test_case.description << ": " << error;
        EXPECT_EQ(allocation_tracker.invalid_frees, 0u)
            << test_case.description;
        EXPECT_EQ(allocation_tracker.live.size(), runtime_baseline)
            << test_case.description;
    }

    wasm_runtime_destroy();
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_TRUE(allocation_tracker.live.empty());
}

TEST(ComponentParserFailureCleanupTest,
     FlagsAndEnumLabelAllocationFailuresPreserveArrayOwnership)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(tracking_malloc);
    init_args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(tracking_realloc);
    init_args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(tracking_free);
    allocation_tracker.attempt = 0;
    allocation_tracker.fail_at = kNoFailure;
    allocation_tracker.invalid_frees = 0;
    allocation_tracker.injected_failure = false;
    allocation_tracker.live.clear();
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    const size_t runtime_baseline = allocation_tracker.live.size();

    const std::vector<uint8_t> source = read_binary("add.wasm");
    ASSERT_GE(source.size(), 8u);
    struct LabelTypeCase {
        const char *description;
        uint8_t tag;
    };
    const LabelTypeCase cases[] = {
        { "flags", WASM_COMP_DEF_VAL_FLAGS },
        { "enum", WASM_COMP_DEF_VAL_ENUM },
    };

    for (const LabelTypeCase &test_case : cases) {
        const std::vector<uint8_t> payload = { 1, test_case.tag, 1, 1, 'x' };
        const SectionSlice section = {
            WASM_COMP_SECTION_TYPE,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
        };
        const std::vector<uint8_t> pristine = single_section_component(
            source, section, static_cast<uint32_t>(payload.size()));

        std::vector<uint8_t> bytes = pristine;
        LoadArgs args = component_load_args();
        char error[256] = {};
        allocation_tracker.attempt = 0;
        WASMComponent *component = wasm_component_load(
            bytes.data(), static_cast<uint32_t>(bytes.size()), &args, error,
            sizeof(error));
        ASSERT_NE(component, nullptr) << test_case.description << ": " << error;
        const size_t successful_load_allocations = allocation_tracker.attempt;
        wasm_component_unload(component);
        ASSERT_GE(successful_load_allocations, 6u) << test_case.description;
        ASSERT_EQ(allocation_tracker.invalid_frees, 0u)
            << test_case.description;
        ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline)
            << test_case.description;

        for (size_t fail_at = 0; fail_at < successful_load_allocations;
             fail_at++) {
            bytes = pristine;
            allocation_tracker.attempt = 0;
            allocation_tracker.fail_at = fail_at;
            allocation_tracker.injected_failure = false;
            error[0] = '\0';

            component = wasm_component_load(bytes.data(),
                                            static_cast<uint32_t>(bytes.size()),
                                            &args, error, sizeof(error));
            allocation_tracker.fail_at = kNoFailure;
            EXPECT_TRUE(allocation_tracker.injected_failure)
                << test_case.description << ", failure index " << fail_at;
            EXPECT_EQ(component, nullptr)
                << test_case.description << ", failure index " << fail_at;
            if (component) {
                wasm_component_unload(component);
            }
            EXPECT_NE(error[0], '\0')
                << test_case.description << ", failure index " << fail_at;
            ASSERT_EQ(allocation_tracker.invalid_frees, 0u)
                << test_case.description << ", failure index " << fail_at;
            ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline)
                << test_case.description << ", failure index " << fail_at;
        }
    }

    wasm_runtime_destroy();
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_TRUE(allocation_tracker.live.empty());
}

TEST(ComponentParserFailureCleanupTest,
     ComponentInstanceAllocationRejectsOverflowingLayout)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));

    WASMComponentIndexCount counts = {};
    counts.functions = UINT32_MAX;
    char error[256] = {};
    EXPECT_EQ(wasm_component_instance_allocate(&counts, error, sizeof(error)),
              nullptr);
    EXPECT_NE(std::string(error).find("too large"), std::string::npos) << error;

    WASMComponentLabelValType dummy_field = {};
    WASMComponentRecordType record = {};
    record.count = UINT32_MAX;
    record.fields = &dummy_field;
    WASMComponentDefValType def_val = {};
    def_val.tag = WASM_COMP_DEF_VAL_RECORD;
    def_val.def_val.record = &record;
    uint64 def_val_size = 0;
    ASSERT_TRUE(wasm_get_def_val_type_size(&def_val, &def_val_size));
    EXPECT_GT(def_val_size, UINT32_MAX);

    WASMComponentParamList params = {};
    params.count = UINT32_MAX;
    params.params = &dummy_field;
    WASMComponentResultList results = {};
    WASMComponentFuncType func_type = {};
    func_type.params = &params;
    func_type.results = &results;
    uint64 func_type_size = 0;
    ASSERT_TRUE(wasm_get_func_type_size(&func_type, &func_type_size));
    EXPECT_GT(func_type_size, UINT32_MAX);

    WASMComponentTypes nested_type = {};
    nested_type.tag = WASM_COMP_DEF_TYPE;
    nested_type.type.def_val_type = &def_val;
    WASMComponentInstDecl instance_decl = {};
    instance_decl.tag = WASM_COMP_COMPONENT_DECL_INSTANCE_TYPE;
    instance_decl.decl.type = &nested_type;
    WASMComponentInstType instance_type = {};
    instance_type.count = 1;
    instance_type.instance_decls = &instance_decl;
    WASMComponentInstanceDeclTypeSize instance_counts = {};
    uint64 instance_size = 0;
    ASSERT_TRUE(wasm_get_inst_decl_size(&instance_type, &instance_counts,
                                        &instance_size));
    EXPECT_GT(instance_size, UINT32_MAX);
    EXPECT_GT(instance_counts.types_size, UINT32_MAX);

    counts = {};
    counts.types_total_size = UINT64_MAX;
    error[0] = '\0';
    EXPECT_EQ(wasm_component_instance_allocate(&counts, error, sizeof(error)),
              nullptr);
    EXPECT_NE(std::string(error).find("too large"), std::string::npos) << error;

    wasm_runtime_destroy();
}

TEST(ComponentParserFailureCleanupTest,
     EveryInjectedAllocationFailureReturnsToTheRuntimeBaseline)
{
    RuntimeInitArgs init_args = {};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(tracking_malloc);
    init_args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(tracking_realloc);
    init_args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(tracking_free);

    allocation_tracker.attempt = 0;
    allocation_tracker.fail_at = kNoFailure;
    allocation_tracker.invalid_frees = 0;
    allocation_tracker.injected_failure = false;
    allocation_tracker.live.clear();
    ASSERT_TRUE(wasm_runtime_full_init(&init_args));
    const size_t runtime_baseline = allocation_tracker.live.size();

    const std::vector<uint8_t> pristine_source = read_binary("add.wasm");
    ASSERT_FALSE(pristine_source.empty());
    std::vector<uint8_t> source = pristine_source;
    LoadArgs args = component_load_args();
    char error[256] = {};

    allocation_tracker.attempt = 0;
    WASMComponent *component =
        wasm_component_load(source.data(), static_cast<uint32_t>(source.size()),
                            &args, error, sizeof(error));
    ASSERT_NE(component, nullptr) << error;
    wasm_component_unload(component);
    ASSERT_EQ(allocation_tracker.invalid_frees, 0u);
    ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline);

    /* The first core-module load can initialize process-wide parser caches.
     * Measure the steady-state path that each injected iteration follows. */
    source = pristine_source;
    allocation_tracker.attempt = 0;
    component =
        wasm_component_load(source.data(), static_cast<uint32_t>(source.size()),
                            &args, error, sizeof(error));
    ASSERT_NE(component, nullptr) << error;
    const size_t successful_load_allocations = allocation_tracker.attempt;
    wasm_component_unload(component);
    ASSERT_EQ(allocation_tracker.invalid_frees, 0u);
    ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline);

    for (size_t fail_at = 0; fail_at < successful_load_allocations; fail_at++) {
        source = pristine_source;
        allocation_tracker.attempt = 0;
        allocation_tracker.fail_at = fail_at;
        allocation_tracker.injected_failure = false;
        error[0] = '\0';

        component = wasm_component_load(source.data(),
                                        static_cast<uint32_t>(source.size()),
                                        &args, error, sizeof(error));
        allocation_tracker.fail_at = kNoFailure;
        if (component) {
            wasm_component_unload(component);
        }
        else {
            EXPECT_NE(error[0], '\0') << "failure index " << fail_at;
        }

        ASSERT_TRUE(allocation_tracker.injected_failure)
            << "failure index " << fail_at;
        ASSERT_EQ(allocation_tracker.invalid_frees, 0u)
            << "failure index " << fail_at;
        ASSERT_EQ(allocation_tracker.live.size(), runtime_baseline)
            << "failure index " << fail_at;
    }

    wasm_runtime_destroy();
    EXPECT_EQ(allocation_tracker.invalid_frees, 0u);
    EXPECT_TRUE(allocation_tracker.live.empty());
}

} // namespace
