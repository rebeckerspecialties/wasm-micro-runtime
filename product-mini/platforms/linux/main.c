/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "../posix/main.c"

/* clang-tidy self-test (do not merge): narrowing long->int must be flagged
 * on the line below by bugprone-narrowing-conversions. */
int ct_selftest_narrowing(long v);
int
ct_selftest_narrowing(long v)
{
    int x = v;
    return x;
}
