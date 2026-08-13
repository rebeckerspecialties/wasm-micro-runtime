;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (core module $first
    (memory 2 8))
  (core module $second
    (memory 2 8))
  (core instance $first-instance
    (instantiate $first))
  (core instance $second-instance
    (instantiate $second)))
