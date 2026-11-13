// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <Kernel/IOKitKernelInternal.h>
#include <switch/memory_attachment.h>
#include <kern/thread.h>

extern "C" void switch_gpu_admission_changed(void) __attribute__((weak_import));

namespace {
IOLock *attachmentLock;

bool initialize()
{
    if (__atomic_load_n(&attachmentLock, __ATOMIC_ACQUIRE))
        return true;
    auto *made = IOLockAlloc();
    if (!made)
        return false;
    IOLock *expected = nullptr;
    if (!__atomic_compare_exchange_n(&attachmentLock, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        IOLockFree(made);
    return true;
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
    uint64_t expected = 0;
    __atomic_compare_exchange_n(&reserved->switchGeneration, &expected, uint64_t(1), false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
    return &reserved->switchGeneration;
}

namespace {
OSObject **slotOf(IOMemoryDescriptorReserved *reserved, bool gpu)
{
    return gpu ? &reserved->switchGpuRecord : &reserved->switchAttachment;
}

void clearSlot(IOMemoryDescriptorReserved *reserved, bool gpu)
{
    OSObject **slot = slotOf(reserved, gpu);
    if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE))
        return;
    IOLockLock(attachmentLock);
    auto *old = *slot;
    __atomic_store_n(slot, static_cast<OSObject *>(nullptr), __ATOMIC_RELEASE);
    IOLockUnlock(attachmentLock);
    if (old)
        old->release();
}
}

OSObject *SwitchMemoryAttachment::copySlot(IOMemoryDescriptor *memory, bool gpu)
{
    if (!memory)
        return nullptr;
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    if (!reserved || !__atomic_load_n(slotOf(reserved, gpu), __ATOMIC_ACQUIRE))
        return nullptr;
    IOLockLock(attachmentLock);
    auto *object = *slotOf(reserved, gpu);
    if (object)
        object->retain();
    IOLockUnlock(attachmentLock);
    return object;
}

bool SwitchMemoryAttachment::replaceSlot(IOMemoryDescriptor *memory, bool gpu, OSObject *expected,
                                         OSObject *replacement)
{
    if (!memory || !initialize())
        return false;
    auto *reserved = memory->getKernelReserved();
    if (!reserved)
        return false;
    if (replacement)
        replacement->retain();
    OSObject **slot = slotOf(reserved, gpu);
    IOLockLock(attachmentLock);
    auto *old = *slot;
    const bool changed = old == expected;
    if (changed)
        __atomic_store_n(slot, replacement, __ATOMIC_RELEASE);
    IOLockUnlock(attachmentLock);
    if (changed ? old != nullptr : replacement != nullptr)
        (changed ? old : replacement)->release();
    return changed;
}

OSObject *SwitchMemoryAttachment::copy(IOMemoryDescriptor *memory)
{
    return copySlot(memory, false);
}

bool SwitchMemoryAttachment::replace(IOMemoryDescriptor *memory, OSObject *expected, OSObject *replacement)
{
    return replaceSlot(memory, false, expected, replacement);
}

OSObject *SwitchMemoryAttachment::copyGpu(IOMemoryDescriptor *memory)
{
    return copySlot(memory, true);
}

bool SwitchMemoryAttachment::replaceGpu(IOMemoryDescriptor *memory, OSObject *expected, OSObject *replacement)
{
    return replaceSlot(memory, true, expected, replacement);
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
    if (!memory || !initialize())
        return -12;
    auto *reserved = prepare(memory);
    if (!reserved)
        return -12;
    memory->retain();
    IOLockLock(attachmentLock);
    while (reserved->switchExclusive && !avoid)
        IOLockSleep(attachmentLock, reserved, THREAD_UNINT);
    const bool admitted = !reserved->switchExclusive && reserved->switchCpuUsers != UINT32_MAX;
    if (admitted)
        ++reserved->switchCpuUsers;
    IOLockUnlock(attachmentLock);
    if (!admitted)
        memory->release();
    return admitted ? 0 : -16;
}

void SwitchMemoryAttachment::leaveCpu(IOMemoryDescriptor *memory)
{
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    IOLockLock(attachmentLock);
    if (!reserved || !reserved->switchCpuUsers)
        panic("SwitchSurface: unbalanced descriptor CPU reservation");
    --reserved->switchCpuUsers;
    if (!reserved->switchCpuUsers)
        IOLockWakeup(attachmentLock, reserved, false);
    IOLockUnlock(attachmentLock);
    memory->release();
}

uint32_t SwitchMemoryAttachment::cpuUsers(IOMemoryDescriptor *memory)
{
    auto *reserved = memory ? __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE) : nullptr;
    if (!reserved || !__atomic_load_n(&attachmentLock, __ATOMIC_ACQUIRE))
        return 0;
    IOLockLock(attachmentLock);
    const uint32_t users = reserved->switchCpuUsers;
    IOLockUnlock(attachmentLock);
    return users;
}

int SwitchMemoryAttachment::enterExclusive(IOMemoryDescriptor *memory, bool avoid)
{
    if (!memory || !initialize())
        return -12;
    auto *reserved = prepare(memory);
    if (!reserved)
        return -12;
    memory->retain();
    IOLockLock(attachmentLock);
    while ((reserved->switchExclusive || reserved->switchCpuUsers) && !avoid)
        IOLockSleep(attachmentLock, reserved, THREAD_UNINT);
    const bool admitted = !reserved->switchExclusive && !reserved->switchCpuUsers;
    if (admitted)
        reserved->switchExclusive = true;
    IOLockUnlock(attachmentLock);
    if (!admitted)
        memory->release();
    return admitted ? 0 : -16;
}

void SwitchMemoryAttachment::leaveExclusive(IOMemoryDescriptor *memory)
{
    auto *reserved = __atomic_load_n(&memory->reserved, __ATOMIC_ACQUIRE);
    IOLockLock(attachmentLock);
    if (!reserved || !reserved->switchExclusive)
        panic("SwitchSurface: unbalanced descriptor publication reservation");
    reserved->switchExclusive = false;
    IOLockWakeup(attachmentLock, reserved, false);
    IOLockUnlock(attachmentLock);
    if (switch_gpu_admission_changed)
        switch_gpu_admission_changed();
    memory->release();
}
