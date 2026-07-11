(module
  (memory (export "memory") 1 1)
  (table (export "table") 1 1 funcref)
  (global (export "global") (mut i32) (i32.const 41)))
