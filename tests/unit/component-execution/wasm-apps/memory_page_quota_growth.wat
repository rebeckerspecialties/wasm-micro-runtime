;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (core module $guest
    (memory 1 8)
    (func (export "grow") (param i32) (result i32)
      local.get 0
      memory.grow))
  (core instance $guest-instance
    (instantiate $guest))
  (alias core export $guest-instance "grow" (core func $grow))
  (type $grow-type (func (param "pages" s32) (result s32)))
  (func $lifted-grow (type $grow-type)
    (canon lift (core func $grow)))
  (export "grow" (func $lifted-grow)))
