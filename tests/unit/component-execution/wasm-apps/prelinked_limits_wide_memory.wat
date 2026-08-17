(module
  (table (export "table") 2 3 funcref)
  (memory (export "memory") 2 5)
  (func
    (drop (table.grow (ref.null func) (i32.const 0)))
    (drop (memory.grow (i32.const 0)))))
