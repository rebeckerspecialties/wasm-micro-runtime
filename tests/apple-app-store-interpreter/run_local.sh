#!/usr/bin/env bash
# Copyright (C) 2026 Matt Hargett. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/apple-wasip2-fast-interp-local"}
profile="$repo_root/tests/apple-app-store-interpreter/apple_wasip2_fast_interp.cmake"
library="$build_dir/libiwasm.a"
fixture="$repo_root/tests/unit/component-execution/wasm-apps/add.wasm"
host_fixture="$repo_root/tests/apple-app-store-interpreter/static_host_component.wasm"
wasip2_fixture="$repo_root/tests/apple-app-store-interpreter/wasip2_command.wasm"
architecture=$(uname -m)
bison_executable=${BISON_EXECUTABLE:-}

if [[ -z "$bison_executable" ]]; then
  for candidate in /opt/homebrew/opt/bison/bin/bison \
                   /usr/local/opt/bison/bin/bison; do
    if [[ -x "$candidate" ]]; then
      bison_executable=$candidate
      break
    fi
  done
fi
if [[ -z "$bison_executable" ]]; then
  echo "component builds require Bison 3.0 or newer; install Homebrew bison" >&2
  exit 1
fi

case "$architecture" in
  arm64) wamr_target=AARCH64 ;;
  x86_64) wamr_target=X86_64 ;;
  *)
    echo "unsupported local macOS architecture: $architecture" >&2
    exit 1
    ;;
esac

cmake \
  -C "$profile" \
  -S "$repo_root/product-mini/platforms/darwin" \
  -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBISON_EXECUTABLE="$bison_executable" \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DCMAKE_OSX_ARCHITECTURES="$architecture" \
  -DWAMR_BUILD_TARGET="$wamr_target"
cmake --build "$build_dir" --target vmlib --config Release --parallel 4

"$repo_root/tests/apple-app-store-interpreter/verify_archive.sh" \
  "$build_dir" "$library" "$architecture"

common_flags=(
  -std=c11
  -O3
  -DNDEBUG
  -Wall
  -Wextra
  -Werror
  -flto=full
  -arch "$architecture"
  -I "$repo_root/core/iwasm/include"
)
component_flags=(
  -DWASM_ENABLE_COMPONENT_MODEL=1
  -DWASM_ENABLE_INTERP=1
  -DWASM_ENABLE_FAST_INTERP=1
  -DWASM_ENABLE_LIBC_WASI_P2=1
  -DWASM_ENABLE_THREAD_MGR=1
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
  -flto=full
  "-Wl,-dead_strip"
  -lpthread
  -lm
)

xcrun --sdk macosx clang "${common_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/smoke.c" \
  "${link_flags[@]}" -o "$build_dir/embed-smoke"

xcrun --sdk macosx clang "${common_flags[@]}" \
  "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-smoke"
"$repo_root/tests/apple-app-store-interpreter/verify_link.sh" \
  "$build_dir/component-smoke"

xcrun --sdk macosx clang "${common_flags[@]}" \
  "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_termination_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-termination-smoke"

xcrun --sdk macosx clang "${common_flags[@]}" \
  "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_host_import_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-host-import-smoke"

xcrun --sdk macosx clang "${common_flags[@]}" \
  "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_wasip2_command_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-wasip2-command-smoke"

xcrun --sdk macosx clang "${common_flags[@]}" \
  "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_wasip2_defaults_smoke.c" \
  "${link_flags[@]}" -o "$build_dir/component-wasip2-defaults-smoke"

xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/smoke.c" \
  -o "$build_dir/embed-smoke.plist"
xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_smoke.c" \
  -o "$build_dir/component-smoke.plist"
xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_termination_smoke.c" \
  -o "$build_dir/component-termination-smoke.plist"
xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_host_import_smoke.c" \
  -o "$build_dir/component-host-import-smoke.plist"
xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_wasip2_command_smoke.c" \
  -o "$build_dir/component-wasip2-command-smoke.plist"
xcrun --sdk macosx clang --analyze -Xanalyzer -analyzer-werror \
  "${common_flags[@]}" "${component_flags[@]}" \
  "$repo_root/tests/apple-app-store-interpreter/component_wasip2_defaults_smoke.c" \
  -o "$build_dir/component-wasip2-defaults-smoke.plist"

codesign --force --sign - --options runtime "$build_dir/embed-smoke"
codesign --force --sign - --options runtime "$build_dir/component-smoke"
codesign --force --sign - --options runtime \
  "$build_dir/component-termination-smoke"
codesign --force --sign - --options runtime \
  "$build_dir/component-host-import-smoke"
codesign --force --sign - --options runtime \
  "$build_dir/component-wasip2-command-smoke"
codesign --force --sign - --options runtime \
  "$build_dir/component-wasip2-defaults-smoke"
"$build_dir/embed-smoke"
"$build_dir/component-smoke" "$fixture"
"$build_dir/component-termination-smoke"
"$build_dir/component-host-import-smoke" "$host_fixture"
"$build_dir/component-wasip2-command-smoke" "$wasip2_fixture"
"$build_dir/component-wasip2-defaults-smoke" "$wasip2_fixture"
