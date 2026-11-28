// SPDX-License-Identifier: MIT
#include "lock.h"
#include "gpu.h"

#include <mach/mach.h>

#include "gm20b_gpu_internal.h"

namespace gm20b {
namespace {
constexpr uint64_t kArenaGranule = 0x10000;

void addAlias(ResourceSet &set, Kernel &k, const Bo &bo, uint64_t va)
{
    Bo alias = bo;
    alias.va = alias.vaBase = va;
    set.add(k, alias, TegraGM20BSurfaceRead);
}
}  // namespace

bool Device::bindArenaOffLock(Arena &a)
{
    bool bound = false;
    if (a.kernelBo) {
        bound = k_.alloc(a.bytes, true, a.bo, a.kind, kArenaGranule, false, false, true, a.va, true);
        if (bound)
            a.cpu = a.bo.cpu;
    } else if (a.wc) {
        bound = k_.importPages(a.cpu, a.bytes, a.bo) && k_.bindImportAt(a.cpu, a.bo, a.va, a.kind);
    } else {
        bound = k_.alloc(a.bytes, false, a.bo, a.kind, kArenaGranule, false, false, false, a.va);
    }
    bound = bound && (a.bufferVa == a.va || k_.bindAlias(a.bo, a.bufferVa, kPteKindPitch));
    for (const Arena::View &v : a.views)
        bound = bound && k_.bindAlias(a.bo, v.va, v.kind);
    if (!bound) {
        if (a.bo.handle)
            k_.free(a.bo);
        return false;
    }
    if (!a.wc) {
        Bo alias = a.bo;
        alias.va = alias.vaBase = a.bufferVa;
        set_.add(k_, {{&a.bo, TegraGM20BSurfaceRead}, {&alias, TegraGM20BSurfaceRead}});
        for (const Arena::View &v : a.views)
            addAlias(set_, k_, a.bo, v.va);
    }
    return true;
}

bool Device::restoreKernelArenaOffLock(bool wasParked, Bo &oldBo, uint8_t *cpu, uint64_t va, uint64_t bufferVa,
                                       uint64_t bytes, uint32_t kind, const std::vector<Arena::View> &views, Bo &outBo,
                                       bool &unparkResident, bool &unparkReclaimed, bool &unparkMismatch)
{
    if (!wasParked || !oldBo.handle)
        return false;
    bool needFresh = false;
    uint64_t addr = 0, mappedBytes = 0;
    uint32_t status = 0;
    const int kr = k_.unpark(oldBo.handle, addr, mappedBytes, status);
    if (kr == KERN_SUCCESS && status == TegraGM20BUnparkResident && addr != 0) {
        if (addr == uint64_t(uintptr_t(cpu)) && mappedBytes >= bytes) {
            outBo = oldBo;
            outBo.parked = false;
            outBo.cpu = cpu;
            outBo.mapped = mappedBytes;
            unparkResident = true;
            return true;
        }
        // Kernel already moved the BO back to Live with a live IOMemoryMap at addr; unmap and free it cleanly.
        oldBo.parked = false;
        oldBo.cpu = reinterpret_cast<uint8_t *>(uintptr_t(addr));
        oldBo.mapped = mappedBytes;
        k_.free(oldBo);
        unparkMismatch = true;
        return false;
    }
    if (kr == KERN_SUCCESS && status == TegraGM20BUnparkReclaimed) {
        unparkReclaimed = true;
        needFresh = true;
    }
    if (!needFresh)
        return false;
    Bo fresh;
    bool ok = k_.alloc(bytes, true, fresh, kind, kArenaGranule, false, false, false, va, true);
    ok = ok && (bufferVa == va || k_.bindAlias(fresh, bufferVa, kPteKindPitch));
    for (const Arena::View &v : views)
        ok = ok && k_.bindAlias(fresh, v.va, v.kind);
    // Fixed-VA map last so bind failures leave the parked reservation intact, and free the reclaimed handle only after
    // the new mapping owns the range (switch_vm_drop_parked_range skips live BO entries).
    if (ok && k_.mapAt(fresh.token, bytes, uint64_t(uintptr_t(cpu)))) {
        fresh.cpu = cpu;
        fresh.mapped = bytes;
        outBo = fresh;
        if (oldBo.handle)
            k_.free(oldBo);
        return true;
    }
    k_.free(fresh);
    return false;
}

bool Device::purgeKernelArenaLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index)
{
    Arena &initial = arenas_[index];
    const uint64_t bytes = initial.bytes;
    Bo bo = initial.bo;
    initial.bo = {};
    retiring_.heapBytes.fetch_add(bytes, std::memory_order_relaxed);
    SWITCH_METAL_DIAGNOSTIC(stats.poolBytes -= bytes);
    l.unlock();
    bool parked = false;
    if (bo.handle && k_.park(bo.handle, false) == KERN_SUCCESS) {
        bo.parked = true;
        bo.cpu = nullptr;
        bo.mapped = 0;
        parked = true;
    }
    retiring_.heapBytes.fetch_sub(bytes, std::memory_order_relaxed);
    l.lock();
    Arena &a = arenas_[index];
    if (parked) {
        a.bo = bo;
        a.boParked = true;
        SWITCH_METAL_DIAGNOSTIC(stats.parkedHeaps++);
        SWITCH_METAL_DIAGNOSTIC(stats.parkedHeapBytes += bytes);
        SWITCH_METAL_DIAGNOSTIC(stats.heapParks++);
    } else {
        a.bo = bo;
        a.purged = false;
        a.boParked = false;
        purgedArenas_--;
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += bytes);
    }
    a.busy = false;
    arenaReady_.notify_all();
    return parked;
}

}  // namespace gm20b
