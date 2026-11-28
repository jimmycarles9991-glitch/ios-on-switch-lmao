// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNativeSurfaceObject.h"

namespace {
IOLock *gPrepareLock;

// Publications their surface replaced while a view still held them. Each is held by this ring and by the views still
// reading it; once only the ring holds it, the next publication that cannot reuse its surface's own object takes it
// instead of allocating one. Under gPrepareLock.
constexpr unsigned kRetired = 16;
SwitchNativeSurface *gRetired[kRetired];
unsigned gRetiredCount;
#if DEVELOPMENT || DEBUG
uint64_t gPublished, gAllocated, gSurfaces;
#endif
}  // namespace

IOLock *switch_native_surface_prepare_lock()
{
    IOLock *lock = __atomic_load_n(&gPrepareLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *made = IOLockAlloc();
    if (made && !__atomic_compare_exchange_n(&gPrepareLock, &lock, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        IOLockFree(made);
        return lock;
    }
    return made;
}

void switch_native_surface_empty(SwitchNativeSurface *object)
{
    object->delivered = 0;
    if (object->readLease)
        switch_gpu_backing_cpu_release_owned(&object->readLease, 0);
    if (object->native)
        switch_gpu_backing_release(object->native);
    object->native = nullptr;
    const uint64_t live = object->generation + 1;
    object->generation = live;
    object->live = live;
    object->producer = {};
    object->picture = {};
    object->pitch = {};
    object->layout = {};
    object->crop = {};
    object->turn = {};
    object->turnOp = {};
    object->chromaSite = 0;
    object->writeEpoch = 0;
    object->pitchValid = false;
    object->gpuOwned = false;
}

SwitchNativeSurface *switch_native_surface_take_retired()
{
    SwitchNativeSurface *reused = nullptr;
    IOLock *lock = switch_native_surface_prepare_lock();
    if (!lock)
        return nullptr;
    IOLockLock(lock);
    for (unsigned i = 0; i < gRetiredCount && !reused; ++i) {
        if (gRetired[i]->getRetainCount() != 1)
            continue;
        reused = gRetired[i];
        gRetired[i] = gRetired[--gRetiredCount];
        gRetired[gRetiredCount] = nullptr;
    }
    IOLockUnlock(lock);
    if (reused)
        switch_native_surface_empty(reused);
    return reused;
}

void switch_native_surface_retire(SwitchNativeSurface *old)
{
    if (old->native && !old->readLease) {
        switch_gpu_backing_release(old->native);
        old->native = nullptr;
    }
    IOLock *lock = switch_native_surface_prepare_lock();
    if (!lock) {
        old->release();
        return;
    }
    SwitchNativeSurface *evicted = nullptr;
    IOLockLock(lock);
    if (gRetiredCount == kRetired) {
        evicted = gRetired[0];
        gRetired[0] = gRetired[--gRetiredCount];
    }
    gRetired[gRetiredCount++] = old;
    IOLockUnlock(lock);
    if (evicted)
        evicted->release();
}

void switch_native_surface_count(bool allocated, bool firstIntoSurface)
{
#if DEVELOPMENT || DEBUG
    IOLock *lock = switch_native_surface_prepare_lock();
    if (!lock)
        return;
    IOLockLock(lock);
    ++gPublished;
    gAllocated += allocated;
    gSurfaces += firstIntoSurface;
    IOLockUnlock(lock);
#else
    (void)allocated;
    (void)firstIntoSurface;
#endif
}

#if DEVELOPMENT || DEBUG
extern "C" void switch_native_surface_publications(uint64_t *published, uint64_t *allocated, uint64_t *surfaces)
{
    IOLockLock(switch_native_surface_prepare_lock());
    *published = gPublished;
    *allocated = gAllocated;
    *surfaces = gSurfaces;
    IOLockUnlock(switch_native_surface_prepare_lock());
}
#endif
