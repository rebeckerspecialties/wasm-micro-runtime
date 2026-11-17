(module
  (import "shim" "table" (table 5 32 funcref))
  (func $escaped (result i32) i32.const 55)
  (elem (i32.const 4) $escaped)
  (func $start unreachable)
  (start $start))
