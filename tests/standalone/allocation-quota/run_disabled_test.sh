#!/usr/bin/env bash
# Copyright (C) 2026 Rebecker Specialties. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Builds the runtime without WAMR_BUILD_ALLOCATION_QUOTA and checks that the
# stock allocator path works and that quota-requesting loads fail closed.

set -euo pipefail

test_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$test_dir/../../.." && pwd)
mode=${1:-asan-ubsan}
build_dir=${2:-$(mktemp -d "${TMPDIR:-/tmp}/wamr-allocation-quota-disabled-$mode.XXXXXX")}
architecture=$(uname -m)

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
  none)
    wamr_sanitizers=
    compiler_sanitizers=
    runtime_environment=(env)
    ;;
  *)
    echo "usage: $0 [asan-ubsan|none] [build-dir]" >&2
    exit 2
    ;;
esac

cmake \
  -S "$repo_root/product-mini/platforms/darwin" \
  -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DWAMR_BUILD_TARGET="$wamr_target" \
  -DWAMR_BUILD_INTERP=1 \
  -DWAMR_BUILD_FAST_INTERP=1 \
  -DWAMR_BUILD_AOT=0 \
  -DWAMR_BUILD_JIT=0 \
  -DWAMR_BUILD_COMPONENT_MODEL=0 \
  -DWAMR_BUILD_LIBC_WASI=0 \
  -DWAMR_BUILD_LIBC_BUILTIN=0 \
  -DWAMR_BUILD_MULTI_MODULE=0 \
  -DWAMR_BUILD_THREAD_MGR=0 \
  -DWAMR_BUILD_SIMD=0 \
  -DWAMR_BUILD_ALLOCATION_QUOTA=0 \
  -DWAMR_BUILD_SANITIZER="$wamr_sanitizers"
cmake --build "$build_dir" --target vmlib --parallel 4

if grep -Fq -- '-DWASM_ENABLE_ALLOCATION_QUOTA=1' "$build_dir/compile_commands.json"; then
  echo "allocation quota accounting unexpectedly enabled" >&2
  exit 1
fi

compile_args=(
  -std=c11
  -O1
  -g
  -Wall
  -Wextra
  -Werror
  -fno-omit-frame-pointer
  -I "$repo_root/core/iwasm/include"
)
link_args=("$build_dir/libiwasm.a" -lpthread -lm)
if [[ -n "$compiler_sanitizers" ]]; then
  compile_args+=("-fsanitize=$compiler_sanitizers")
  link_args+=("-fsanitize=$compiler_sanitizers")
fi

/usr/bin/clang "${compile_args[@]}" \
  "$test_dir/allocation_quota_disabled_test.c" "${link_args[@]}" \
  -o "$build_dir/allocation-quota-disabled-test"
"${runtime_environment[@]}" "$build_dir/allocation-quota-disabled-test"
