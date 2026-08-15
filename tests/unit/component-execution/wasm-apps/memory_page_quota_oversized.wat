;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (core module $guest
    (memory 3 3))
  (core instance $guest-instance
    (instantiate $guest)))
