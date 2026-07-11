;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $bump-type (func (param "value" s32) (result s32)))
  (type $host-type
    (instance
      (export "bump" (func (type $bump-type)))))
  (import "test:project/static-host@0.1.0"
    (instance $host (type $host-type)))

  (alias export $host "bump" (func $bump))
  (core func $lowered-bump (canon lower (func $bump)))
  (core instance $lowered-host
    (export "bump" (func $lowered-bump)))

  (core module $guest
    (import "host" "bump" (func $bump (param i32) (result i32)))
    (func (export "call") (param $value i32) (result i32)
      local.get $value
      call $bump))
  (core instance $guest-instance
    (instantiate $guest
      (with "host" (instance $lowered-host))))

  (alias core export $guest-instance "call" (core func $call))
  (func $lifted-call (type $bump-type) (canon lift (core func $call)))
  (export "call" (func $lifted-call)))
