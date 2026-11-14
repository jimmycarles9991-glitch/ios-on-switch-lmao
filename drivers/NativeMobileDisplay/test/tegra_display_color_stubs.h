// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraDisplayColor.cpp sees it on the host: IOKit's locks as mutexes that count their takes and throw in
// a filter (display_filter_context.h), IOLog into a list. DC A's registers are the test's (SWITCH_MMIO_HOST_MODEL).
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "display_filter_context.h"

using IOReturn = int32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = static_cast<IOReturn>(0xe00002c2),
                   kIOReturnUnsupported = static_cast<IOReturn>(0xe00002c7),
                   kIOReturnNotReady = static_cast<IOReturn>(0xe00002d8);
using IOInterruptState = int;

inline std::atomic<unsigned> gSimpleLockTakes{0}, gLockTakes{0};

struct IOSimpleLock {
    std::mutex mutex;
    std::atomic<unsigned> takes{0};
};

struct IOLock {
    std::mutex mutex;
    std::atomic<unsigned> takes{0};
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    return new IOSimpleLock;
}

inline void IOSimpleLockFree(IOSimpleLock *lock)
{
    delete lock;
}

__attribute__((no_thread_safety_analysis)) inline void IOSimpleLockLock(IOSimpleLock *lock)
{
    filterMayNot("IOSimpleLockLock");
    lock->mutex.lock();
    ++lock->takes;
    ++gSimpleLockTakes;
}

__attribute__((no_thread_safety_analysis)) inline void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->mutex.unlock();
}

__attribute__((no_thread_safety_analysis)) inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    IOSimpleLockLock(lock);
    return 0;
}

__attribute__((no_thread_safety_analysis)) inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock,
                                                                                         IOInterruptState)
{
    IOSimpleLockUnlock(lock);
}

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

__attribute__((no_thread_safety_analysis)) inline void IOLockLock(IOLock *lock)
{
    filterMayNot("IOLockLock");
    lock->mutex.lock();
    ++lock->takes;
    ++gLockTakes;
}

__attribute__((no_thread_safety_analysis)) inline void IOLockUnlock(IOLock *lock)
{
    lock->mutex.unlock();
}

inline std::mutex gColourLogLock;
inline std::vector<std::string> gColourLog;

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    filterMayNot("IOLog");
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    const std::lock_guard<std::mutex> hold(gColourLogLock);
    gColourLog.emplace_back(text);
}
