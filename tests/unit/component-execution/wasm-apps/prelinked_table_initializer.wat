(module
  (type $indirect (func (param i32) (result i32)))
  (import "provider" "add-seven" (func $add-seven (type $indirect)))
  (import "owner" "table" (table $table 1 1 funcref))

  (table $local 1 1 funcref)
  (elem (table $local) (i32.const 0) func $add-seven)

  ;; This mirrors the Rust Preview 2 adapter: a later core instance populates
  ;; an earlier core's borrowed table with its own imported functions.
  (elem (i32.const 0) func $add-seven)

  ;; Exercise the mutation paths that must keep per-element provenance in
  ;; sync after component instantiation.
  (func (export "mutate-table")
    (table.fill $table (i32.const 0) (ref.null func) (i32.const 1))
    (table.set $table (i32.const 0) (ref.func $add-seven))
    (table.copy $table $table (i32.const 0) (i32.const 0) (i32.const 1)))

  ;; A local source has no provenance sidecar. Copying into the borrowed table
  ;; must translate its module-relative index into an initializer-owned ref.
  (func (export "copy-local-to-borrowed")
    (table.copy $table $local (i32.const 0) (i32.const 0) (i32.const 1)))

  ;; Entries owned by this module can safely return from the borrowed table to
  ;; a local table after the raw index and provenance pointer are cross-checked.
  (func (export "copy-borrowed-to-local")
    (table.copy $local $table (i32.const 0) (i32.const 0) (i32.const 1)))

  (func (export "call-local") (param $value i32) (result i32)
    local.get $value
    i32.const 0
    call_indirect $local (type $indirect)))
