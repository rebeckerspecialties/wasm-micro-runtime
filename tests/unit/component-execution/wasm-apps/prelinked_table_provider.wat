(module
  (type $indirect (func (param i32) (result i32)))
  (global $bias i32 (i32.const 7))

  (func (export "add-seven") (type $indirect)
        (param $value i32) (result i32)
    local.get $value
    global.get $bias
    i32.add)

  ;; Ordinary local tables must stay on WAMR's existing module-relative fast
  ;; path until another core instance actually borrows the table.
  (table 1 1 funcref)
  (elem (i32.const 0) func 0)
  (func (export "call-local") (param $value i32) (result i32)
    local.get $value
    i32.const 0
    call_indirect (type $indirect)))
