# AGENTS.md — WAMR fork: WASIp2 / component-model + Apple integration

Dev guide for the `rebeckerspecialties/wasm-micro-runtime` fork's WASI-Preview-2
work. This lineage is SEPARATE from the fork's `feat/legacy-eh-*` and
`feat/relaxed-simd-*` branches (those rebase on upstream `cd390ea0`). The
parent benchmark repo's `AGENTS.md` (`../AGENTS.md` when this is a submodule)
has the cross-runtime / Apple-app context.

## Branch & PR map

All branch from the airbus fork's `dev/cm_wasip2_complete @ 2815b698` (pulled in
as the base; it adds component-model + WASIp2 + the WAVE text parser). Airbus
contact: Mihai Dimoiu (component-model author).

- **#10 `feat/wasip2-apple-port`** — ports the Linux-only WASIp2 host layer
  (`core/iwasm/libraries/libc-wasi-p2/`) to Apple platforms, all `#if
  defined(__APPLE__)`: kqueue+EVFILT_TIMER (vs timerfd), getentropy (vs
  getrandom), openat+O_NOFOLLOW_ANY (vs openat2/RESOLVE_BENEATH), st_*timespec,
  fsync, posix_fadvise no-op, SO_NOSIGPIPE, socket+fcntl (vs SOCK_CLOEXEC/
  accept4/pipe2), TCP_KEEPALIVE, local recvmsg/sendmsg loops (vs recvmmsg),
  IOV_MAX, SO_SNDLOWAT permit. PLUS the base-CI fixes below.
- **#9 `integration/cm-wasip2-all`** — the integration PR: #10 + #8 + #7 +
  relaxed-SIMD + legacy-EH + fuzz, all merged. Keep this branch carrying every
  fix (sync after each change to #10).
- **#8 `feat/component-conformance-traps`** — canonical-ABI trap coverage +
  ptr+len overflow fix (`wasm_component_canonical.c`, 3 sites widen operands
  before add) + `tests/unit/canonical-abi/test_canonical_abi_traps.cc`.
- **#7 `feat/component-parser-fuzz`** — `tests/fuzz/component-fuzz/` libFuzzer
  target + 4 memory-safety fixes.

## Base-CI regressions FIXED vs upstream (don't re-investigate)

The airbus base is RED on its own CI; these were all green on
`bytecodealliance` upstream but red on the base. Each verified by reading the CI
job log + comparing the same-named job on upstream `main`. **Method that worked:
`gh run view --job <id> --log`, then diff the suspect function vs
`gh api .../contents/<file> | base64 -d`.**

1. **`product-mini/.../main.c` `error_buf` truncation** — `execute_wasm_module`
   took `char *error_buf` and passed `sizeof(error_buf)` (=8) to
   `wasm_runtime_load`/`instantiate`; trap messages truncated to "WASM mo", so
   the spec-test harness never matched expected trap text → data/elem/start
   failed in ALL interp/jit modes + ba-issues regression. Fixed by threading a
   real `error_buf_size` param. THE spec-test killer.
2. **main.c AoT CLI dispatch gap** — the base's new `wasm_decode_header` +
   `is_wasm_module`/`is_wasm_component` dispatch had NO AoT branch → `iwasm
   foo.aot` → "Unknown WASM file type" exit 255. Fixed: `|| get_package_type(...)
   == Wasm_Module_AoT`. Cleared wasi-threads/debug-tools samples + ba-issues AoT.
3. **main.c unconditional `#include "wasm.h"`** — broke all AOT-only (INTERP=0)
   builds with "wasm.h file not found". Fixed: move inside the COMPONENT_MODEL
   guard (it's only needed there).
4. **main.c instantiation print** — used `LOG_ERROR` (bh_log "[time-tid]:"
   banner pollutes spec-test stdout); changed to `printf` to match upstream.
5. **`core/iwasm/common/wave-parser/wave_parser.cmake`** — passed bison
   `--feature=caret` (Bison ≥2.6) unconditionally; gated on `BISON_VERSION`.
   ALSO `wave_parser.y` uses `%code requires` (Bison 2.4+) in the GRAMMAR source
   — macOS runner ships Bison 2.3, so `compilation_on_macos.yml`'s
   `build_samples_wasm_c_api` job gets a `brew install bison` + PATH step.
6. **`tests/unit/{linear-memory-aot,runtime-common}/build_aot.sh`** — used
   `command -v wat2wasm`; CI installs WABT to `/opt/wabt/bin` (not on PATH).
   Fixed: prefer `/opt/wabt/bin/wat2wasm`, fall back to PATH (matches upstream).

## CI state & gates

- **June 18 head: 454 passed, 4 failed, 1 cancelled.** Three failing AOT jobs
  share the `align.wast` "stack size does not match block type" failure. The
  X86_32 unit job is missing generated WASIp2 component fixtures, and the
  X86_64 unit job was cancelled after the canonical UDP server test ran for six
  hours. macOS, Windows, SGX, Zephyr, NuttX, and coding-guideline jobs passed.
  Treat these as branch CI debt, not as infrastructure failures.
- **Gates**: `compilation` workflows (`-Werror`), `Coding Guidelines`
  (`ci/coding_guidelines_check.py` runs `git-clang-format-14 --diff` — only
  CHANGED lines, so pre-existing violations don't count; format with
  `/opt/homebrew/opt/llvm@14/bin/clang-format -i --style=file`). `pull_request`
  fires CI; matrices use `fail-fast` (one failure cancels siblings → fix the
  earliest real failure first).
- **Gate decision (user, 2026-06-14)**: interpreter-only green is enough; ignore
  pre-existing AOT-path base failures the interpreter app doesn't exercise.

## Building on macOS

```sh
# wasip2 + Apple host port (component model + WASIp2):
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH"
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
  cmake -S product-mini/platforms/darwin -B build \
  -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=0 \
  -DWAMR_BUILD_COMPONENT_MODEL=1 -DWAMR_BUILD_LIBC_WASI=1 -DWAMR_BUILD_LIBC_BUILTIN=1
cmake --build build -j8        # -> build/iwasm (2.4.3), build/libiwasm.a
```

Gotchas: the `darwin` platform CMakeLists does NOT default COMPONENT_MODEL on
(unlike top-level/linux); `WASM_ENABLE_COMPONENT_MODEL` defaults 0 in
`core/config.h`. `SIMD + CLASSIC_INTERP` and `MEMORY64 + FAST_INTERP` are
rejected by `build-scripts/unsupported_combination.cmake` — use FAST_INTERP
with SIMD, or disable SIMD for classic. The component model requires an
interpreter (calls `wasm_instantiate`); pure AOT-only + component can't link.

## App Store core-wasm embedding profile

CapsHost is core wasm with custom preview2-style imports. It is not component
execution, so its first WAMR build keeps `WAMR_BUILD_COMPONENT_MODEL`,
`WAMR_BUILD_LIBC_WASI`, and `WAMR_BUILD_LIBC_BUILTIN` off. The production shape
is a static fast-interpreter archive with multi-module, SIMD/relaxed-SIMD,
legacy-EH parsing, custom names, and call stacks enabled; all AOT/JIT/compiler
paths stay off. Enable `WAMR_BUILD_THREAD_MGR=1` solely so asynchronous
termination is observed inside pure-Wasm loops; keep guest pthreads, WASI
threads, and shared memory disabled.

Set `WAMR_DISABLE_HW_BOUND_CHECK=1` on every Apple target. This keeps software
linear-memory checks while avoiding WAMR's process-global SIGSEGV/SIGBUS
handlers; `sigaltstack` is unavailable on tvOS/watchOS. Non-executable macOS
mappings must not carry `MAP_JIT`, so this profile does not need the hardened
runtime JIT entitlement. Also set `WAMR_DISABLE_WAKEUP_BLOCKING_OP=1`; CapsHost
owns cancellation/wakeup for its native asynchronous imports.

`.github/workflows/apple_app_store_interpreter.yml` cross-builds that exact
archive for macOS arm64, iOS arm64, tvOS arm64, watchOS arm64_32, and visionOS
arm64 under `-Werror`. It rejects AOT/JIT/compiler objects and runs a hardened
macOS embedding smoke that covers runtime initialization, a native import with
an attachment, module load/instantiate, an exported function call, and
asynchronous termination of an infinite guest loop.

WAMR's multi-module reader and module registry are process-global and keyed only
by logical module name. Until WAMR gains scoped resolver contexts and graph
teardown, use one serialized WAMR session per active guest root: retain every
root/worklet wasm buffer, load all worklets from the same immutable namespace,
join/deinstantiate them before `wasm_runtime_destroy()`, and reject a logical
dependency name that maps to different bytes. Put host state on explicit exec
environment user data; submodule instance custom data is not inherited.

## Constraints

- App-Store target is **interpreter-only**: no JIT / AOT / MAP_JIT anywhere.
- Patch-stack discipline: tidy, well-named commits for upstreaming.
- **Do NOT mention Claude or Anthropic** in commit messages, PR text, or branch
  names.
