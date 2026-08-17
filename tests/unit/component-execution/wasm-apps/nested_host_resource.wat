;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $host-type
    (instance
      (export "item" (type (sub resource)))
      (type (own 0))
      (type (func (result 1)))
      (export "make" (func (type 2)))
      (type (borrow 0))
      (type (func (param "self" 3) (result u32)))
      (export "rep" (func (type 4)))))
  (import "test:nested/host@1.0.0"
    (instance $host (type $host-type)))

  (component $nested
    (type $host-type
      (instance
        (export "item" (type (sub resource)))
        (type (own 0))
        (type (func (result 1)))
        (export "make" (func (type 2)))
        (type (borrow 0))
        (type (func (param "self" 3) (result u32)))
        (export "rep" (func (type 4)))))
    (import "host" (instance $host (type $host-type)))

    (alias export $host "item" (type $item))
    (alias export $host "make" (func $make))
    (alias export $host "rep" (func $rep))
    (core func $lowered-make (canon lower (func $make)))
    (core func $lowered-rep (canon lower (func $rep)))
    (core func $drop-item (canon resource.drop $item))
    (core instance $lowered-host
      (export "make" (func $lowered-make))
      (export "rep" (func $lowered-rep))
      (export "drop" (func $drop-item)))

    (core module $guest
      (import "host" "make" (func $make (result i32)))
      (import "host" "rep" (func $rep (param i32) (result i32)))
      (import "host" "drop" (func $drop (param i32)))
      (func (export "round-trip") (result i32)
        (local $handle i32)
        (local $representation i32)
        call $make
        local.tee $handle
        call $rep
        local.set $representation
        local.get $handle
        call $drop
        local.get $representation))
    (core instance $guest-instance
      (instantiate $guest
        (with "host" (instance $lowered-host))))

    (type $round-trip-type (func (result u32)))
    (alias core export $guest-instance "round-trip"
      (core func $round-trip-core))
    (func $round-trip (type $round-trip-type)
      (canon lift (core func $round-trip-core)))
    (export "round-trip" (func $round-trip)))

  (instance $nested-instance
    (instantiate $nested
      (with "host" (instance $host))))
  (alias export $nested-instance "round-trip" (func $round-trip))
  (export "round-trip" (func $round-trip)))
