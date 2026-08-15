#!/usr/bin/env bash
# Copyright (C) 2026 Matt Hargett. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-asan-ubsan}
developer_dir=${DEVELOPER_DIR:-/Applications/Xcode-beta.app/Contents/Developer}
architecture=$(uname -m)
profile="$repo_root/tests/apple-app-store-interpreter/apple_wasip2_fast_interp.cmake"
fixture="$repo_root/tests/apple-app-store-interpreter/component_host_wasi_io.wasm"
realloc_fixture="$repo_root/tests/apple-app-store-interpreter/static_host_component.wasm"
bison_executable=${BISON_EXECUTABLE:-/opt/homebrew/opt/bison/bin/bison}

case "$architecture" in
  arm64) wamr_target=AARCH64 ;;
  x86_64) wamr_target=X86_64 ;;
  *)
    echo "unsupported local macOS architecture: $architecture" >&2
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
  *)
    echo "usage: $0 [asan-ubsan|tsan]" >&2
    exit 2
    ;;
esac

build_dir=${2:-"/tmp/wamr-component-host-io-$mode"}
library="$build_dir/libiwasm.a"

DEVELOPER_DIR="$developer_dir" cmake \
  -C "$profile" \
  -S "$repo_root/product-mini/platforms/darwin" \
  -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBISON_EXECUTABLE="$bison_executable" \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DCMAKE_OSX_ARCHITECTURES="$architecture" \
  -DWAMR_BUILD_TARGET="$wamr_target" \
  -DWAMR_BUILD_EXCE_HANDLING=0 \
  -DWAMR_BUILD_SIMD=0 \
  -DWAMR_BUILD_RELAXED_SIMD=0 \
  -DWAMR_BUILD_SANITIZER="$wamr_sanitizers"
DEVELOPER_DIR="$developer_dir" cmake --build "$build_dir" \
  --target vmlib --parallel 4

compile_flags=(
  -std=c11
  -O1
  -g
  -Wall
  -Wextra
  -Werror
  -fno-omit-frame-pointer
  "-fsanitize=$compiler_sanitizers"
  -DWASM_ENABLE_COMPONENT_MODEL=1
  -DWASM_ENABLE_INTERP=1
  -DWASM_ENABLE_FAST_INTERP=1
  -DWASM_ENABLE_LIBC_WASI_P2=1
  -DWASM_ENABLE_THREAD_MGR=1
  -I "$repo_root/core/iwasm/include"
  -I "$repo_root/core/iwasm/common"
  -I "$repo_root/core/iwasm/common/component-model"
  -I "$repo_root/core/iwasm/interpreter"
  -I "$repo_root/core/shared/mem-alloc"
  -I "$repo_root/core/shared/utils"
  -I "$repo_root/core/shared/platform/include"
  -I "$repo_root/core/shared/platform/darwin"
)
link_flags=(
  "$library"
  "-fsanitize=$compiler_sanitizers"
  -lpthread
  -lm
)

DEVELOPER_DIR="$developer_dir" xcrun --sdk macosx clang \
  "${compile_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_host_wasi_io_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-host-wasi-io-smoke"
"${runtime_environment[@]}" "$build_dir/component-host-wasi-io-smoke" \
  "$fixture"

if [[ "$mode" == asan-ubsan ]]; then
  DEVELOPER_DIR="$developer_dir" xcrun --sdk macosx clang \
    "${compile_flags[@]}" \
    "$repo_root/tests/apple-app-store-interpreter/component_host_import_smoke.c" \
    "${link_flags[@]}" -o "$build_dir/component-host-import-smoke"
  "${runtime_environment[@]}" "$build_dir/component-host-import-smoke" \
    "$realloc_fixture"
fi
