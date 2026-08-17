#!/usr/bin/env bash
# Copyright (C) 2026 Rebecker Specialties. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

test_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$test_dir/../../.." && pwd)
mode=${1:-asan-ubsan}
build_dir=${2:-$(mktemp -d "${TMPDIR:-/tmp}/wamr-allocation-quota-$mode.XXXXXX")}
architecture=$(uname -m)
multi_module=${WAMR_ALLOCATION_QUOTA_MULTI_MODULE:-0}
alloc_with_user_data=${WAMR_ALLOCATION_QUOTA_USER_DATA:-1}
wasm_cache=${WAMR_ALLOCATION_QUOTA_WASM_CACHE:-0}
openssl_prefix=${WAMR_ALLOCATION_QUOTA_OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}

if [[ "$multi_module" != 0 && "$multi_module" != 1 ]]; then
  echo "WAMR_ALLOCATION_QUOTA_MULTI_MODULE must be 0 or 1" >&2
  exit 2
fi
if [[ "$alloc_with_user_data" != 0 && "$alloc_with_user_data" != 1 ]]; then
  echo "WAMR_ALLOCATION_QUOTA_USER_DATA must be 0 or 1" >&2
  exit 2
fi
if [[ "$wasm_cache" != 0 && "$wasm_cache" != 1 ]]; then
  echo "WAMR_ALLOCATION_QUOTA_WASM_CACHE must be 0 or 1" >&2
  exit 2
fi
if [[ "$wasm_cache" == 1
      && ( ! -f "$openssl_prefix/include/openssl/sha.h"
        || ! -f "$openssl_prefix/lib/libcrypto.a" ) ]]; then
  echo "cache test requires OpenSSL headers and libcrypto under $openssl_prefix" >&2
  exit 2
fi

case "$architecture" in
  arm64) wamr_target=AARCH64 ;;
  x86_64) wamr_target=X86_64 ;;
  *)
    echo "unsupported local architecture: $architecture" >&2
    exit 1
    ;;
esac

case "$mode" in
  asan-ubsan)
    wamr_sanitizers=asan,ubsan
    compiler_sanitizers=address,undefined
    runtime_environment=(
      env
      ASAN_OPTIONS=detect_stack_use_after_return=1:abort_on_error=1
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    )
    ;;
  tsan)
    wamr_sanitizers=tsan
    compiler_sanitizers=thread
    runtime_environment=(env TSAN_OPTIONS=halt_on_error=1:abort_on_error=1)
    ;;
  none)
    wamr_sanitizers=
    compiler_sanitizers=
    runtime_environment=(env)
    ;;
  *)
    echo "usage: $0 [asan-ubsan|tsan|none] [build-dir]" >&2
    exit 2
    ;;
esac

cmake_args=(
  -S "$repo_root/product-mini/platforms/darwin"
  -B "$build_dir"
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_C_COMPILER=/usr/bin/clang
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++
  -DWAMR_BUILD_TARGET="$wamr_target"
  -DWAMR_BUILD_INTERP=1
  -DWAMR_BUILD_FAST_INTERP=1
  -DWAMR_BUILD_AOT=0
  -DWAMR_BUILD_JIT=0
  -DWAMR_BUILD_COMPONENT_MODEL=0
  -DWAMR_BUILD_LIBC_WASI=0
  -DWAMR_BUILD_LIBC_BUILTIN=0
  -DWAMR_BUILD_MULTI_MODULE="$multi_module"
  -DWAMR_BUILD_THREAD_MGR=0
  -DWAMR_BUILD_SIMD=0
  -DWAMR_BUILD_ALLOC_WITH_USER_DATA="$alloc_with_user_data"
  -DWAMR_BUILD_SANITIZER="$wamr_sanitizers"
)
if [[ "$wasm_cache" == 1 ]]; then
  cmake_args+=(
    "-DCMAKE_C_FLAGS:STRING=-DWASM_ENABLE_WASM_CACHE=1 -I$openssl_prefix/include"
  )
fi
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --target vmlib --parallel 4

compile_args=(
  -std=c11
  -O1
  -g
  -Wall
  -Wextra
  -Werror
  -fno-omit-frame-pointer
  -DWASM_ENABLE_INTERP=1
  -DWASM_ENABLE_FAST_INTERP=1
  -DWASM_ENABLE_MULTI_MODULE="$multi_module"
  -DWASM_MEM_ALLOC_WITH_USER_DATA="$alloc_with_user_data"
  -I "$repo_root/core/iwasm/include"
  -I "$repo_root/core/iwasm/common"
  -I "$repo_root/core/iwasm/interpreter"
  -I "$repo_root/core/shared/mem-alloc"
  -I "$repo_root/core/shared/utils"
  -I "$repo_root/core/shared/platform/include"
  -I "$repo_root/core/shared/platform/darwin"
)
link_args=("$build_dir/libiwasm.a" -lpthread -lm)
if [[ "$wasm_cache" == 1 ]]; then
  compile_args+=(-DWASM_ENABLE_WASM_CACHE=1)
  link_args+=("$openssl_prefix/lib/libcrypto.a")
fi
if [[ -n "$compiler_sanitizers" ]]; then
  compile_args+=("-fsanitize=$compiler_sanitizers")
  link_args+=("-fsanitize=$compiler_sanitizers")
fi

/usr/bin/clang "${compile_args[@]}" \
  "$test_dir/allocation_quota_test.c" "${link_args[@]}" \
  -o "$build_dir/allocation-quota-test"
"${runtime_environment[@]}" "$build_dir/allocation-quota-test"

/usr/bin/clang "${compile_args[@]}" \
  "$test_dir/allocation_quota_load_test.c" "${link_args[@]}" \
  -o "$build_dir/allocation-quota-load-test"
"${runtime_environment[@]}" "$build_dir/allocation-quota-load-test"

for component_model in 0 1; do
  for c_api_first in 0 1; do
    /usr/bin/clang -std=c11 -Wall -Wextra -Werror \
      -DWASM_ENABLE_COMPONENT_MODEL="$component_model" \
      -DLOAD_ARGS_INCLUDE_C_API_FIRST="$c_api_first" \
      -I "$repo_root/core/iwasm/include" \
      "$test_dir/load_args_abi_test.c" \
      -o "$build_dir/load-args-abi-c-$component_model-$c_api_first"
    "$build_dir/load-args-abi-c-$component_model-$c_api_first"

    /usr/bin/clang++ -x c++ -std=c++17 -Wall -Wextra -Werror \
      -DWASM_ENABLE_COMPONENT_MODEL="$component_model" \
      -DLOAD_ARGS_INCLUDE_C_API_FIRST="$c_api_first" \
      -I "$repo_root/core/iwasm/include" \
      "$test_dir/load_args_abi_test.c" \
      -o "$build_dir/load-args-abi-cxx-$component_model-$c_api_first"
    "$build_dir/load-args-abi-cxx-$component_model-$c_api_first"
  done
done
