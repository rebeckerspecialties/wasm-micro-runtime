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

  (core module $memory-provider
    (memory (export "memory") 1))
  (core instance $memory-instance
    (instantiate $memory-provider))
  (alias core export $memory-instance "memory" (core memory $memory))
  (core instance $memory-export
    (export "memory" (memory $memory)))

  (core func $lowered-bump
    (canon lower (func $bump) (memory $memory)))
  (core instance $lowered-host
    (export "bump" (func $lowered-bump)))

  (core module $guest
    (import "env" "memory" (memory 1))
    (import "host" "bump" (func $bump (param i32) (result i32)))
    (data (i32.const 32) "wamr")
    (func (export "call") (param $value i32) (result i32)
      local.get $value
      call $bump))
  (core instance $guest-instance
    (instantiate $guest
      (with "env" (instance $memory-export))
      (with "host" (instance $lowered-host))))

  (alias core export $guest-instance "call" (core func $call))
  (func $lifted-call (type $bump-type) (canon lift (core func $call)))
  (export "call" (func $lifted-call)))
