/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stddef.h>
#include <stdint.h>

#if LOAD_ARGS_INCLUDE_C_API_FIRST
#include "wasm_c_api.h"
#include "wasm_export.h"
#else
#include "wasm_export.h"
#include "wasm_c_api.h"
#endif

#ifdef __cplusplus
#define ABI_STATIC_ASSERT static_assert
#else
#define ABI_STATIC_ASSERT _Static_assert
#endif

#define LOAD_ARGS_LEGACY_SIZE \
    (UINTPTR_MAX == UINT64_MAX ? (size_t)16 : (size_t)8)

ABI_STATIC_ASSERT(offsetof(LoadArgs, name) == 0, "legacy name offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs, clone_wasm_binary) == sizeof(void *),
                  "legacy clone flag offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs, wasm_binary_freeable)
                      == sizeof(void *) + 1,
                  "legacy freeable flag offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs, no_resolve) == sizeof(void *) + 2,
                  "legacy resolve flag offset changed");
#if WASM_ENABLE_COMPONENT_MODEL != 0
ABI_STATIC_ASSERT(offsetof(LoadArgs, is_component) == sizeof(void *) + 3,
                  "legacy component flag offset changed");
#endif
ABI_STATIC_ASSERT(sizeof(LoadArgs) == LOAD_ARGS_LEGACY_SIZE,
                  "legacy LoadArgs size changed");

ABI_STATIC_ASSERT(offsetof(LoadArgs2, struct_size) == 0,
                  "LoadArgs2 size offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, abi_version) == 4,
                  "LoadArgs2 version offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, name) == 8,
                  "LoadArgs2 name offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, clone_wasm_binary) == 8 + sizeof(void *),
                  "LoadArgs2 flag offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, allocation_quota_attachment)
                      == 16 + sizeof(void *),
                  "LoadArgs2 quota offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, allocation_quota_reserve)
                      == 16 + 2 * sizeof(void *),
                  "LoadArgs2 reserve offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, allocation_quota_release)
                      == 16 + 3 * sizeof(void *),
                  "LoadArgs2 release offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, allocation_quota_attachment_retain)
                      == 16 + 4 * sizeof(void *),
                  "LoadArgs2 retain offset changed");
ABI_STATIC_ASSERT(offsetof(LoadArgs2, allocation_quota_attachment_release)
                      == 16 + 5 * sizeof(void *),
                  "LoadArgs2 attachment release offset changed");
ABI_STATIC_ASSERT(sizeof(LoadArgs2) == 16 + 6 * sizeof(void *),
                  "LoadArgs2 V1 size changed");
ABI_STATIC_ASSERT(WASM_LOAD_ARGS2_SIZE_V1 == sizeof(LoadArgs2),
                  "LoadArgs2 V1 minimum changed");

int
main(void)
{
    return 0;
}
