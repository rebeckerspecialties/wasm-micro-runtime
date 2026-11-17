# Instance-local C API linking regressions

Build this focused target with CMake and run CTest. It uses only the fast
interpreter and software bounds checks; no network, GPU, or full CI matrix is
needed. Run a second build with `-fsanitize=address,undefined` in both the C
compiler and executable linker flags, `-fno-sanitize=alignment` in the compiler
flags, and `UBSAN_OPTIONS=halt_on_error=1` for CTest. The alignment exclusion is
explicit: WAMR's existing fast bytecode and four-byte frame layout use unaligned
native accesses on AArch64; the table cases expose those existing diagnostics.

`WAMR_BUILD_C_API_CORE_LINKING=1` is experimental and off by default. It binds
imports per instance, without mutating reusable module metadata or caller-owned
external wrappers. The profile supports interpreter-exported, non-shared
wasm32 memories, tables, guest functions and host callbacks. Global imports
still fail closed. Tail-call, GC and AOT/JIT combinations
are rejected at configuration time; this is not a claim of support for all
WAMR feature combinations. Function counts are bounded by the existing C API's
16-bit function-wrapper index representation.
The normal C API import enumeration order is retained. This is a core linking
facility, not a Component Model parser, scheduler, or WASI implementation.

The integration branch can also build `-DWAMR_TEST_LEGACY_LINKING=ON` with
the old component and module-name linkers enabled. C API modules still use
explicit imports, never name-registry resolution. This lane runs the same
core-linking tests plus legacy borrowed-table identity/provenance, mutations
and both destruction orders. It preserves the old product path during parity;
it is not an upstream dependency on the Airbus component implementation.

The store owns instantiated objects until store deletion, as in the existing
C API. Callback environment ownership is shared by function copies and retained
instance imports. A store and its objects remain thread-confined. Embedders must
bound store lifetime/admission rather than accumulating unbounded instances.
Function references use store-scoped identities, including table.get/set,
active/passive element segments, globals, and indirect calls. Table backing
storage grows geometrically on demand rather than reserving the declared
maximum at instantiation. Failed instantiations retain their allocated objects:
active element segments may already have published callable references, and a
later start trap does not roll back those store effects.

Tests cover active data segments, memory identity through re-export, growth in
either instance, maximum limits, start-time callbacks (including an imported
start function), duplicate instantiation with different environments, foreign
stores, missing imports, owner-wrapper deletion and exactly-once callback
finalization after the last instance reference.
Table cases additionally exercise the real shim/fixup pattern, host and guest
imports in another instance's table, owner-local mutable globals, indirect-call
type mismatch, passive initialization, growth/re-export visibility, C API
reference transfer, and calling a function published by a trapping start.
They also cover table.get/set, table.fill, table.copy, growth failure and
separate instantiations of the same table-owning module.

# Instance policy

`instance_policy_test` passes the existing opaque `InstantiationArgs2` through
the C API and checks aggregate initial/grown page accounting and refunds. A
store-local lifecycle observer admits each runtime identity before its start
function, and retracts it before destruction. The host owns the interrupt
mutex, thread and policy; no scheduler is added to WAMR. Twenty spinning-start
iterations keep an interrupter alive across store teardown to exercise the
retirement fence. Rejected admission has no retirement callback; a failed start
may be retained by the store until deletion because its references can have
escaped into an imported table.
