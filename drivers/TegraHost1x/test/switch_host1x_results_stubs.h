// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>

struct IOLock {
    bool held;
};

IOLock *IOLockAlloc();
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
void IOLockWakeup(IOLock *lock, void *event, bool one);
void IOLockSleep(IOLock *lock, void *event, int interruptible);
void *switch_host1x_test_alloc(size_t count, size_t bytes);

constexpr int THREAD_UNINT = 0;
#define IONewZero(type, count) static_cast<type *>(switch_host1x_test_alloc(count, sizeof(type)))
