;; Copyright (C) 2026 Matt Hargett. All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

(component
  (type $host-interface
    (instance
      (export "item" (type (sub resource)))
      (type $use-type
        (func (param "item" (borrow 0))))
      (export "use-item" (func (type $use-type)))))

  (import "test:alias/first@1.0.0"
    (instance (type $host-interface)))
  (import "test:alias/second@1.0.0"
    (instance (type $host-interface))))
