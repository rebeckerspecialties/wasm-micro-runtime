# Component Model / WASIp2 conformance plan

The bespoke gtest unit tests verify that a given constructor/layout produces
the value the test author hand-wrote. They almost never verify that a malformed
or hostile input **traps**, and they have no independent oracle: a systematic
encoding bug implemented identically in the production code and in the test's
expected value passes forever. This plan tracks closing both gaps, prioritized
for an interpreter-only on-device runtime (no JIT/CFI safety net).

## Status

| Increment | State |
|-----------|-------|
| Canonical-ABI bounds/alignment/overflow trap tests | **this PR** (`tests/unit/canonical-abi/test_canonical_abi_traps.cc`) |
| ptr+len overflow fix in the bounds checks | **this PR** (`wasm_component_canonical.c`: widen `load_int` / `load_string_from_range` / `load_list_from_range` before the add) |
| Invalid enum/variant discriminant trap tests | next |
| Resource-handle safety traps (unknown-handle, wrong-type, lift-own-from-borrow, remove-owned-while-borrowed) | next |
| UTF-16 / Latin1+UTF-16 transcode round-trip + surrogate/NaN-canonicalization coverage | next |
| Differential canonical-ABI execution vs wasmtime deterministic mode | planned |
| Official component-model `.wast` conformance runner | planned (needs a text→binary component encoder; WAMR loads binary only) |

## Differential oracle (the highest-value item)

Lift/lower the same byte buffers through both WAMR and wasmtime
(`Config::relaxed_simd_deterministic`-style deterministic component mode) and
assert (a) byte-identical lifted values and (b) identical trap/no-trap
outcomes. This converts every hand-authored constant into an oracle-checked
value and simultaneously lands most of the trap-coverage gaps (wasmtime traps
where WAMR must also trap). Same pattern already used for relaxed-SIMD in the
benchmark repo's `relaxed_simd_diff_fuzz` harness.

## Why these specific traps matter on-device

Every canonical-ABI `trap_if` guard is the boundary between a clean trap and a
wild read/write of linear memory. With no JIT/CFI on iOS/watchOS, an unchecked
ptr+len overflow or an out-of-range variant discriminant is a direct path from
a crafted component binary to silent memory corruption. The trap tests in this
PR are the cheapest high-value coverage because they target that path directly
and need no second runtime.
