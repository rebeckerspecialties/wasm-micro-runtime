(module
  (import "host" "notify" (func $notify))
  (import "shared" "table" (table 1 1 funcref))
  (import "shared" "memory" (memory 1 1))
  (import "shared" "global" (global $global (mut i32)))

  (func $start
    i32.const 0
    global.get $global
    i32.store
    i32.const 42
    global.set $global
    call $notify)
  (start $start)

  (export "table" (table 0))
  (export "memory" (memory 0))
  (export "global" (global $global)))
