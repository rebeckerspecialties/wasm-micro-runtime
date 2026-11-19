/*
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#ifndef WASM_OWNER_TLS_H
#define WASM_OWNER_TLS_H

/* Include the platform headers before this header. Prefer the port's own
   storage qualifier; do not introduce a runtime lookup on guest entry. */
#if defined(os_thread_local_attribute)
#define WASM_OWNER_TLS os_thread_local_attribute
#elif defined(_MSC_VER)
#define WASM_OWNER_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define WASM_OWNER_TLS __thread
#else
#define WASM_OWNER_TLS _Thread_local
#endif
#endif
