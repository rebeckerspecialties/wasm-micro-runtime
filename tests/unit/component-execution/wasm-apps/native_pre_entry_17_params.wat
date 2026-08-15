;; Copyright (C) 2026 Rebecker Specialties. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(module
  (type $target-type
    (func
      (param
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32 i32)))
  (import "test:quota/native" "target"
    (func $target (type $target-type)))
  (export "target" (func $target)))
