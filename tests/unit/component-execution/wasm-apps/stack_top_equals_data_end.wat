(module
  (memory (export "memory") 1 1)
  (global $__stack_pointer (mut i32) (i32.const 65536))
  (global $__data_end i32 (i32.const 65536))
  (global $__heap_base i32 (i32.const 65536))
  (export "__data_end" (global $__data_end))
  (export "__heap_base" (global $__heap_base))
  (func (export "answer") (result i32)
    i32.const 42))
