// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
using IOOptionBits = uint32_t;

class IOMemoryDescriptor {
public:
    virtual ~IOMemoryDescriptor() = default;
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};

struct IOLock {
    std::mutex mutex;
    std::condition_variable changed;
};

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
    lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.unlock();
}

inline void IOLockSleep(IOLock *lock, void *, int)
{
    std::unique_lock held(lock->mutex, std::adopt_lock);
    lock->changed.wait(held);
    held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
    lock->changed.notify_all();
}

constexpr int THREAD_UNINT = 0;
#define IONewZero(type, count) static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) \
    do {                               \
        (void)(count);                 \
        std::free(pointer);            \
    } while (0)
