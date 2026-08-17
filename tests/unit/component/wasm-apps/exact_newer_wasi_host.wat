;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $random-host
    (instance
      (type $get-random-u64 (func (result u64)))
      (export "get-random-u64" (func (type $get-random-u64)))))
  (import "wasi:random/random@0.3.0"
    (instance (type $random-host))))
