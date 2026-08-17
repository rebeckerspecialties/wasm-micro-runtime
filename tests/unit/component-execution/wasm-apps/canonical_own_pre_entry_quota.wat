;; Copyright (C) 2026 Rebecker Specialties. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $host-type
    (instance
      (export "item" (type (sub resource)))
      (type $own-item (own 0))
      (type $make-type (func (result 1)))
      (export "make" (func (type $make-type)))))
  (import "test:quota/host@1.0.0"
    (instance $host (type $host-type)))

  (alias export $host "item" (type $item))
  (alias export $host "make" (func $make))
  (core func $lowered-make (canon lower (func $make)))
  (core func $drop-item (canon resource.drop $item))

  (core module $producer
    (import "resource" "drop" (func $drop (param i32)))
    (func (export "consume")
      (param
        i32
        i64 i64 i64 i64 i64 i64 i64 i64
        i64 i64 i64 i64 i64 i64 i64)
      (result i32)
      (local
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32
        i32 i32 i32 i32 i32 i32 i32 i32)
      local.get 0
      call $drop
      i32.const 99))
  (core instance $resource-functions
    (export "drop" (func $drop-item)))
  (core instance $producer-instance
    (instantiate $producer
      (with "resource" (instance $resource-functions))))
  (alias core export $producer-instance "consume"
    (core func $consume-core))

  (type $consume-type
    (func
      (param "item" (own $item))
      (param "a01" u64) (param "a02" u64)
      (param "a03" u64) (param "a04" u64)
      (param "a05" u64) (param "a06" u64)
      (param "a07" u64) (param "a08" u64)
      (param "a09" u64) (param "a10" u64)
      (param "a11" u64) (param "a12" u64)
      (param "a13" u64) (param "a14" u64)
      (param "a15" u64)
      (result u32)))
  (func $consume (type $consume-type)
    (canon lift (core func $consume-core)))
  (core func $lowered-consume (canon lower (func $consume)))

  (core instance $adapter
    (export "make" (func $lowered-make))
    (export "consume" (func $lowered-consume)))
  (core module $consumer
    (import "adapter" "make" (func $make (result i32)))
    (import "adapter" "consume"
      (func $consume
        (param
          i32
          i64 i64 i64 i64 i64 i64 i64 i64
          i64 i64 i64 i64 i64 i64 i64)
        (result i32)))
    (func (export "run") (result i32)
      call $make
      i64.const 1
      i64.const 2
      i64.const 3
      i64.const 4
      i64.const 5
      i64.const 6
      i64.const 7
      i64.const 8
      i64.const 9
      i64.const 10
      i64.const 11
      i64.const 12
      i64.const 13
      i64.const 14
      i64.const 15
      call $consume))
  (core instance $consumer-instance
    (instantiate $consumer
      (with "adapter" (instance $adapter))))
  (alias core export $consumer-instance "run" (core func $run-core))
  (type $run-type (func (result u32)))
  (func $run (type $run-type) (canon lift (core func $run-core)))
  (export "run" (func $run)))
