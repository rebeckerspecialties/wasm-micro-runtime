(module
  (memory (export "memory") 1 3)
  (func (export "read") (param i32) (result i32)
    local.get 0 i32.load8_u)
  (func (export "grow") (param i32) (result i32)
    local.get 0 memory.grow))
