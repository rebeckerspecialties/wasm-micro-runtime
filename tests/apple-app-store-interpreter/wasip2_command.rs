// Copyright (C) 2026 Matt Hargett. All rights reserved.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Regenerate the checked fixture with the product's pinned guest toolchain:
// rustc +1.93.1 --crate-name wamr_wasip2_command \
//   --target wasm32-wasip2 -O wasip2_command.rs \
//   -o /tmp/wasip2_command.wasm
// wasm-tools strip /tmp/wasip2_command.wasm -o wasip2_command.wasm

fn main() {
    println!("wamr wasm32-wasip2 smoke");
}
