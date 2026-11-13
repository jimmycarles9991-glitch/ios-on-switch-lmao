// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>

using IOReturn = int32_t;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0;
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

#define IONew(type, count)             new (std::nothrow) type[count]
#define IONewZero(type, count)         new (std::nothrow) type[count]()
#define IODelete(pointer, type, count) delete[] (pointer)

class OSObject {
public:
    std::atomic<uint32_t> refs{1};

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

class IOMemoryDescriptor : public OSObject {
public:
    uint64_t address = 0x100000;
};

struct switch_gpu_backing {
    std::atomic<uint32_t> refs{1};
    void *owner = nullptr;
    void (*release)(void *) = nullptr;
};
