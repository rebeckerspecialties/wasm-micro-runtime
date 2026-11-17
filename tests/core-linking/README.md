# Instance-local C API linking regressions

Build this focused target with CMake and run CTest. It uses only the fast
interpreter and software bounds checks; no network, GPU, or full CI matrix is
needed. Run a second build with `-fsanitize=address,undefined` in both the C
compiler and executable linker flags.

`WAMR_BUILD_C_API_CORE_LINKING=1` is experimental and off by default. It binds
imports per instance, without mutating reusable module metadata or caller-owned
external wrappers. The first slice supports interpreter-exported, non-shared
wasm32 memories and host callbacks. Unsupported import kinds fail closed.
The normal C API import enumeration order is retained. This is a core linking
facility, not a Component Model parser, scheduler, or WASI implementation.

The store owns instantiated objects until store deletion, as in the existing
C API. Callback environment ownership is shared by function copies and retained
instance imports. A store and its objects remain thread-confined. Embedders must
bound store lifetime/admission rather than accumulating unbounded instances.

Tests cover active data segments, memory identity through re-export, growth in
either instance, maximum limits, start-time callbacks (including an imported
start function), duplicate instantiation with different environments, foreign
stores, missing imports, owner-wrapper deletion and exactly-once callback
finalization after the last instance reference.
