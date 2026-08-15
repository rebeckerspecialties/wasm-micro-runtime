/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "test_helper.h"
#include "gtest/gtest.h"

#include "platform_api_extension.h"

#if defined(OS_ENABLE_HW_BOUND_CHECK) && defined(WAMR_POSIX_THREAD_START_TEST) \
    && WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
extern "C" void
os_posix_thread_test_arm_alt_stack_init_failure(void);

extern "C" void
os_posix_thread_test_get_wrapper_counts(uint32 *allocations, uint32 *frees);

static void *
unexpected_thread_start(void *arg)
{
    *(bool *)arg = true;
    return nullptr;
}

TEST(posix_thread_start_test, alt_stack_failure_completes_handshake_and_frees)
{
    WAMRRuntimeRAII<> runtime;
    korp_tid tid;
    bool start_called = false;
    uint32 allocations = 0;
    uint32 frees = 0;

    os_posix_thread_test_arm_alt_stack_init_failure();

    /* A regression in the parent/child handshake must fail the test process
       promptly instead of leaving the runtime or CI waiting indefinitely. */
    alarm(5);
    EXPECT_EQ(BHT_ERROR,
              os_thread_create(&tid, unexpected_thread_start, &start_called,
                               APP_THREAD_STACK_SIZE_DEFAULT));
    alarm(0);

    os_posix_thread_test_get_wrapper_counts(&allocations, &frees);
    EXPECT_FALSE(start_called);
    EXPECT_EQ(1U, allocations);
    EXPECT_EQ(1U, frees);
}
#endif
