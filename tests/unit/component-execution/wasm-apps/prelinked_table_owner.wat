(module
  (type $indirect (func (param i32) (result i32)))
  ;; Function index zero deliberately has the expected type but the wrong
  ;; behavior. Treating an initializer-relative table element as owner-relative
  ;; would call this function.
  (func $wrong-module (type $indirect) (param $value i32) (result i32)
    local.get $value
    i32.const 100
    i32.add)

  (table $shared (export "table") 1 1 funcref)
  (table $local 1 1 funcref)
  (elem (table $local) (i32.const 0) func $wrong-module)

  (func (export "call") (param $value i32) (result i32)
    local.get $value
    i32.const 0
    call_indirect $shared (type $indirect))

  (func (export "call-local") (param $value i32) (result i32)
    local.get $value
    i32.const 0
    call_indirect $local (type $indirect))

  ;; A foreign funcref cannot be represented on the non-GC operand stack
  ;; without losing its defining instance. Both propagation operations must
  ;; trap before the local table is modified.
  (func (export "get-foreign-and-store")
    i32.const 0
    i32.const 0
    table.get $shared
    table.set $local)

  (func (export "copy-foreign-to-local")
    i32.const 0
    i32.const 0
    i32.const 1
    table.copy $local $shared))
