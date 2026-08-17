;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $error-host
    (instance
      (export "error" (type (sub resource)))
      (type $error-borrow (borrow 0))
      (type $debug-string-type
        (func (param "self" $error-borrow) (result string)))
      (export "[method]error.to-debug-string"
        (func (type $debug-string-type)))))
  (import "wasi:io/error@0.2.6"
    (instance $errors (type $error-host)))
  (alias export $errors "error" (type $error))

  (type $poll-host
    (instance
      (export "pollable" (type (sub resource)))
      (type $pollable-borrow (borrow 0))
      (type $block-type (func (param "self" $pollable-borrow)))
      (export "[method]pollable.block" (func (type $block-type)))
      (type $ready-type
        (func (param "self" $pollable-borrow) (result bool)))
      (export "[method]pollable.ready" (func (type $ready-type)))
      (type $pollable-list (list $pollable-borrow))
      (type $index-list (list u32))
      (type $poll-type
        (func (param "in" $pollable-list) (result $index-list)))
      (export "poll" (func (type $poll-type)))))
  (import "wasi:io/poll@0.2.6"
    (instance $poll (type $poll-host)))
  (alias export $poll "pollable" (type $pollable))

  (type $streams-host
    (instance
      (export "input-stream" (type (sub resource)))
      (export "output-stream" (type (sub resource)))
      (alias outer 1 $error (type $outer-error))
      (export "error" (type (eq $outer-error)))
      ;; Import-facing function types must use the exported equality type,
      ;; not the hidden outer alias. The exported `error` is local type 3.
      (type $own-error (own 3))
      (type $stream-error
        (variant
          (case "last-operation-failed" $own-error)
          (case "closed")))
      (export "stream-error" (type (eq $stream-error)))
      (alias outer 1 $pollable (type $outer-pollable))
      (export "pollable" (type (eq $outer-pollable)))
      (type $input-borrow (borrow 0))
      (type $byte-list (list u8))
      ;; The exported `stream-error` equality type is local type 6.
      (type $read-result (result $byte-list (error 6)))
      (type $read-type
        (func
          (param "self" $input-borrow)
          (param "len" u64)
          (result $read-result)))
      (export "[method]input-stream.read" (func (type $read-type)))
      (export "[method]input-stream.blocking-read" (func (type $read-type)))
      (type $skip-result (result u64 (error 6)))
      (type $skip-type
        (func
          (param "self" $input-borrow)
          (param "len" u64)
          (result $skip-result)))
      (export "[method]input-stream.skip" (func (type $skip-type)))
      (export "[method]input-stream.blocking-skip" (func (type $skip-type)))
      ;; The exported `pollable` equality type is local type 8.
      (type $own-pollable (own 8))
      (type $subscribe-type
        (func (param "self" $input-borrow) (result $own-pollable)))
      (export "[method]input-stream.subscribe"
        (func (type $subscribe-type)))
      (type $output-borrow (borrow 1))
      (type $check-write-type
        (func (param "self" $output-borrow) (result $skip-result)))
      (export "[method]output-stream.check-write"
        (func (type $check-write-type)))
      (type $void-stream-result (result (error 6)))
      (type $write-type
        (func
          (param "self" $output-borrow)
          (param "contents" $byte-list)
          (result $void-stream-result)))
      (export "[method]output-stream.write" (func (type $write-type)))
      (export "[method]output-stream.blocking-write-and-flush"
        (func (type $write-type)))
      (type $flush-type
        (func (param "self" $output-borrow) (result $void-stream-result)))
      (export "[method]output-stream.flush" (func (type $flush-type)))
      (export "[method]output-stream.blocking-flush"
        (func (type $flush-type)))
      (type $output-subscribe-type
        (func (param "self" $output-borrow) (result $own-pollable)))
      (export "[method]output-stream.subscribe"
        (func (type $output-subscribe-type)))
      (type $write-zeroes-type
        (func
          (param "self" $output-borrow)
          (param "len" u64)
          (result $void-stream-result)))
      (export "[method]output-stream.write-zeroes"
        (func (type $write-zeroes-type)))
      (export "[method]output-stream.blocking-write-zeroes-and-flush"
        (func (type $write-zeroes-type)))
      (type $splice-type
        (func
          (param "self" $output-borrow)
          (param "src" $input-borrow)
          (param "len" u64)
          (result $skip-result)))
      (export "[method]output-stream.splice" (func (type $splice-type)))
      (export "[method]output-stream.blocking-splice"
        (func (type $splice-type)))))
  (import "wasi:io/streams@0.2.6"
    (instance $streams (type $streams-host)))
  (alias export $streams "input-stream" (type $input-stream))
  (alias export $streams "output-stream" (type $output-stream))

  (type $stdout-host
    (instance
      (alias outer 1 $output-stream (type $outer-output-stream))
      (export "output-stream" (type (eq $outer-output-stream)))
      (type $own-output-stream (own 1))
      (type $get-stdout-type (func (result $own-output-stream)))
      (export "get-stdout" (func (type $get-stdout-type)))))
  (import "wasi:cli/stdout@0.2.6"
    (instance $stdout (type $stdout-host)))

  (type $files-host
    (instance
      (export "selected-file" (type (sub resource)))
      (alias outer 1 $input-stream (type $outer-input-stream))
      (export "input-stream" (type (eq $outer-input-stream)))
      (type $file-borrow (borrow 0))
      ;; The exported `input-stream` equality type is local type 2.
      (type $own-input-stream (own 2))
      (type $contents-type
        (func (param "self" $file-borrow) (result $own-input-stream)))
      (export "[method]selected-file.contents"
        (func (type $contents-type)))))
  (import "w3c:file-system-access/read-only-file-picker@0.0.2"
    (instance $files (type $files-host)))
  (alias export $files "selected-file" (type $selected-file))

  (type $camera-host
    (instance
      (export "camera-barcode-scanner" (type (sub resource)))
      (alias outer 1 $pollable (type $outer-camera-pollable))
      (export "pollable" (type (eq $outer-camera-pollable)))
      (type $scanner-borrow (borrow 0))
      ;; The exported `pollable` equality type is local type 2.
      (type $own-camera-pollable (own 2))
      (type $event-pollable-type
        (func
          (param "self" $scanner-borrow)
          (result $own-camera-pollable)))
      (export "[method]camera-barcode-scanner.event-pollable"
        (func (type $event-pollable-type)))))
  (import "snqr:shape-detection-camera/live-camera-detection@0.0.1"
    (instance $camera (type $camera-host)))
  (alias export $camera "camera-barcode-scanner"
    (type $camera-barcode-scanner))

  (type $clock-host
    (instance
      (alias outer 1 $pollable (type $outer-clock-pollable))
      (export "pollable" (type (eq $outer-clock-pollable)))
      ;; The exported `pollable` equality type is local type 1.
      (type $own-clock-pollable (own 1))
      (type $subscribe-duration-type
        (func
          (param "when" u64)
          (result $own-clock-pollable)))
      (export "subscribe-duration"
        (func (type $subscribe-duration-type)))))
  (import "wasi:clocks/monotonic-clock@0.2.6"
    (instance $clock (type $clock-host)))

  ;; A test-only factory supplies owned handles for the two application
  ;; resources. The returned resource identities remain the exact W3C/SNQR
  ;; imported types above; only construction is fixture-specific.
  (type $factory-host
    (instance
      (alias outer 1 $selected-file (type $outer-selected-file))
      (export "selected-file" (type (eq $outer-selected-file)))
      (alias outer 1 $camera-barcode-scanner (type $outer-scanner))
      (export "camera-barcode-scanner" (type (eq $outer-scanner)))
      (type $own-selected-file (own 1))
      (type $make-file-type (func (result $own-selected-file)))
      (export "make-file" (func (type $make-file-type)))
      (type $own-scanner (own 3))
      (type $make-scanner-type (func (result $own-scanner)))
      (export "make-scanner" (func (type $make-scanner-type)))))
  (import "test:project/component-host-io@0.1.0"
    (instance $factory (type $factory-host)))

  (core module $memory-provider
    (memory (export "memory") 4 8)
    (global $next (mut i32) (i32.const 4096))
    (func (export "cabi_realloc")
      (param $old-ptr i32) (param $old-size i32)
      (param $align i32) (param $new-size i32) (result i32)
      (local $result i32)
      local.get $new-size
      i32.eqz
      if
        i32.const 0
        return
      end
      global.get $next
      local.get $align
      i32.const 1
      i32.sub
      i32.add
      local.get $align
      i32.const 1
      i32.sub
      i32.const -1
      i32.xor
      i32.and
      local.tee $result
      local.get $new-size
      i32.add
      global.set $next
      local.get $result))
  (core instance $memory-instance (instantiate $memory-provider))
  (alias core export $memory-instance "memory" (core memory $memory))
  (alias core export $memory-instance "cabi_realloc" (core func $realloc))
  (core instance $memory-export
    (export "memory" (memory $memory)))

  (core func $drop-file (canon resource.drop $selected-file))
  (core func $drop-scanner (canon resource.drop $camera-barcode-scanner))
  (core func $drop-input-stream (canon resource.drop $input-stream))
  (core func $drop-output-stream (canon resource.drop $output-stream))
  (core func $drop-pollable (canon resource.drop $pollable))
  (core instance $resource-functions
    (export "drop-file" (func $drop-file))
    (export "drop-scanner" (func $drop-scanner))
    (export "drop-input-stream" (func $drop-input-stream))
    (export "drop-output-stream" (func $drop-output-stream))
    (export "drop-pollable" (func $drop-pollable)))

  (alias export $factory "make-file" (func $make-file))
  (alias export $factory "make-scanner" (func $make-scanner))
  (core func $lowered-make-file (canon lower (func $make-file)))
  (core func $lowered-make-scanner (canon lower (func $make-scanner)))
  (core instance $lowered-factory
    (export "make-file" (func $lowered-make-file))
    (export "make-scanner" (func $lowered-make-scanner)))

  (alias export $files "[method]selected-file.contents" (func $contents))
  (core func $lowered-contents (canon lower (func $contents)))
  (core instance $lowered-files
    (export "[method]selected-file.contents" (func $lowered-contents)))

  (alias export $camera
    "[method]camera-barcode-scanner.event-pollable"
    (func $event-pollable))
  (core func $lowered-event-pollable
    (canon lower (func $event-pollable)))
  (core instance $lowered-camera
    (export "[method]camera-barcode-scanner.event-pollable"
      (func $lowered-event-pollable)))

  (alias export $streams "[method]input-stream.read" (func $read))
  (alias export $streams "[method]input-stream.blocking-read"
    (func $blocking-read))
  (alias export $streams "[method]input-stream.skip" (func $skip))
  (alias export $streams "[method]input-stream.blocking-skip"
    (func $blocking-skip))
  (alias export $streams "[method]input-stream.subscribe"
    (func $stream-subscribe))
  (alias export $streams "[method]output-stream.splice" (func $splice))
  (alias export $streams "[method]output-stream.blocking-splice"
    (func $blocking-splice))
  (core func $lowered-read
    (canon lower (func $read) (memory $memory) (realloc $realloc)))
  (core func $lowered-blocking-read
    (canon lower
      (func $blocking-read) (memory $memory) (realloc $realloc)))
  (core func $lowered-skip
    (canon lower (func $skip) (memory $memory)))
  (core func $lowered-blocking-skip
    (canon lower (func $blocking-skip) (memory $memory)))
  (core func $lowered-stream-subscribe
    (canon lower (func $stream-subscribe)))
  (core func $lowered-splice
    (canon lower (func $splice) (memory $memory)))
  (core func $lowered-blocking-splice
    (canon lower (func $blocking-splice) (memory $memory)))
  (core instance $lowered-streams
    (export "[method]input-stream.read" (func $lowered-read))
    (export "[method]input-stream.blocking-read"
      (func $lowered-blocking-read))
    (export "[method]input-stream.skip" (func $lowered-skip))
    (export "[method]input-stream.blocking-skip"
      (func $lowered-blocking-skip))
    (export "[method]input-stream.subscribe"
      (func $lowered-stream-subscribe))
    (export "[method]output-stream.splice" (func $lowered-splice))
    (export "[method]output-stream.blocking-splice"
      (func $lowered-blocking-splice)))

  (alias export $stdout "get-stdout" (func $get-stdout))
  (core func $lowered-get-stdout (canon lower (func $get-stdout)))
  (core instance $lowered-stdout
    (export "get-stdout" (func $lowered-get-stdout)))

  (alias export $poll "[method]pollable.block" (func $poll-block))
  (alias export $poll "[method]pollable.ready" (func $poll-ready))
  (alias export $poll "poll" (func $poll-list))
  (core func $lowered-poll-block (canon lower (func $poll-block)))
  (core func $lowered-poll-ready (canon lower (func $poll-ready)))
  (core func $lowered-poll-list
    (canon lower (func $poll-list) (memory $memory) (realloc $realloc)))
  (core instance $lowered-poll
    (export "[method]pollable.block" (func $lowered-poll-block))
    (export "[method]pollable.ready" (func $lowered-poll-ready))
    (export "poll" (func $lowered-poll-list)))

  (alias export $clock "subscribe-duration" (func $subscribe-duration))
  (core func $lowered-subscribe-duration
    (canon lower (func $subscribe-duration)))
  (core instance $lowered-clock
    (export "subscribe-duration" (func $lowered-subscribe-duration)))

  (core module $guest
    (import "env" "memory" (memory 4))
    (import "resources" "drop-file" (func $drop-file (param i32)))
    (import "resources" "drop-scanner" (func $drop-scanner (param i32)))
    (import "resources" "drop-input-stream"
      (func $drop-input-stream (param i32)))
    (import "resources" "drop-output-stream"
      (func $drop-output-stream (param i32)))
    (import "resources" "drop-pollable"
      (func $drop-pollable (param i32)))
    (import "test:project/component-host-io@0.1.0" "make-file"
      (func $make-file (result i32)))
    (import "test:project/component-host-io@0.1.0" "make-scanner"
      (func $make-scanner (result i32)))
    (import "w3c:file-system-access/read-only-file-picker@0.0.2"
      "[method]selected-file.contents"
      (func $contents (param i32) (result i32)))
    (import "snqr:shape-detection-camera/live-camera-detection@0.0.1"
      "[method]camera-barcode-scanner.event-pollable"
      (func $event-pollable (param i32) (result i32)))
    (import "wasi:io/streams@0.2.6" "[method]input-stream.read"
      (func $read (param i32 i64 i32)))
    (import "wasi:io/streams@0.2.6" "[method]input-stream.blocking-read"
      (func $blocking-read (param i32 i64 i32)))
    (import "wasi:io/streams@0.2.6" "[method]input-stream.skip"
      (func $skip (param i32 i64 i32)))
    (import "wasi:io/streams@0.2.6" "[method]input-stream.blocking-skip"
      (func $blocking-skip (param i32 i64 i32)))
    (import "wasi:io/streams@0.2.6" "[method]input-stream.subscribe"
      (func $stream-subscribe (param i32) (result i32)))
    (import "wasi:io/streams@0.2.6" "[method]output-stream.splice"
      (func $splice (param i32 i32 i64 i32)))
    (import "wasi:io/streams@0.2.6" "[method]output-stream.blocking-splice"
      (func $blocking-splice (param i32 i32 i64 i32)))
    (import "wasi:cli/stdout@0.2.6" "get-stdout"
      (func $get-stdout (result i32)))
    (import "wasi:io/poll@0.2.6" "[method]pollable.block"
      (func $poll-block (param i32)))
    (import "wasi:io/poll@0.2.6" "[method]pollable.ready"
      (func $poll-ready (param i32) (result i32)))
    (import "wasi:io/poll@0.2.6" "poll"
      (func $poll-list (param i32 i32 i32)))
    (import "wasi:clocks/monotonic-clock@0.2.6" "subscribe-duration"
      (func $subscribe-duration (param i64) (result i32)))

    (func $poll-two (param $first i32) (param $second i32) (result i32)
      i32.const 512
      local.get $first
      i32.store
      i32.const 516
      local.get $second
      i32.store
      i32.const 512
      i32.const 2
      i32.const 0
      call $poll-list
      i32.const 4
      i32.load
      i32.const 1
      i32.ne
      if
        unreachable
      end
      i32.const 0
      i32.load
      i32.load)

    (func (export "stream-explicit") (result i32)
      (local $file i32)
      (local $stream i32)
      (local $pollable i32)
      call $make-file
      local.tee $file
      call $contents
      local.set $stream
      local.get $stream
      i64.const 100000
      i32.const 32
      call $read
      local.get $stream
      i64.const 4
      i32.const 64
      call $blocking-read
      local.get $stream
      i64.const 2
      i32.const 96
      call $skip
      local.get $stream
      i64.const 2
      i32.const 128
      call $blocking-skip
      local.get $stream
      call $stream-subscribe
      local.tee $pollable
      call $poll-block
      local.get $pollable
      call $poll-ready
      local.get $pollable
      call $drop-pollable
      local.get $stream
      call $drop-input-stream
      local.get $file
      call $drop-file)

    (func (export "stream-implicit")
      (local $file i32)
      call $make-file
      local.tee $file
      call $contents
      drop
      local.get $file
      call $drop-file)

    (func (export "stream-overreport")
      (local $file i32)
      (local $stream i32)
      call $make-file
      local.tee $file
      call $contents
      local.tee $stream
      i64.const 8
      i32.const 160
      call $read
      local.get $stream
      i64.const 8
      i32.const 192
      call $skip
      local.get $stream
      call $drop-input-stream
      local.get $file
      call $drop-file)

    (func (export "stream-splice")
      (local $file i32)
      (local $stream i32)
      (local $output i32)
      call $make-file
      local.tee $file
      call $contents
      local.set $stream
      call $get-stdout
      local.set $output
      local.get $output
      local.get $stream
      i64.const 4
      i32.const 224
      call $splice
      local.get $output
      local.get $stream
      i64.const 4
      i32.const 256
      call $blocking-splice
      local.get $output
      call $drop-output-stream
      local.get $stream
      call $drop-input-stream
      local.get $file
      call $drop-file)

    (func (export "poll-always-ready") (result i32)
      (local $file i32)
      (local $stream i32)
      (local $ready-pollable i32)
      (local $timer i32)
      (local $index i32)
      call $make-file
      local.tee $file
      call $contents
      local.tee $stream
      call $stream-subscribe
      local.set $ready-pollable
      i64.const 10000000000
      call $subscribe-duration
      local.set $timer
      local.get $timer
      local.get $ready-pollable
      call $poll-two
      local.set $index
      local.get $timer
      call $drop-pollable
      local.get $ready-pollable
      call $drop-pollable
      local.get $stream
      call $drop-input-stream
      local.get $file
      call $drop-file
      local.get $index)

    (func (export "poll-wait") (result i32)
      (local $scanner i32)
      (local $host-pollable i32)
      (local $timer i32)
      (local $index i32)
      call $make-scanner
      local.tee $scanner
      call $event-pollable
      local.tee $host-pollable
      call $poll-ready
      if
        unreachable
      end
      i64.const 10000000000
      call $subscribe-duration
      local.set $timer
      local.get $timer
      local.get $host-pollable
      call $poll-two
      local.set $index
      local.get $host-pollable
      call $poll-ready
      i32.eqz
      if
        unreachable
      end
      local.get $host-pollable
      call $drop-pollable
      local.get $timer
      call $drop-pollable
      local.get $scanner
      call $drop-scanner
      local.get $index)

    (func (export "poll-block-wait") (result i32)
      (local $scanner i32)
      (local $host-pollable i32)
      call $make-scanner
      local.tee $scanner
      call $event-pollable
      local.tee $host-pollable
      call $poll-ready
      if
        unreachable
      end
      local.get $host-pollable
      call $poll-block
      local.get $host-pollable
      call $poll-ready
      i32.eqz
      if
        unreachable
      end
      local.get $host-pollable
      call $drop-pollable
      local.get $scanner
      call $drop-scanner
      i32.const 1)

    (func (export "poll-implicit")
      (local $scanner i32)
      call $make-scanner
      local.tee $scanner
      call $event-pollable
      drop
      local.get $scanner
      call $drop-scanner))
  (core instance $guest-instance
    (instantiate $guest
      (with "env" (instance $memory-export))
      (with "resources" (instance $resource-functions))
      (with "test:project/component-host-io@0.1.0"
        (instance $lowered-factory))
      (with "w3c:file-system-access/read-only-file-picker@0.0.2"
        (instance $lowered-files))
      (with "snqr:shape-detection-camera/live-camera-detection@0.0.1"
        (instance $lowered-camera))
      (with "wasi:io/streams@0.2.6" (instance $lowered-streams))
      (with "wasi:cli/stdout@0.2.6" (instance $lowered-stdout))
      (with "wasi:io/poll@0.2.6" (instance $lowered-poll))
      (with "wasi:clocks/monotonic-clock@0.2.6"
        (instance $lowered-clock))))

  (type $void-type (func))
  (type $result-type (func (result u32)))
  (alias core export $guest-instance "stream-explicit"
    (core func $stream-explicit-core))
  (func $stream-explicit (type $result-type)
    (canon lift (core func $stream-explicit-core)))
  (export "stream-explicit" (func $stream-explicit))
  (alias core export $guest-instance "stream-implicit"
    (core func $stream-implicit-core))
  (func $stream-implicit (type $void-type)
    (canon lift (core func $stream-implicit-core)))
  (export "stream-implicit" (func $stream-implicit))
  (alias core export $guest-instance "stream-overreport"
    (core func $stream-overreport-core))
  (func $stream-overreport (type $void-type)
    (canon lift (core func $stream-overreport-core)))
  (export "stream-overreport" (func $stream-overreport))
  (alias core export $guest-instance "stream-splice"
    (core func $stream-splice-core))
  (func $stream-splice (type $void-type)
    (canon lift (core func $stream-splice-core)))
  (export "stream-splice" (func $stream-splice))
  (alias core export $guest-instance "poll-always-ready"
    (core func $poll-always-ready-core))
  (func $poll-always-ready (type $result-type)
    (canon lift (core func $poll-always-ready-core)))
  (export "poll-always-ready" (func $poll-always-ready))
  (alias core export $guest-instance "poll-wait"
    (core func $poll-wait-core))
  (func $poll-wait (type $result-type)
    (canon lift (core func $poll-wait-core)))
  (export "poll-wait" (func $poll-wait))
  (alias core export $guest-instance "poll-block-wait"
    (core func $poll-block-wait-core))
  (func $poll-block-wait (type $result-type)
    (canon lift (core func $poll-block-wait-core)))
  (export "poll-block-wait" (func $poll-block-wait))
  (alias core export $guest-instance "poll-implicit"
    (core func $poll-implicit-core))
  (func $poll-implicit (type $void-type)
    (canon lift (core func $poll-implicit-core)))
  (export "poll-implicit" (func $poll-implicit)))
