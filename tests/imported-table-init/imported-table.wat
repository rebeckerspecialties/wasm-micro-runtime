(module
  (type $value (func (result i32)))
  (import "env" "table" (table 1 1 funcref))
  (func $value (result i32) i32.const 42)
  (elem $values func $value)
  (func (export "run") (result i32)
    i32.const 0 i32.const 0 i32.const 1 table.init $values
    i32.const 0 call_indirect (type $value))
  (func (export "out-of-bounds")
    i32.const 1 i32.const 0 i32.const 1 table.init $values))
