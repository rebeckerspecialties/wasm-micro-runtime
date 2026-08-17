;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $observe-type (func (result u32)))
  (type $host-type
    (instance
      (export "item" (type (sub resource)))
      (export "observe" (func (type $observe-type)))))
  (import "test:context/host@1.0.0"
    (instance $host (type $host-type)))

  (alias export $host "observe" (func $observe))
  (core func $lowered-observe (canon lower (func $observe)))
  (core instance $lowered-host
    (export "observe" (func $lowered-observe)))

  (core module $guest
    (import "host" "observe" (func $observe (result i32)))
    (func (export "call") (result i32)
      call $observe))
  (core instance $guest-instance
    (instantiate $guest
      (with "host" (instance $lowered-host))))

  (alias core export $guest-instance "call" (core func $call))
  (func $lifted-call (type $observe-type) (canon lift (core func $call)))
  (export "call" (func $lifted-call)))
