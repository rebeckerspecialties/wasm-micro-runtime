;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (core module $module
    (func (export "spin")
      (loop $again
        br $again)))
  (core instance $instance (instantiate $module))
  (alias core export $instance "spin" (core func $spin))
  (type $spin-type (func))
  (func $spin-func (type $spin-type) (canon lift (core func $spin)))
  (export "spin" (func $spin-func)))
