// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include "SwitchVICBouncePool.h"
#include "TegraVIC.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

namespace {
constexpr uint64_t kBudget = 256ull << 20;
SwitchVICBounceSlot gSlots[4];
IOLock *gLock;
uint64_t gBytes;

struct Storage {
    switch_dma_pages pages;
};

void freeStorage(void *context)
{
    auto *storage = static_cast<Storage *>(context);
    switch_dma_pages_free(&storage->pages);
    IODelete(storage, Storage, 1);
}

void discard(SwitchVICBounceBuffer &buffer)
{
    switch_vic_mapping_release(buffer.mapping);
    if (buffer.backing)
        switch_gpu_backing_release(buffer.backing);
    gBytes -= buffer.capacity;
    buffer = {};
}

bool initialize()
{
    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE))
        return true;
    auto *made = IOLockAlloc();
    if (!made)
        return false;
    IOLock *expected = nullptr;
    if (!__atomic_compare_exchange_n(&gLock, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        IOLockFree(made);
    return true;
}

int allocate(SwitchVICBounceBuffer &buffer, const TegraVICSurfaceDesc &desc)
{
    if (!desc.length)
        return 0;
    if (buffer.capacity >= desc.length) {
        buffer.desc = desc;
        return 0;
    }
    discard(buffer);
    auto *owner = IONewZero(Storage, 1);
    if (!owner)
        return -12;
    if (!switch_dma_pages_alloc(&owner->pages, static_cast<size_t>(desc.length))) {
        IODelete(owner, Storage, 1);
        return -12;
    }
    auto *backing = switch_gpu_backing_register(owner->pages.pages, owner->pages.count, 0, owner, freeStorage);
    if (!backing) {
        freeStorage(owner);
        return -12;
    }
    auto *mapping = switch_vic_mapping_acquire(backing, true);
    if (!mapping) {
        switch_gpu_backing_release(backing);
        return -12;
    }
    buffer = {owner->pages.memory, backing, mapping, desc, owner->pages.size};
    gBytes += buffer.capacity;
    return 0;
}
}

int switch_vic_bounce_acquire(const TegraVICSurfaceDesc *source, const TegraVICSurfaceDesc *destination,
                              SwitchVICBounceSlot **out)
{
    *out = nullptr;
    TegraVICSurfaceDesc requested[2] = {};
    const TegraVICTraits *traits = tegra_vic_traits();
    if (source)
        tegra_vic_bounce_desc(source, tegra_vic_pitch_align(traits, true), &requested[0]);
    if (destination)
        tegra_vic_bounce_desc(destination, tegra_vic_pitch_align(traits, false), &requested[1]);
    if (requested[0].length > kBudget || requested[1].length > kBudget - requested[0].length)
        return -12;
    if (!initialize())
        return -12;
    IOLockLock(gLock);
    SwitchVICBounceSlot *slot = nullptr;
    for (;;) {
        uint64_t bestGrowth = UINT64_MAX;
        for (auto &candidate : gSlots) {
            if (candidate.busy)
                continue;
            uint64_t growth = 0;
            for (unsigned i = 0; i < 2; ++i)
                if (requested[i].length > candidate.buffers[i].capacity)
                    growth += requested[i].length - candidate.buffers[i].capacity;
            if (growth < bestGrowth) {
                slot = &candidate;
                bestGrowth = growth;
            }
        }
        if (slot) {
            for (unsigned i = 0; i < 2; ++i)
                if (!requested[i].length)
                    discard(slot->buffers[i]);
            for (auto &candidate : gSlots) {
                if (&candidate == slot || candidate.busy)
                    continue;
                for (auto &buffer : candidate.buffers)
                    if (gBytes + bestGrowth > kBudget)
                        discard(buffer);
            }
            if (gBytes + bestGrowth <= kBudget)
                break;
            slot = nullptr;
        }
        IOLockSleep(gLock, gSlots, THREAD_UNINT);
    }
    slot->busy = true;
    int error = allocate(slot->buffers[0], requested[0]);
    if (!error)
        error = allocate(slot->buffers[1], requested[1]);
    if (error) {
        slot->busy = false;
        IOLockWakeup(gLock, gSlots, false);
    } else
        *out = slot;
    IOLockUnlock(gLock);
    return error;
}

void switch_vic_bounce_release(SwitchVICBounceSlot *slot)
{
    if (!slot)
        return;
    IOLockLock(gLock);
    slot->busy = false;
    IOLockWakeup(gLock, gSlots, false);
    IOLockUnlock(gLock);
}
