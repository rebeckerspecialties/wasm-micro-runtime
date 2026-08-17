(module
  (import "source" "table" (table 1 4 funcref))
  (import "source" "memory" (memory 1 4))
  (func
    (drop (table.grow (ref.null func) (i32.const 0)))
    (drop (memory.grow (i32.const 0)))))
