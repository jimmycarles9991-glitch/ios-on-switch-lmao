// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchScratch.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/task.h>
#include "../SwitchTime/SwitchTime.h"

namespace {
constexpr uint64_t kPage = 4096, kGrain = 64 << 10;

IOLock *gLock;
switch_scratch *gKept;
uint64_t gKeptBytes, gHeld, gPeak, gHandouts;

// The one place a scratch's memory is charged: to `owner`'s graphics footprint. On a thread of a task without
// com.apple.private.memory.ownership_transfer XNU lets the memory be charged to that task alone
// (mach_memory_entry_ownership), and a purgeable object made there is that task's from the start: the kernel's charge
// then falls to the task, whose surface the scratch is held for.
IOReturn charge(IOBufferMemoryDescriptor *memory, task_t owner)
{
    const IOReturn ret = memory->setOwnership(owner, kIOMemoryLedgerTagGraphics, 0);
    if (ret == kIOReturnSuccess || owner == current_task())
        return ret;
    return memory->setOwnership(current_task(), kIOMemoryLedgerTagGraphics, 0);
}

void destroy(switch_scratch *s)
{
    if (s->releaseDeviceBacking)
        s->releaseDeviceBacking(s->deviceBacking);
    s->memory->complete();
    s->memory->release();
    IODelete(s->pas, uint64_t, s->npages);
    IODelete(s, switch_scratch, 1);
}

switch_scratch *create(uint64_t bytes)
{
    switch_scratch *s = IONew(switch_scratch, 1);
    if (!s)
        return nullptr;
    *s = {};
    // Its own VM object (pageable), tagged before any page of it is touched: XNU tags an object's pages for the ledger
    // only while it has none resident
    s->memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                                                            static_cast<IOOptionBits>(kIODirectionInOut) |
                                                                static_cast<IOOptionBits>(kIOMemoryPageable) |
                                                                static_cast<IOOptionBits>(kIOMemoryPurgeable),
                                                            bytes, kPage);
    s->npages = static_cast<uint32_t>(bytes / kPage);
    s->pas = s->memory ? IONew(uint64_t, s->npages) : nullptr;
    if (!s->pas || charge(s->memory, kernel_task) != kIOReturnSuccess || s->memory->prepare() != kIOReturnSuccess) {
        if (s->pas)
            IODelete(s->pas, uint64_t, s->npages);
        if (s->memory)
            s->memory->release();
        IODelete(s, switch_scratch, 1);
        return nullptr;
    }
    s->bytes = bytes;
    for (uint32_t i = 0; i < s->npages; i++)
        s->pas[i] = s->memory->getPhysicalSegment(static_cast<uint64_t>(i) * kPage, nullptr, kIOMemoryMapperNone);
    return s;
}

// lock held: the kept ones idle too long, then the oldest while too many bytes are kept; returned to free outside it
switch_scratch *trim(uint64_t now)
{
    switch_scratch *drop = nullptr;
    for (switch_scratch **p = &gKept; *p;) {
        switch_scratch *s = *p;
        if (now - s->idleSince > kSwitchScratchIdleNs || gKeptBytes > kSwitchScratchKeepBytes) {
            *p = s->next;
            gKeptBytes -= s->bytes;
            s->next = drop;
            drop = s;
            continue;
        }
        p = &s->next;
    }
    return drop;
}
}  // namespace

switch_scratch *switch_scratch_get(uint64_t bytes, task_t owner)
{
    bytes = (bytes + kGrain - 1) & ~(kGrain - 1);
    if (!bytes)
        return nullptr;
    if (!gLock) {
        IOLock *fresh = IOLockAlloc();
        IOLock *expected = nullptr;  // NOLINT(misc-const-correctness): __atomic_compare_exchange_n writes it
        if (!__atomic_compare_exchange_n(&gLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
        if (!gLock)
            return nullptr;
    }
    IOLockLock(gLock);
    switch_scratch *found = nullptr;
    for (switch_scratch **p = &gKept; *p; p = &(*p)->next) {
        if ((*p)->bytes >= bytes && (*p)->bytes <= 2 * bytes) {
            found = *p;
            *p = found->next;
            gKeptBytes -= found->bytes;
            break;
        }
    }
    switch_scratch *drop = trim(switch_now_ns());
    IOLockUnlock(gLock);
    while (drop) {
        switch_scratch *next = drop->next;
        destroy(drop);
        drop = next;
    }
    if (found) {
        found->next = nullptr;
        IOOptionBits was = 0;
        found->memory->setPurgeable(kIOMemoryPurgeableNonVolatile, &was);
    } else {
        found = create(bytes);
    }
    if (!found)
        return nullptr;
    charge(found->memory, owner);
    IOLockLock(gLock);
    gHeld += found->bytes;
    gPeak = gHeld > gPeak ? gHeld : gPeak;
    found->handout = ++gHandouts;
    IOLockUnlock(gLock);
    return found;
}

switch_scratch *switch_scratch_reclaim(switch_scratch *s, uint64_t handout, task_t owner, bool *intact)
{
    *intact = false;
    if (!s || !gLock)
        return nullptr;
    bool kept = false;
    IOLockLock(gLock);
    for (switch_scratch **p = &gKept; *p; p = &(*p)->next) {
        if (*p == s && s->handout == handout) {
            *p = s->next;
            gKeptBytes -= s->bytes;
            kept = true;
            break;
        }
    }
    IOLockUnlock(gLock);
    if (!kept)
        return nullptr;
    s->next = nullptr;
    IOOptionBits was = 0;
    s->memory->setPurgeable(kIOMemoryPurgeableNonVolatile, &was);
    *intact = was != kIOMemoryPurgeableEmpty;
    charge(s->memory, owner);
    IOLockLock(gLock);
    gHeld += s->bytes;
    gPeak = gHeld > gPeak ? gHeld : gPeak;
    s->handout = ++gHandouts;
    IOLockUnlock(gLock);
    return s;
}

void switch_scratch_put(switch_scratch *s)
{
    if (!s)
        return;
    charge(s->memory, kernel_task);
    IOOptionBits was = 0;
    s->memory->setPurgeable(kIOMemoryPurgeableVolatile, &was);
    IOLockLock(gLock);
    gHeld -= s->bytes;
    s->idleSince = switch_now_ns();
    s->next = gKept;
    gKept = s;
    gKeptBytes += s->bytes;
    switch_scratch *drop = trim(s->idleSince);
    IOLockUnlock(gLock);
    while (drop) {
        switch_scratch *next = drop->next;
        destroy(drop);
        drop = next;
    }
}

void switch_scratch_discard(switch_scratch *s)
{
    if (!s)
        return;
    IOLockLock(gLock);
    gHeld -= s->bytes;
    IOLockUnlock(gLock);
    destroy(s);
}

void switch_scratch_stats(uint64_t *held, uint64_t *peak)
{
    *held = *peak = 0;
    if (!gLock)
        return;
    IOLockLock(gLock);
    *held = gHeld;
    *peak = gPeak;
    IOLockUnlock(gLock);
}
