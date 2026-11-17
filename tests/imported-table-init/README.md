# Imported table.init regression

Configure a Debug build, build, and run CTest. Repeat with
`-DWAMR_BUILD_FAST_INTERP=0` for the classic interpreter. No component support,
multi-module linker, WASI library, network, or CI matrix is required.

An imported table contributes to the instance's total table count, but not the
module's defined-table count. A passive element segment initialized into table
zero must execute normally; an out-of-range destination must still trap.
