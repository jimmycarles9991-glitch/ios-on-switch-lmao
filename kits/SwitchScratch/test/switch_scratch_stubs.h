// What kits/SwitchScratch takes from the kernel, on the host: tasks (the current one settable, each with or without
// com.apple.private.memory.ownership_transfer), a pageable purgeable IOBufferMemoryDescriptor whose ownership follows
// XNU's mach_memory_entry_ownership, IOKit's locks and allocation macros, and a clock the test moves.
#ifndef SWITCH_TEST_SWITCH_SCRATCH_STUBS_H
#define SWITCH_TEST_SWITCH_SCRATCH_STUBS_H

#include <cstdint>
#include <mutex>

using IOReturn = int;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using vm_size_t = uint64_t;
using vm_offset_t = uint64_t;
using addr64_t = uint64_t;

constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotPermitted = static_cast<IOReturn>(0xe00002e2);
constexpr IOOptionBits kIODirectionInOut = 3, kIOMemoryPageable = 0x400, kIOMemoryPurgeable = 0x40,
                       kIOMemoryMapperNone = 0x800;
constexpr IOOptionBits kIOMemoryPurgeableNonVolatile = 2, kIOMemoryPurgeableVolatile = 3, kIOMemoryPurgeableEmpty = 4;
constexpr int kIOMemoryLedgerTagGraphics = 3;

#define NSEC_PER_USEC 1000ull
#define NSEC_PER_MSEC 1000000ull

struct switch_test_task {
    const char *name;
    bool transfers;  // holds com.apple.private.memory.ownership_transfer (the kernel always may)
};

using task_t = switch_test_task *;

inline switch_test_task gKernelTask{"kernel_task", true};
inline task_t kernel_task = &gKernelTask;
inline task_t gCurrentTask = &gKernelTask;

inline task_t current_task()
{
    return gCurrentTask;
}

inline uint64_t gSwitchTimeNow;

inline uint64_t mach_absolute_time()
{
    return gSwitchTimeNow;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns;
}

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = gSwitchTimeNow + uint64_t{interval} * scale;
}

#define IONew(type, count)         (new type[count])
#define IODelete(ptr, type, count) (delete[] (ptr))

struct IOLock {
    std::mutex m;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *l)
{
    delete l;
}

inline void IOLockLock(IOLock *l) __attribute__((no_thread_safety_analysis))
{
    l->m.lock();
}

inline void IOLockUnlock(IOLock *l) __attribute__((no_thread_safety_analysis))
{
    l->m.unlock();
}

inline int gScratchLive;

class IOBufferMemoryDescriptor {
public:
    task_t owner = nullptr;  // whose footprint its pages count in
    int tag = 0;
    IOOptionBits purgeable = 0;
    bool prepared = false;

    // A purgeable object is its making thread's task's from the start (vm_user.c mach_make_memory_entry_internal)
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t, IOOptionBits options, vm_size_t, vm_offset_t)
    {
        auto *md = new IOBufferMemoryDescriptor;
        if (options & kIOMemoryPurgeable) {
            md->owner = current_task();
            md->purgeable = kIOMemoryPurgeableNonVolatile;
        }
        ++gScratchLive;
        return md;
    }

    // mach_memory_entry_ownership: off the kernel's own threads, a task without the entitlement charges only itself
    IOReturn setOwnership(task_t newOwner, int newTag, IOOptionBits)
    {
        const task_t self = current_task();
        if (self != kernel_task && newOwner != self && newOwner != nullptr && !self->transfers)
            return kIOReturnNotPermitted;
        if (newOwner)
            owner = newOwner;
        tag = newTag;
        return kIOReturnSuccess;
    }

    IOReturn setPurgeable(IOOptionBits state, IOOptionBits *old)
    {
        *old = purgeable;
        purgeable = state;
        return kIOReturnSuccess;
    }

    IOReturn prepare()
    {
        prepared = true;
        return kIOReturnSuccess;
    }

    IOReturn complete()
    {
        prepared = false;
        return kIOReturnSuccess;
    }

    addr64_t getPhysicalSegment(uint64_t offset, IOByteCount *, IOOptionBits)
    {
        return 0x100000000ULL + offset;
    }

    void release()
    {
        --gScratchLive;
        delete this;
    }
};

#endif
