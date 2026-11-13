// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as SwitchHost1xTokenFence.cpp and SwitchHost1xResults.h see it: locks that check they are not taken
// twice, and the allocations and sleeps the results' records use (nothing here sleeps).
#pragma once
#include <new>
#include <stdexcept>

struct IOLock {
    bool held;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    if (lock->held)
        throw std::runtime_error("lock taken twice");
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

inline int IOLockSleep(IOLock *, void *, int)
{
    throw std::runtime_error("unexpected sleep");
}

#define THREAD_UNINT                   0
#define IONewZero(type, count)         (new type[count]())
#define IODelete(pointer, type, count) (delete[] (pointer))
