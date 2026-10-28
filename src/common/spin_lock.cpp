// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include "common/spin_lock.h"
#include "common/types.h"

#if _MSC_VER
#include <intrin.h>
#if _M_AMD64
#define __x86_64__ 1
#endif
#if _M_ARM64
#define __aarch64__ 1
#endif
#else
#if __x86_64__
#include <xmmintrin.h>
#endif
#endif

namespace {

void ThreadPause() {
#if __x86_64__
    _mm_pause();
#elif __aarch64__ && _MSC_VER
    __yield();
#elif __aarch64__
    asm("yield");
#endif
}

} // Anonymous namespace

namespace Common {

void SpinLock::lock() {
    // Spin on a plain load and yield after a short spin: with more runnable threads than cores the
    // holder may be descheduled, and unbounded spinning then burns the time it needs to release.
    u32 spins = 0;
    while (lck.test_and_set(std::memory_order_acquire)) {
        while (lck.test(std::memory_order_relaxed)) {
            if (++spins < 64) {
                ThreadPause();
            } else {
                std::this_thread::yield();
            }
        }
    }
}

void SpinLock::unlock() {
    lck.clear(std::memory_order_release);
}

bool SpinLock::try_lock() {
    if (lck.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    return true;
}

} // namespace Common
