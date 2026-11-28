// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <Kernel/IOKitKernelInternal.h>
#include <switch/memory_attachment.h>
#include <kern/thread.h>

extern "C" void switch_gpu_admission_changed(void) __attribute__((weak_import));

namespace {
constexpr size_t kShardCount = 64;
constexpr uint32_t kExclusiveBit = 1u << 31;
constexpr uint32_t kWaitersBit = 1u << 30;
constexpr uint32_t kCpuMask = kWaitersBit - 1u;

IOLock *shardLocks[kShardCount];

size_t shardIndex(const void *key)
{
    const uintptr_t bits = reinterpret_cast<uintptr_t>(key);
    return ((bits >> 6) ^ (bits >> 13)) & (kShardCount - 1);
}

IOLock *shardLockFor(const void *key)
{
    IOLock *&slot = shardLocks[shardIndex(key)];
    IOLock *lock = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    auto *made = IOLockAlloc();
    if (!made)
        return nullptr;
    IOLock *expected = nullptr;
    if (!__atomic_compare_exchange_n(&slot, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        IOLockFree(made);
        return expected;
    }
    return made;
}
}

IOMemoryDescriptorReserved *SwitchMemoryAttachment::prepare(IOMemoryDescriptor *memory)
{
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    if (reserved)
        return reserved;
    auto *made = IOMallocType(IOMemoryDescriptorReserved);
    if (!made)
        return nullptr;
    if (__atomic_compare_exchange_n(&memory->reserved, &reserved, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return made;
    IOFreeType(made, IOMemoryDescriptorReserved);
    return reserved;
}

volatile uint64_t *SwitchMemoryAttachment::generation(IOMemoryDescriptor *memory)
{
    if (!memory)
        return nullptr;
    auto *reserved = prepare(memory);
    if (!reserved)
        return nullptr;
    if (__atomic_load_n(&reserved->switchGeneration, __ATOMIC_RELAXED) == 0) {
        uint64_t expected = 0;
        __atomic_compare_exchange_n(&reserved->switchGeneration, &expected, uint64_t(1), false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
    }
    return &reserved->switchGeneration;
}

namespace {
void clearSlot(IOMemoryDescriptorReserved *reserved, bool gpu)
{
    if (gpu) {
        auto *old = reserved->switchGpuRecord;
        if (!old)
            return;
        reserved->switchGpuRecord = nullptr;
        old->release();
        return;
    }
    OSObject **slot = &reserved->switchAttachment;
    if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE))
        return;
    IOLock *lock = shardLockFor(reserved);
    IOLockLock(lock);
    auto *old = *slot;
    __atomic_store_n(slot, static_cast<OSObject *>(nullptr), __ATOMIC_RELEASE);
    IOLockUnlock(lock);
    if (old)
        old->release();
}
}

OSObject *SwitchMemoryAttachment::copy(IOMemoryDescriptor *memory)
{
    if (!memory)
        return nullptr;
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    if (!reserved || !__atomic_load_n(&reserved->switchAttachment, __ATOMIC_ACQUIRE))
        return nullptr;
    IOLock *lock = shardLockFor(reserved);
    IOLockLock(lock);
    auto *object = reserved->switchAttachment;
    if (object)
        object->retain();
    IOLockUnlock(lock);
    return object;
}

bool SwitchMemoryAttachment::replace(IOMemoryDescriptor *memory, OSObject *expected, OSObject *replacement)
{
    if (!memory)
        return false;
    auto *reserved = memory->getKernelReserved();
    if (!reserved)
        return false;
    IOLock *lock = shardLockFor(reserved);
    if (!lock)
        return false;
    if (replacement)
        replacement->retain();
    IOLockLock(lock);
    auto *old = reserved->switchAttachment;
    const bool changed = old == expected;
    if (changed)
        __atomic_store_n(&reserved->switchAttachment, replacement, __ATOMIC_RELEASE);
    IOLockUnlock(lock);
    if (changed ? old != nullptr : replacement != nullptr)
        (changed ? old : replacement)->release();
    return changed;
}

OSObject *SwitchMemoryAttachment::borrowGpu(IOMemoryDescriptor *memory)
{
    if (!memory)
        return nullptr;
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    return reserved ? __atomic_load_n(&reserved->switchGpuRecord, __ATOMIC_ACQUIRE) : nullptr;
}

bool SwitchMemoryAttachment::publishGpu(IOMemoryDescriptor *memory, OSObject *made)
{
    if (!memory || !made)
        return false;
    auto *reserved = memory->getKernelReserved();
    if (!reserved)
        return false;
    made->retain();
    OSObject *expected = nullptr;
    if (__atomic_compare_exchange_n(&reserved->switchGpuRecord, &expected, made, false, __ATOMIC_RELEASE,
                                    __ATOMIC_ACQUIRE))
        return true;
    made->release();
    return false;
}

void SwitchMemoryAttachment::clear(IOMemoryDescriptorReserved *reserved)
{
    if (!reserved)
        return;
    clearSlot(reserved, false);
    clearSlot(reserved, true);
}

int SwitchMemoryAttachment::enterCpu(IOMemoryDescriptor *memory, bool avoid)
{
    if (!memory)
        return -12;
    auto *reserved = prepare(memory);
    if (!reserved)
        return -12;
    uint32_t state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_RELAXED);
    for (;;) {
        if (state & kExclusiveBit) {
            if (avoid)
                return -16;
            IOLock *lock = shardLockFor(reserved);
            if (!lock)
                return -12;
            IOLockLock(lock);
            state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_ACQUIRE);
            while (state & kExclusiveBit) {
                if (!(state & kWaitersBit)) {
                    if (!__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, state | kWaitersBit, true,
                                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED))
                        continue;
                }
                IOLockSleep(lock, reserved, THREAD_UNINT);
                state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_ACQUIRE);
            }
            IOLockUnlock(lock);
            continue;
        }
        if ((state & kCpuMask) == kCpuMask)
            return -16;
        if (__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, state + 1u, true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            memory->retain();
            return 0;
        }
    }
}

void SwitchMemoryAttachment::leaveCpu(IOMemoryDescriptor *memory)
{
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    if (!reserved)
        panic("SwitchSurface: unbalanced descriptor CPU reservation");
    uint32_t state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_RELAXED);
    bool wake = false;
    for (;;) {
        const uint32_t users = state & kCpuMask;
        if (!users)
            panic("SwitchSurface: unbalanced descriptor CPU reservation");
        uint32_t next = state - 1u;
        wake = (users == 1u) && (state & kWaitersBit) != 0u;
        if (wake)
            next &= ~kWaitersBit;
        if (__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, next, true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            break;
    }
    if (wake) {
        IOLock *lock = shardLockFor(reserved);
        IOLockLock(lock);
        IOLockWakeup(lock, reserved, false);
        IOLockUnlock(lock);
    }
    memory->release();
}

uint32_t SwitchMemoryAttachment::cpuUsers(IOMemoryDescriptor *memory)
{
    auto *reserved = memory ? __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE) : nullptr;
    if (!reserved)
        return 0;
    return __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_RELAXED) & kCpuMask;
}

int SwitchMemoryAttachment::enterExclusive(IOMemoryDescriptor *memory, bool avoid)
{
    if (!memory)
        return -12;
    auto *reserved = prepare(memory);
    if (!reserved)
        return -12;
    uint32_t state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_RELAXED);
    for (;;) {
        if (!(state & (kExclusiveBit | kCpuMask))) {
            if (__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, state | kExclusiveBit, true,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                memory->retain();
                return 0;
            }
            continue;
        }
        if (avoid)
            return -16;
        IOLock *lock = shardLockFor(reserved);
        if (!lock)
            return -12;
        IOLockLock(lock);
        state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_ACQUIRE);
        while (state & (kExclusiveBit | kCpuMask)) {
            if (!(state & kWaitersBit)) {
                if (!__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, state | kWaitersBit, true,
                                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED))
                    continue;
            }
            IOLockSleep(lock, reserved, THREAD_UNINT);
            state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_ACQUIRE);
        }
        IOLockUnlock(lock);
    }
}

void SwitchMemoryAttachment::leaveExclusive(IOMemoryDescriptor *memory)
{
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    if (!reserved)
        panic("SwitchSurface: unbalanced descriptor publication reservation");
    uint32_t state = __atomic_load_n(&reserved->switchCpuUsers, __ATOMIC_RELAXED);
    for (;;) {
        if (!(state & kExclusiveBit))
            panic("SwitchSurface: unbalanced descriptor publication reservation");
        const uint32_t next = state & ~(kExclusiveBit | kWaitersBit);
        if (__atomic_compare_exchange_n(&reserved->switchCpuUsers, &state, next, true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            break;
    }
    if (state & kWaitersBit) {
        IOLock *lock = shardLockFor(reserved);
        IOLockLock(lock);
        IOLockWakeup(lock, reserved, false);
        IOLockUnlock(lock);
    }
    if (switch_gpu_admission_changed)
        switch_gpu_admission_changed();
    memory->release();
}
