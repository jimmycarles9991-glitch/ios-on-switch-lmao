// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "TegraVICSurface.h"
#include "TegraVIC.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraGM20B/TegraGM20BViews.h"

namespace {
constexpr uint32_t kSurfaceCacheCount = 32;
constexpr uint64_t kSurfaceCacheBytes = 128ULL << 20;

struct CachedSurface {
    OSObject *surface = nullptr;
    IOMemoryDescriptor *memory = nullptr;
    switch_gpu_backing *backing = nullptr;
    uint64_t base = 0, length = 0, used = 0;
};

CachedSurface gSurfaces[kSurfaceCacheCount];
IOLock *gLock;
uint64_t gClock, gBytes;

bool initialize()
{
    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE))
        return true;
    auto *made = IOLockAlloc();
    IOLock *expected = nullptr;
    if (!made)
        return false;
    if (!__atomic_compare_exchange_n(&gLock, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        IOLockFree(made);
    return true;
}

void evict(CachedSurface &item)
{
    if (!item.surface)
        return;
    gBytes -= (item.length + 4095) & ~4095ULL;
    switch_gpu_backing_release(item.backing);
    item.surface->release();
    item = {};
}

CachedSurface *oldest()
{
    CachedSurface *candidate = &gSurfaces[0];
    for (auto &item : gSurfaces) {
        if (!item.surface)
            return &item;
        if (item.used < candidate->used)
            candidate = &item;
    }
    return candidate;
}

void releaseSurface(void *context)
{
    auto *surface = static_cast<TegraVICSurface *>(context);
    tegra_vic_surface_release(surface);
    IODelete(surface, TegraVICSurface, 1);
}

switch_gpu_backing *adopt(const TegraVICSurface *surface, switch_gpu_backing *backing)
{
    if (backing && switch_gpu_surface_adopt(surface->memory, backing)) {
        switch_gpu_backing_release(backing);
        return nullptr;
    }
    return backing;
}

switch_gpu_backing *cached(const TegraVICSurface *surface)
{
    for (auto &item : gSurfaces) {
        if (item.surface == surface->surface && item.memory == surface->memory && item.base == surface->baseOffset &&
            item.length == surface->desc.length) {
            item.used = ++gClock;
            switch_gpu_backing_retain(item.backing);
            return item.backing;
        }
    }
    return nullptr;
}
}  // namespace

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *surface)
{
    const uint64_t length = surface->desc.length;
    if (!length || length > kSurfaceCacheBytes || !initialize())
        return nullptr;
    IOLockLock(gLock);
    auto *backing = cached(surface);
    IOLockUnlock(gLock);
    if (backing)
        return adopt(surface, backing);
    const uint32_t count = static_cast<uint32_t>((length + 4095) >> 12);
    auto *owner = IONew(TegraVICSurface, 1);
    uint64_t *pages = IONew(uint64_t, count);
    if (!owner || !pages) {
        if (owner)
            IODelete(owner, TegraVICSurface, 1);
        if (pages)
            IODelete(pages, uint64_t, count);
        return nullptr;
    }
    *owner = *surface;
    owner->prepared = false;
    owner->surface->retain();
    uint32_t actual = 0;
    if (tegra_vic_surface_prepare(owner, length, pages, count, &actual) == kIOReturnSuccess && actual == count)
        backing = switch_gpu_backing_register(pages, count, owner->id, owner, releaseSurface);
    IODelete(pages, uint64_t, count);
    if (!backing) {
        releaseSurface(owner);
        return nullptr;
    }
    IOLockLock(gLock);
    auto *existing = cached(surface);
    if (!existing) {
        while (gBytes + ((length + 4095) & ~4095ULL) > kSurfaceCacheBytes) {
            CachedSurface *victim = nullptr;
            for (auto &item : gSurfaces) {
                if (item.surface && (!victim || item.used < victim->used))
                    victim = &item;
            }
            if (!victim)
                break;
            evict(*victim);
        }
        auto *slot = oldest();
        evict(*slot);
        switch_gpu_backing_retain(backing);
        surface->surface->retain();
        *slot = {surface->surface, surface->memory, backing, surface->baseOffset, length, ++gClock};
        gBytes += (length + 4095) & ~4095ULL;
    }
    IOLockUnlock(gLock);
    if (existing) {
        switch_gpu_backing_release(backing);
        backing = existing;
    }
    return adopt(surface, backing);
}
