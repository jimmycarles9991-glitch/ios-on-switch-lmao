// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <atomic>
#include <functional>
#include <switch/memory_attachment.h>

class OSObject {
public:
    std::atomic<unsigned> refs{1};
    std::function<void()> onFree;

    explicit OSObject(std::function<void()> fn = {}) : onFree(fn)
    {}

    virtual ~OSObject()
    {
        if (onFree)
            onFree();
    }

    void retain()
    {
        ++refs;
    }

    void release()
    {
        if (--refs == 0)
            delete this;
    }
};

struct IOMemoryDescriptorReserved {
    OSObject *switchAttachment = nullptr;
    OSObject *switchGpuRecord = nullptr;
    uint32_t switchCpuUsers = 0;
    bool switchExclusive = false;
    uint64_t switchGeneration = 0;
    std::atomic<unsigned> unrelatedState{0};
};

class IOMemoryDescriptor : public OSObject {
    friend class SwitchMemoryAttachment;
    IOMemoryDescriptorReserved *reserved = nullptr;

    IOMemoryDescriptorReserved *getKernelReserved()
    {
        return SwitchMemoryAttachment::prepare(this);
    }

public:
    ~IOMemoryDescriptor()
    {
        SwitchMemoryAttachment::clear(reserved);
        delete reserved;
    }
};

#include <atomic>
#include <new>

inline std::atomic<unsigned> switchAllocations{0};
inline std::atomic<unsigned> switchAllocationFailures{0};

template <class T> T *switchAllocate()
{
    if (switchAllocationFailures.load() && switchAllocationFailures.fetch_sub(1))
        return nullptr;
    ++switchAllocations;
    return new (std::nothrow) T;
}

#define IOMallocType(T)      switchAllocate<T>()
#define IOFreeType(value, T) delete (value)

#include <condition_variable>
#include <mutex>
#include <cstdlib>

inline void panic(const char *)
{
    std::abort();
}

constexpr int THREAD_UNINT = 0;

struct IOLock {
    std::mutex mutex;
    std::condition_variable changed;
};

inline std::mutex switchSleepMutex;
inline std::condition_variable switchSleepChanged;
inline unsigned switchSleeps = 0;

inline IOLock *IOLockAlloc()
{
    return new IOLock;
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
    {
        std::lock_guard guard(switchSleepMutex);
        ++switchSleeps;
        switchSleepChanged.notify_all();
    }
    std::unique_lock held(lock->mutex, std::adopt_lock);
    lock->changed.wait(held);
    held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
    lock->changed.notify_all();
}
