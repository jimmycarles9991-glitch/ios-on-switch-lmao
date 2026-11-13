// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <switch/memory_attachment.h>

using IOReturn = int32_t;
#ifndef PAGE_SIZE
#define PAGE_SIZE 16384
#endif
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr int THREAD_UNINT = 0;

inline void panic(const char *)
{
    std::abort();
}

#define OSDeclareDefaultStructors(type) \
public:                                 \
    type() = default;                   \
    ~type() override = default;
#define OSDefineMetaClassAndStructors(type, base)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define IOMallocType(type)          new (std::nothrow) type()
#define IOFreeType(object, type)    delete object

inline void IOLog(const char *, ...)
{}

class OSObject {
public:
    std::atomic<unsigned> refs{1};
    virtual ~OSObject() = default;

    bool init()
    {
        return true;
    }

    virtual void free()
    {
        delete this;
    }

    void retain()
    {
        ++refs;
    }

    void release()
    {
        if (--refs == 0)
            free();
    }

    int getRetainCount() const
    {
        return static_cast<int>(refs.load());
    }
};

struct IOMemoryDescriptorReserved {
    OSObject *switchAttachment = nullptr;
    OSObject *switchGpuRecord = nullptr;
    uint32_t switchCpuUsers = 0;
    bool switchExclusive = false;
    uint64_t switchGeneration = 0;
};
struct switch_gpu_backing;

class IOMemoryDescriptor : public OSObject {
    friend class SwitchMemoryAttachment;
    IOMemoryDescriptorReserved *reserved = nullptr;

    IOMemoryDescriptorReserved *getKernelReserved()
    {
        return SwitchMemoryAttachment::prepare(this);
    }

public:
    uint64_t length = 0;
    switch_gpu_backing *backing = nullptr;

    uint64_t getLength() const
    {
        return length;
    }

    ~IOMemoryDescriptor() override
    {
        SwitchMemoryAttachment::clear(reserved);
        delete reserved;
    }
};

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
