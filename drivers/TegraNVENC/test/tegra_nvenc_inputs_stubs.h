// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraNVENCInputs.h sees it: a lock that checks it is not taken twice.
#pragma once
#include <stdexcept>

struct IOLock {
    bool held;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock{};
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
