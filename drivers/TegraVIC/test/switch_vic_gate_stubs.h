// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>

using IOReturn = int32_t;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnBadArgument = static_cast<IOReturn>(0xe00002c2);
constexpr IOReturn kIOReturnNoMemory = static_cast<IOReturn>(0xe00002bd);
using IOLock = std::mutex;

inline IOLock *IOLockAlloc()
{
    return new (std::nothrow) IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->unlock();
}

inline int gateLogLines = 0;
#define IOLog(...) (++gateLogLines, std::fprintf(stderr, __VA_ARGS__))

inline task_t gateCallerTask = reinterpret_cast<task_t>(0x7a5c);

inline task_t current_task()
{
    return gateCallerTask;
}

class OSObject {};

class IOMemoryDescriptor {};

struct IOExternalMethodArguments {
    uint32_t scalarInputCount = 0, scalarOutputCount = 0;
    const void *structureInput = nullptr;
    uint32_t structureInputSize = 0;
    void *structureOutput = nullptr;
    uint32_t structureOutputSize = 0;
};
