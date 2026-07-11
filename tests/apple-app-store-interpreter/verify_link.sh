#!/usr/bin/env bash
# Copyright (C) 2026 Matt Hargett. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <component-smoke-executable>" >&2
  exit 2
fi

executable=$1
symbols=$(nm "$executable")

# Setup and teardown are cold operations.  The two functions used once per
# render quantum must be internalized/inlined by the final full-LTO link so the
# statically composed host pays no extra public adapter-call boundary.
if grep -Eq \
    '[[:space:]][Tt][[:space:]]+_?wasm_component_(call_prepared|prepared_call_post_return)$' \
    <<<"$symbols"; then
  echo "full LTO did not eliminate a hot prepared-call adapter boundary" >&2
  grep -E \
    '[[:space:]][Tt][[:space:]]+_?wasm_component_(call_prepared|prepared_call_post_return)$' \
    <<<"$symbols" >&2
  exit 1
fi

if grep -Eiq \
    '[[:space:]][Tt][[:space:]]+_?(wasm_interp_classic|aot_|jit_|fast_jit)' \
    <<<"$symbols"; then
  echo "final component smoke contains a forbidden runtime symbol" >&2
  exit 1
fi

echo "verified full-LTO component embedding link"
