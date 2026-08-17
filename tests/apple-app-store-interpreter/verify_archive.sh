#!/usr/bin/env bash
# Copyright (C) 2026 Matt Hargett. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <build-directory> <archive> <architecture>" >&2
  exit 2
fi

build_dir=$1
library=$2
architecture=$3
compile_commands="$build_dir/compile_commands.json"

if [[ ! -f "$library" || ! -f "$compile_commands" ]]; then
  echo "missing archive or compile database for Apple profile verification" >&2
  exit 1
fi

lipo -info "$library" | grep -F "architecture: $architecture"

members=$(ar -t "$library")

require_member() {
  local pattern=$1
  local description=$2
  if ! grep -Eq "$pattern" <<<"$members"; then
    echo "missing $description in $library" >&2
    exit 1
  fi
}

reject_member() {
  local pattern=$1
  local description=$2
  if grep -Eiq "$pattern" <<<"$members"; then
    echo "unexpected $description in $library" >&2
    grep -Ei "$pattern" <<<"$members" >&2
    exit 1
  fi
}

require_member '(^|/)wasm_interp_fast\.c\.o$' "fast interpreter object"
require_member '(^|/)wasm_component_runtime\.c\.o$' "component runtime object"
require_member '(^|/)wasm_component_application\.c\.o$' "component application object"
require_member '(^|/)libc_wasi_p2_wrapper\.c\.o$' \
  "Preview 2 wrapper object"
require_member '(^|/)thread_manager\.c\.o$' "native thread-manager object"

reject_member '(^|/)wasm_interp_classic\.c\.o$' "classic interpreter object"
reject_member '(^|/)(aot_|jit_|fast_jit|llvm_jit|compilation)' \
  "AOT, JIT, or compiler object"
reject_member '(^|/)libc_wasi_wrapper\.c\.o$' "Preview 1 wrapper object"
reject_member '(^|/)(lib_pthread|lib_wasi_threads)' \
  "guest-thread library object"

if ! grep -Fq -- '-flto=full' "$compile_commands"; then
  echo "compile database does not contain the required -flto=full" >&2
  exit 1
fi
if grep -Fq -- '-flto=thin' "$compile_commands"; then
  echo "compile database unexpectedly enables ThinLTO" >&2
  exit 1
fi

require_compile_source() {
  local source=$1
  if ! grep -Fq "$source" "$compile_commands"; then
    echo "compile database is missing $source" >&2
    exit 1
  fi
}

reject_compile_source() {
  local source=$1
  if grep -Fq "$source" "$compile_commands"; then
    echo "compile database unexpectedly includes $source" >&2
    exit 1
  fi
}

require_compile_source '/interpreter/wasm_interp_fast.c'
require_compile_source '/common/component-model/'
require_compile_source '/libraries/thread-mgr/thread_manager.c'
require_compile_source '/libraries/libc-wasi-p2/libc_wasi_p2_wrapper.c'
reject_compile_source '/interpreter/wasm_interp_classic.c'
reject_compile_source '/aot/'
reject_compile_source '/compilation/'
reject_compile_source '/fast-jit/'
reject_compile_source '/libraries/libc-wasi/libc_wasi_wrapper.c'
reject_compile_source '/libraries/lib-pthread/'
reject_compile_source '/libraries/lib-wasi-threads/'

if ! grep -Fq -- '-DWASM_ENABLE_LIBC_WASI_P2=1' "$compile_commands"; then
  echo "compile database does not enable Preview 2" >&2
  exit 1
fi
if grep -Fq -- '-DWASM_ENABLE_LIBC_WASI=1' "$compile_commands"; then
  echo "compile database unexpectedly enables Preview 1" >&2
  exit 1
fi

echo "verified Preview 2-only wasm32-wasip2 fast-interpreter full-LTO archive"
