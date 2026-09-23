#!/usr/bin/env bash
# Copyright (C) 2026 Rebecker Specialties. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

test_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$test_dir/../../.." && pwd)
mode=${1:-none}
build_dir=${2:-$(mktemp -d "${TMPDIR:-/tmp}/wamr-page-quota-$mode.XXXXXX")}

case "$(uname -m)" in
  arm64) wamr_target=AARCH64 ;;
  x86_64) wamr_target=X86_64 ;;
  *) echo "unsupported local architecture" >&2; exit 1 ;;
esac

case "$mode" in
  asan-ubsan)
    wamr_sanitizers=asan,ubsan
    compiler_sanitizers=address,undefined
    runtime_environment=(env ASAN_OPTIONS=detect_stack_use_after_return=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1)
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
  *) echo "usage: $0 [none|asan-ubsan|tsan] [build-dir]" >&2; exit 2 ;;
esac

cmake -S "$repo_root/product-mini/platforms/darwin" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DWAMR_BUILD_TARGET="$wamr_target" \
  -DWAMR_BUILD_INTERP=1 \
  -DWAMR_BUILD_FAST_INTERP=1 \
  -DWAMR_BUILD_AOT=0 \
  -DWAMR_BUILD_JIT=0 \
  -DWAMR_BUILD_LIBC_WASI=0 \
  -DWAMR_BUILD_LIBC_BUILTIN=0 \
  -DWAMR_BUILD_MULTI_MODULE=0 \
  -DWAMR_BUILD_THREAD_MGR=0 \
  -DWAMR_BUILD_SIMD=0 \
  -DWAMR_BUILD_SANITIZER="$wamr_sanitizers"
cmake --build "$build_dir" --target vmlib --parallel 4

compile_args=(
  -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer
  -DWASM_ENABLE_INTERP=1 -DWASM_ENABLE_FAST_INTERP=1
  -I "$repo_root/core/iwasm/include"
  -I "$repo_root/core/iwasm/common"
  -I "$repo_root/core/iwasm/interpreter"
  -I "$repo_root/core/shared/mem-alloc"
  -I "$repo_root/core/shared/utils"
  -I "$repo_root/core/shared/platform/include"
  -I "$repo_root/core/shared/platform/darwin"
)
link_args=("$build_dir/libiwasm.a" -lpthread -lm)
if [[ -n "$compiler_sanitizers" ]]; then
  compile_args+=("-fsanitize=$compiler_sanitizers")
  link_args+=("-fsanitize=$compiler_sanitizers")
fi

/usr/bin/clang "${compile_args[@]}" "$test_dir/memory_page_quota_test.c" \
  "${link_args[@]}" -o "$build_dir/memory-page-quota-test"
"${runtime_environment[@]}" "$build_dir/memory-page-quota-test"
