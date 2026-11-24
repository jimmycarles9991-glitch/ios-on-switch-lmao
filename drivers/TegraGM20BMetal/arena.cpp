// SPDX-License-Identifier: MIT
// See gpu.h (createArena). An arena's GPU VA is a reservation and its host range is never given back while it lives, so a
// purge and a restore rewrite no texture header or CPU pointer. A private heap's arena is one BO the kernel serves from its
// zeroed spares, bound at the reservation and never mapped for the CPU: a purge frees it, a restore allocates and binds
// another. A shared heap's is its own write-combined pages, imported: a purge frees the BO and replaces the host range with
// untouched pages, a restore imports them again, and a trim (arena_trim.h) gives back only the ranges no resource is
// placed in, the same way, and binds the rest in parts.
#include "lock.h"
#include "gpu.h"
#include "TegraGM20BLayout.h"
#include "arena_purge.h"

#include <mach/mach.h>
#include <mach/memory_object_types.h>
#include <os/log.h>
#include <stdlib.h>

namespace gm20b {
namespace {
constexpr uint64_t kArenaGranule = 0x10000;  // the GMMU big page every VA range starts and ends on (kernel.cpp)
constexpr uint64_t kArenaPitchAlign = 256;   // a pitch render target's address and rows (linearTarget), a cbuf's base

// A shared heap's host pages at `at` (`fixed`: in place of the ones there), write-combined as the device's shared buffers
// are (Buffer::wc): a heap used whole (useHeap) leaves no resource of it for an encoder to clean, so what the CPU writes
// there must need no clean.
bool hostPages(uint64_t bytes, vm_address_t &at, bool fixed)
{
    const int place = fixed ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE : VM_FLAGS_ANYWHERE;
    memory_object_size_t size = bytes;
    mach_port_t entry = MACH_PORT_NULL;
    vm_prot_t permission = MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE;
    SET_MAP_MEM(MAP_MEM_WCOMB, permission);
    if (mach_make_memory_entry_64(mach_task_self(), &size, 0, permission, &entry, MACH_PORT_NULL) != KERN_SUCCESS)
        return false;
    const kern_return_t kr = vm_map(mach_task_self(), &at, vm_size_t(bytes), 0, place, entry, 0, FALSE,
                                    VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    mach_port_deallocate(mach_task_self(), entry);
    return kr == KERN_SUCCESS;
}

// A private heap's host range: addresses only, which no access can ever reach. The CPU never reads or writes a private
// heap, and a path that did would fault here instead of writing pages the GPU never reads.
bool reservedPages(uint64_t bytes, vm_address_t &at)
{
    if (vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    if (vm_protect(mach_task_self(), at, bytes, TRUE, VM_PROT_NONE) == KERN_SUCCESS)
        return true;
    vm_deallocate(mach_task_self(), at, bytes);
    return false;
}

// `bo` bound again at `va` as a member of the set there too: the set finds a use by its VA
void addAlias(ResourceSet &set, Kernel &k, const Bo &bo, uint64_t va)
{
    Bo alias = bo;
    alias.va = alias.vaBase = va;
    set.add(k, alias, TegraGM20BSurfaceRead);
}
}  // namespace

bool Device::arenaFootprint(uint32_t width, uint32_t height, uint32_t format, bool blockLinear, uint64_t &bytes,
                            uint64_t &align)
{
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = blockLinear;
    return footprint(ask, bytes, align);
}

void Device::waitArena(std::unique_lock<gm20b::Mutex> &l, uint32_t index)
{
    arenaReady_.wait(l, [&] {
        return index >= arenas_.size() || !arenas_[index].busy;
    });
}

bool Device::restoreArenaLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index)
{
    if (index >= arenas_.size() || !arenas_[index].purged)
        return true;
    Arena &initial = arenas_[index];
    initial.busy = true;
    const bool wc = initial.wc;
    const bool kernelBo = initial.kernelBo;
    const bool wasParked = initial.boParked;
    Bo oldBo = initial.bo;
    uint8_t *const cpu = initial.cpu;
    const uint64_t va = initial.va, bufferVa = initial.bufferVa, bytes = initial.bytes;
    const uint32_t kind = initial.kind;
    const std::vector<Arena::View> views = initial.views;
    l.unlock();
    Bo bo;
    bool bound = false;
    bool unparkResident = false;
    bool unparkReclaimed = false;
    bool unparkMismatch = false;
    if (kernelBo) {
        bound = restoreKernelArenaOffLock(wasParked, oldBo, cpu, va, bufferVa, bytes, kind, views, bo, unparkResident,
                                          unparkReclaimed, unparkMismatch);
    } else {
        bound = wc ? k_.importPages(cpu, bytes, bo) && k_.bindImportAt(cpu, bo, va, kind)
                   : k_.alloc(bytes, false, bo, kind, kArenaGranule, false, false, false, va);
        bound = bound && (bufferVa == va || k_.bindAlias(bo, bufferVa, kPteKindPitch));
        for (const Arena::View &v : views)
            bound = bound && k_.bindAlias(bo, v.va, v.kind);
        if (!bound) {
            k_.free(bo);
        } else if (!wc) {
            Bo alias = bo;
            alias.va = alias.vaBase = bufferVa;
            set_.add(k_, {{&bo, TegraGM20BSurfaceRead}, {&alias, TegraGM20BSurfaceRead}});
            for (const Arena::View &v : views)
                addAlias(set_, k_, bo, v.va);
        }
    }
    l.lock();
    Arena &a = arenas_[index];
    if (bound) {
        if (wasParked) {
            SWITCH_METAL_DIAGNOSTIC(stats.parkedHeaps--);
            SWITCH_METAL_DIAGNOSTIC(stats.parkedHeapBytes -= bytes);
        }
        if (unparkResident)
            SWITCH_METAL_DIAGNOSTIC(stats.heapUnparksResident++);
        else if (unparkReclaimed)
            SWITCH_METAL_DIAGNOSTIC(stats.heapUnparksReclaimed++);
        a.bo = bo;
        a.boParked = false;
        a.purged = false;
        purgedArenas_--;
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += bytes);
        SWITCH_METAL_DIAGNOSTIC(glassMemEvent("heap restored", bytes));
        SWITCH_METAL_DIAGNOSTIC(stats.arenaRestores++);
        SWITCH_METAL_DIAGNOSTIC(stats.hostZeroedBytes += (wc && !kernelBo) ? bytes : 0);
    } else if (unparkMismatch) {
        if (wasParked) {
            SWITCH_METAL_DIAGNOSTIC(stats.parkedHeaps--);
            SWITCH_METAL_DIAGNOSTIC(stats.parkedHeapBytes -= bytes);
        }
        a.bo = oldBo;
        a.boParked = false;
    }
    a.busy = false;
    arenaReady_.notify_all();
    return bound;
}

bool Device::bindArenaPart(Arena &a, uint64_t offset, uint64_t bytes)
{
    Bo part;
    uint8_t *cpu = a.cpu + offset;
    bool bound = k_.importPages(cpu, bytes, part) && k_.bindImportAt(cpu, part, a.va + offset, a.kind) &&
                 (a.bufferVa == a.va || k_.bindAlias(part, a.bufferVa + offset, kPteKindPitch));
    for (const Arena::View &v : a.views)
        bound = bound && k_.bindAlias(part, v.va + offset, v.kind);
    if (!bound) {
        k_.free(part);
        return false;
    }
    SWITCH_METAL_DIAGNOSTIC(stats.hostZeroedBytes += bytes);
    if (!a.wc) {
        Bo alias = part;
        alias.va = alias.vaBase = a.bufferVa + offset;
        set_.add(k_, {{&part, TegraGM20BSurfaceRead}, {&alias, TegraGM20BSurfaceRead}});
        for (const Arena::View &v : a.views)
            addAlias(set_, k_, part, v.va + offset);
    }
    a.parts.emplace_back(offset, part);
    SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += bytes);
    return true;
}

void Device::unbindArena(Arena &a, DeferredFrees &deferred)
{
    a.holes.clear();
    if (a.parts.empty()) {  // whole, or trimmed to no part at all: then its BO is already gone and counted out
        if (a.bo.handle) {
            SWITCH_METAL_DIAGNOSTIC(stats.poolBytes -= a.bytes);
            deferred.add({a.bo});
            a.bo = {};
        }
        return;
    }
    for (auto &[offset, part] : a.parts) {
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes -= part.bytes);
        deferred.add({part});
    }
    a.parts.clear();
}

bool Device::createArena(uint64_t bytes, bool blockLinear, uint32_t &arena, bool explicitPlacement)
{
    const uint64_t logicalBytes = bytes;
    if (bytes > UINT64_MAX - (kArenaGranule - 1))
        return false;
    bytes = (bytes + kArenaGranule - 1) & ~(kArenaGranule - 1);
    if (!bytes)
        return false;
    const uint64_t maxKernelBo = k_.info().max_bo_bytes;
    const bool kernelBo = !blockLinear && k_.supportsBoPark() && (!maxKernelBo || bytes <= maxKernelBo);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (closing_)
        return false;
    ++offLockChunkOps_;
    l.unlock();
    auto finishOp = [&] {
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
    };
    vm_address_t cpu = 0;
    if (!kernelBo && !(blockLinear ? reservedPages(bytes, cpu) : hostPages(bytes, cpu, false))) {
        l.lock();
        finishOp();
        return false;
    }
    Arena a;
    a.wc = !blockLinear;
    a.kernelBo = kernelBo;
    a.bytes = bytes;
    a.kind = blockLinear ? TGM_LAYOUT_KIND : kPteKindPitch;
    a.cpu = reinterpret_cast<uint8_t *>(cpu);
    a.place = HeapPlacement(logicalBytes, explicitPlacement);
    a.multiKindGen = std::make_shared<std::atomic<uint64_t>>(0);
    a.used = true;
    a.va = k_.reserveVa(bytes);
    // Kind 0xfe changes the sector interpretation inside a GOB. Buffers take a pitch view of the same BO.
    a.bufferVa = blockLinear ? k_.reserveVa(bytes) : a.va;
    auto cleanup = [&] {
        if (a.bo.handle)
            k_.free(a.bo);
        if (a.va)
            k_.releaseVa(a.va, bytes);
        if (a.bufferVa && a.bufferVa != a.va)
            k_.releaseVa(a.bufferVa, bytes);
        if (!kernelBo && cpu)
            vm_deallocate(mach_task_self(), cpu, bytes);
    };
    if (!a.va || !a.bufferVa || !bindArenaOffLock(a)) {
        cleanup();
        l.lock();
        finishOp();
        return false;
    }
    l.lock();
    if (!closing_) {
        a.id = ++nextArenaId_;
        uint32_t i = 0;
        while (i < arenas_.size() && arenas_[i].used)
            i++;
        if (i == arenas_.size())
            arenas_.emplace_back();
        arenas_[i] = std::move(a);
        arena = i;
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += bytes);
        SWITCH_METAL_DIAGNOSTIC(stats.hostZeroedBytes += (!blockLinear && !kernelBo) ? bytes : 0);
        SWITCH_METAL_DIAGNOSTIC(stats.arenasMade++);
        finishOp();
        return true;
    }
    l.unlock();
    cleanup();
    l.lock();
    finishOp();
    return false;
}

void Device::freeArena(Arena &a, DeferredFrees &deferred)
{
    const bool hadGpuHeap = !a.purged && !a.wc;
    const bool hadSharedHeap = !a.purged && a.wc;
    if (a.purged) {
        purgedArenas_--;
        if (a.kernelBo && a.bo.handle) {
            if (a.boParked) {
                SWITCH_METAL_DIAGNOSTIC(stats.parkedHeaps--);
                SWITCH_METAL_DIAGNOSTIC(stats.parkedHeapBytes -= a.bytes);
            }
            deferred.add({a.bo});
            a.bo = {};
            a.boParked = false;
        }
    } else {
        unbindArena(a, deferred);
    }
    if (hadGpuHeap)
        deferred.add({{}, 0, 0, nullptr, 0, nullptr, a.bytes, 0, RetireKind::GpuHeap});
    // after its BOs: no other arena binds at these VAs, nor maps this host range, before they are unbound
    deferred.add({{}, a.va, a.bytes});
    if (a.bufferVa != a.va)
        deferred.add({{}, a.bufferVa, a.bytes});
    for (const Arena::View &v : a.views)
        deferred.add({{}, v.va, a.bytes});
    void *const hostToFree = a.kernelBo ? nullptr : a.cpu;
    deferred.add({{},
                  0,
                  0,
                  hostToFree,
                  hostToFree ? a.bytes : 0,
                  nullptr,
                  hadSharedHeap ? a.bytes : 0,
                  0,
                  hadSharedHeap ? RetireKind::SharedHeap : RetireKind::None});
    a = Arena();
}

void Device::destroyArena(uint32_t arena)
{
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    if (arena >= arenas_.size() || !arenas_[arena].used)
        return;
    Arena &a = arenas_[arena];
    a.dying = true;
    if (!a.leases && !a.aliasLeases)
        freeArena(a, deferred);
}

void Device::arenaFree(std::unique_lock<gm20b::Mutex> &l, Span &s, DeferredFrees &deferred)
{
    waitArena(l, s.chunk);
    Arena &a = arenas_[s.chunk];
    delete s.storage;
    a.place.release(s.offset, s.bytes);
    s = {};
    if (a.dying && !a.leases && !a.aliasLeases)
        freeArena(a, deferred);
}

uint64_t Device::arenaView(Arena &a, uint32_t kind)
{
    if (kind == a.kind)
        return a.va;
    if (kind == kPteKindPitch && a.bufferVa != a.va)
        return a.bufferVa;
    for (const Arena::View &v : a.views)
        if (v.kind == kind)
            return v.va;
    const uint64_t va = k_.reserveVa(a.bytes);
    if (!va)
        return 0;
    if (a.bo.handle && !k_.bindAlias(a.bo, va, kind)) {
        k_.releaseVa(va, a.bytes);
        return 0;
    }
    // A trimmed arena's view (arena_trim.h) is bound part by part, its holes as backPlacement binds them. A part bound
    // here before a refusal stays bound until its BO goes, so the VA is kept from any other bind.
    bool bound = true;
    for (const auto &[offset, part] : a.parts)
        bound = bound && k_.bindAlias(part, va + offset, kind);
    if (!bound)
        return 0;
    if (!a.wc) {
        if (a.bo.handle)
            addAlias(set_, k_, a.bo, va);
        for (const auto &[offset, part] : a.parts)
            addAlias(set_, k_, part, va + offset);
    }
    a.views.push_back({va, kind});
    return va;
}

bool Device::arenaSpan(std::unique_lock<gm20b::Mutex> &l, const ArenaAt &at, uint64_t bytes, uint64_t align,
                       uint32_t kind, Texture &t)
{
    waitArena(l, at.arena);
    if (at.arena >= arenas_.size() || !arenas_[at.arena].used || arenas_[at.arena].dying)
        return false;
    if (!restoreArenaLocked(l, at.arena))
        return false;
    Arena &a = arenas_[at.arena];
    uint64_t offset = at.offset.value_or(0);
    const uint64_t view = arenaView(a, kind);
    bool crossed = false;
    if (!view || !(at.offset ? a.place.placeAt(bytes, align, offset, kind, &crossed)
                             : a.place.place(bytes, align, offset, kind, &crossed)))
        return false;
    if (!backPlacement(a, offset, bytes)) {
        a.place.release(offset, bytes);
        return false;
    }
    if (crossed && a.multiKindGen)
        a.multiKindGen->fetch_add(1, std::memory_order_release);
    t.span = {at.arena, offset, bytes, view + offset, a.cpu + offset};
    t.span.storage = arenaStorage(a, at.arena, offset, bytes, t.span.va, kind);
    t.arena = true;
    t.wc = a.wc;
    return true;
}

bool Device::arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t,
                          std::optional<uint64_t> requestedOffset)
{
    bool blockLinear = false;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        if (arena >= arenas_.size() || !arenas_[arena].used)
            return false;
        blockLinear = arenas_[arena].kind != kPteKindPitch;
    }
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = blockLinear;
    return arenaTexture({arena, requestedOffset}, ask, t);
}

// An extended-range texture's stand-in follows it at the stand-in's alignment (footprint), where a placement heap's
// offset names the texture's
bool Device::arenaTexture(const ArenaAt &at, const TextureAsk &ask, Texture &t)
{
    const bool made = ask.kind == TextureAsk::Plain
                          ? createTexture(ask.width, ask.height, ask.format, t, false, ask.tiled, &at)
                          : createLaidOut(ask, t, &at);
    if (!made || !ask.standIn || !ownPipeline(xrConversionOf(t.format)))
        return made;
    ArenaAt standIn = at;
    uint64_t bytes = 0, align = 0;
    if (!footprint(standInAsk(t), bytes, align)) {
        destroyTexture(t);
        return false;
    }
    if (at.offset)
        standIn.offset = (*at.offset + t.span.bytes + align - 1) / align * align;
    if (!createStandIn(t, &standIn)) {
        destroyTexture(t);
        return false;
    }
    return true;
}

bool Device::arenaBuffer(uint32_t arena, uint64_t length, Buffer &b, std::optional<uint64_t> requestedOffset)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying ||
        (arenas_[arena].kind != kPteKindPitch && arenas_[arena].bufferVa == arenas_[arena].va) ||
        length > UINT64_MAX - (kArenaPitchAlign - 1))
        return false;
    if (!restoreArenaLocked(l, arena))
        return false;
    Arena &a = arenas_[arena];
    const uint64_t bytes = (std::max<uint64_t>(length, 16) + kArenaPitchAlign - 1) & ~(kArenaPitchAlign - 1);
    uint64_t offset = requestedOffset.value_or(0);
    bool crossed = false;
    if (!(requestedOffset ? a.place.placeAt(bytes, kArenaPitchAlign, offset, kPteKindPitch, &crossed)
                          : a.place.place(bytes, kArenaPitchAlign, offset, kPteKindPitch, &crossed)))
        return false;
    if (!backPlacement(a, offset, bytes)) {
        a.place.release(offset, bytes);
        return false;
    }
    if (crossed && a.multiKindGen)
        a.multiKindGen->fetch_add(1, std::memory_order_release);
    b = {};
    b.span = {arena, offset, bytes, a.bufferVa + offset, a.cpu + offset};
    b.span.storage = arenaStorage(a, arena, offset, bytes, b.span.va, kPteKindPitch);
    b.arena = true;
    b.wc = a.wc;
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = std::max<uint64_t>(length, 16);
    return true;
}

void Device::arenaRelease(Texture &t)
{
#if SWITCH_METAL_DIAGNOSTICS
    if (t.arena && glassMemOn.load(std::memory_order_relaxed))
        glassMem->forget(t.storageRange().va);
#endif
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (t.arena)
        freeSpan(l, t.span, Memory::Arena, t.lease, deferred);
}

void Device::arenaRelease(Buffer &b)
{
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (b.arena)
        freeSpan(l, b.span, Memory::Arena, b.lease, deferred);
}

uint64_t Device::arenaUsed(uint32_t arena)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.used() : 0;
}

uint64_t Device::arenaLargestFree(uint32_t arena, uint64_t align)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.largestFree(std::max<uint64_t>(align, 1)) : 0;
}

bool Device::setArenaVolatile(uint32_t arena, bool isVolatile)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    return arena < arenas_.size() && arenaSetVolatile(arenas_[arena], !isVolatile);
}

bool Device::setArenaVolatileIfPurged(uint32_t arena)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    if (arena >= arenas_.size() || !arenas_[arena].used || !arenas_[arena].purged)
        return false;
    arenas_[arena].isVolatile = true;
    return true;
}

uint64_t Device::arenaBytes()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    uint64_t bytes = 0;
    for (const Arena &a : arenas_)
        bytes += a.used && !a.purged ? a.bytes - rangeBytes(a.holes) : 0;
    return bytes;
}

void Device::arenaUse(uint64_t &placed, uint64_t &volatileBytes, uint64_t *givenBack)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    placed = volatileBytes = 0;
    uint64_t given = 0;
    for (const Arena &a : arenas_)
        if (a.used && !a.purged) {
            placed += std::min(a.place.used(), a.bytes);
            volatileBytes += a.isVolatile ? a.bytes : 0;
            given += rangeBytes(a.holes);
        }
    if (givenBack)
        *givenBack = given;
}

bool Device::arenaPurged(uint32_t arena)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    return arena < arenas_.size() && arenas_[arena].purged;
}

bool Device::hasBoundVolatileArena()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    for (const Arena &a : arenas_)
        if (a.used && !a.purged && a.isVolatile)
            return true;
    return false;
}

// lock_ held through `l`. The commit path counts itself in `committed` before it counts purged arenas, so one of the
// two sees the other: a command buffer committed meanwhile keeps the pages. The slot stays busy while BoFree and
// hostPages run with lock_ let go so no restore, destroy or access can touch the same VAs or host range until they end.
bool Device::purgeLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index, const std::atomic<uint32_t> &committed,
                         bool *parkBusy)
{
    if (parkBusy)
        *parkBusy = false;
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return false;
    }
    Arena &initial = arenas_[index];
    const bool wc = initial.wc;
    const bool kernelBo = initial.kernelBo;
    uint8_t *const cpu = initial.cpu;
    const uint64_t bytes = initial.bytes;
    initial.busy = true;
    initial.purged = true;
    if (kernelBo) {
        const bool parked = purgeKernelArenaLocked(l, index);
        if (!parked && parkBusy)
            *parkBusy = true;
        return parked;
    }
    DeferredFrees deferred(*this);
    unbindArena(initial, deferred);
    if (wc)
        retiring_.heapBytes.fetch_add(bytes, std::memory_order_relaxed);
    else
        deferred.add({{}, 0, 0, nullptr, 0, nullptr, bytes, 0, RetireKind::GpuHeap});
    SWITCH_METAL_DIAGNOSTIC(glassMemEvent("heap purged", bytes));
    l.unlock();
    deferred.flush();
    if (wc) {
        vm_address_t at = vm_address_t(cpu);
        hostPages(bytes, at, true);
        retiring_.heapBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
    l.lock();
    arenas_[index].busy = false;
    arenaReady_.notify_all();
    return true;
}

void Device::purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    if (arena < arenas_.size() && arenaPurgeable(arenas_[arena], done(submitted()), committed.load() != 0, true))
        purgeLocked(l, arena, committed);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void Device::purgeArenas(const std::atomic<uint32_t> &committed, PurgeStats *stats)
{
    if (stats)
        *stats = {};
    std::unique_lock<gm20b::Mutex> l(lock_);
    for (uint32_t i = 0; i < arenas_.size(); ++i) {
        const Arena &a = arenas_[i];
        if (!a.used || a.purged || !a.isVolatile)
            continue;
        const bool idle = done(submitted());
        const uint32_t committedNow = committed.load();
        const uint64_t bytes = a.bytes;
        const bool nonEmpty = a.place.used() != 0;
        if (!a.busy && arenaPurgeable(a, idle, committedNow != 0, false)) {
            bool parkBusy = false;
            if (purgeLocked(l, i, committed, &parkBusy)) {
                if (stats) {
                    stats->purged++;
                    stats->purgedBytes += bytes;
                }
                continue;
            }
            if (stats) {
                stats->skipped++;
                stats->skippedBytes += bytes;
                if (parkBusy) {
                    stats->busy++;
                } else {
                    const uint32_t after = committed.load();
                    stats->committed++;
                    stats->committedCount = std::max(stats->committedCount, after ? after : 1u);
                }
                if (nonEmpty)
                    stats->nonEmpty++;
            }
            continue;
        }
        if (stats) {
            stats->skipped++;
            stats->skippedBytes += bytes;
            if (a.busy)
                stats->busy++;
            if (!idle)
                stats->inFlight++;
            if (committedNow) {
                stats->committed++;
                stats->committedCount = std::max(stats->committedCount, committedNow);
            }
            if (a.leases)
                stats->leases++;
            if (nonEmpty)
                stats->nonEmpty++;
        }
    }
}
#else
void Device::purgeArenas(const std::atomic<uint32_t> &committed)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    for (uint32_t i = 0; i < arenas_.size(); ++i)
        if (!arenas_[i].busy && arenaPurgeable(arenas_[i], done(submitted()), committed.load() != 0, false))
            purgeLocked(l, i, committed);
}
#endif

// lock_ held through `l`, the device idle: the import goes (no GPU work can read it), each hole's host range is
// replaced by untouched pages, and the placed parts are imported again at their VAs, all with lock_ let go while the
// slot is marked busy. It counts as a purge while it runs (purgeLocked's order with the commit path), so a command
// buffer committed meanwhile waits in restoreArena for the parts to be bound again, and one committed before it keeps
// the arena as it is. A part the kernel refuses binds the whole arena again.
void Device::trimLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index, const std::atomic<uint32_t> &committed)
{
    Arena &initial = arenas_[index];
    std::vector<ArenaRange> holes = arenaHoles(initial.place, initial.bytes, kArenaGranule);
    if (holes.empty() || holes == initial.holes)
        return;
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return;
    }
    initial.busy = true;
    uint8_t *const cpu = initial.cpu;
    const uint64_t va = initial.va, bufferVa = initial.bufferVa, bytes = initial.bytes;
    const uint32_t kind = initial.kind;
    const std::vector<Arena::View> views = initial.views;
    DeferredFrees deferred(*this);
    unbindArena(initial, deferred);
    l.unlock();
    deferred.flush();
    for (const ArenaRange &h : holes) {
        vm_address_t at = vm_address_t(cpu + h.offset);
        hostPages(h.bytes, at, true);
    }
    std::vector<std::pair<uint64_t, Bo>> parts;
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t partsBytes = 0, zeroedBytes = 0;
#endif
    ArenaRange refused = {0, 0};
    auto bindOne = [&](uint64_t offset, uint64_t len, Bo &bo) {
        uint8_t *at = cpu + offset;
        bool bound = k_.importPages(at, len, bo) && k_.bindImportAt(at, bo, va + offset, kind) &&
                     (bufferVa == va || k_.bindAlias(bo, bufferVa + offset, kPteKindPitch));
        for (const Arena::View &v : views)
            bound = bound && k_.bindAlias(bo, v.va + offset, v.kind);
        if (!bound) {
            k_.free(bo);
            return false;
        }
        return true;
    };
    for (const ArenaRange &b : arenaBacked(holes, bytes)) {
        if (refused.bytes)
            break;
        Bo part;
        if (bindOne(b.offset, b.bytes, part)) {
            parts.emplace_back(b.offset, part);
            SWITCH_METAL_DIAGNOSTIC(partsBytes += b.bytes);
            SWITCH_METAL_DIAGNOSTIC(zeroedBytes += b.bytes);
        } else {
            refused = b;
        }
    }
    Bo wholeBo;
    bool whole = false;
    if (refused.bytes) {
        for (auto &[offset, part] : parts)
            k_.free(part);
        parts.clear();
        whole = bindOne(0, bytes, wholeBo);
        SWITCH_METAL_DIAGNOSTIC(if (whole) zeroedBytes += bytes);
        os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                     "SwitchMetal: %{public}s: the kernel refused a trimmed heap's part of %llu bytes at %llu: the "
                     "heap of %llu bytes is %{public}s",
                     getprogname(), (unsigned long long)refused.bytes, (unsigned long long)refused.offset,
                     (unsigned long long)bytes, whole ? "bound whole again" : "purged, its pages refused too");
    }
    l.lock();
    Arena &a = arenas_[index];
    SWITCH_METAL_DIAGNOSTIC(stats.hostZeroedBytes += zeroedBytes);
    if (!refused.bytes) {
        a.parts = std::move(parts);
        a.holes = std::move(holes);
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += partsBytes);
        SWITCH_METAL_DIAGNOSTIC(glassMemEvent("heap trimmed", rangeBytes(a.holes)));
    } else if (whole) {
        a.bo = wholeBo;
        SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += bytes);
    } else {
        a.purged = true;
        purgedArenas_++;
    }
    a.busy = false;
    purgedArenas_--;
    arenaReady_.notify_all();
}

void Device::trimArenas(const std::atomic<uint32_t> &committed)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    for (uint32_t i = 0; i < arenas_.size(); ++i) {
        if (!done(submitted()))
            return;
        const Arena &a = arenas_[i];
        if (a.used && a.wc && !a.kernelBo && !a.purged && !a.dying && !a.leases && !a.aliasLeases && !a.isVolatile &&
            !a.busy)
            trimLocked(l, i, committed);
    }
}

bool Device::backPlacement(Arena &a, uint64_t offset, uint64_t bytes)
{
    if (a.holes.empty())
        return true;
    const auto taken = takeHoles(a.holes, offset, bytes, kArenaGranule);
    for (size_t i = 0; i < taken.size(); ++i)
        if (!bindArenaPart(a, taken[i].offset, taken[i].bytes)) {
            a.holes.insert(a.holes.end(), taken.begin() + i, taken.end());
            std::sort(a.holes.begin(), a.holes.end(), [](const ArenaRange &x, const ArenaRange &y) {
                return x.offset < y.offset;
            });
            return false;
        }
    return true;
}

const ArenaStorage *Device::arenaStorage(const Arena &arena, uint32_t index, uint64_t offset, uint64_t bytes,
                                         uint64_t va, uint32_t kind) const
{
    auto *storage = new ArenaStorage;
    storage->arena = index;
    storage->offset = offset;
    storage->cpu = arena.cpu + offset;
    storage->arenaId = arena.id;
    storage->kind = kind;
    storage->multiKindGen = arena.multiKindGen;
    if (arena.bo.handle) {
        storage->first = {va, bytes};
    } else {
        const auto include = [&](StoragePiece piece) {
            if (!storage->first.bytes)
                storage->first = piece;
            else if (piece.va < storage->first.va) {
                storage->rest.push_back(storage->first);
                storage->first = piece;
            } else {
                storage->rest.push_back(piece);
            }
        };
        for (const auto &[start, part] : arena.parts) {
            const uint64_t first = std::max(offset, start), last = std::min(offset + bytes, start + part.bytes);
            if (first < last)
                include({va + first - offset, last - first});
        }
        std::sort(storage->rest.begin(), storage->rest.end(), [](const auto &a, const auto &b) {
            return a.va < b.va;
        });
    }
    return storage;
}

bool Device::restoreArena(uint32_t arena)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitArena(l, arena);
    return arena >= arenas_.size() || restoreArenaLocked(l, arena);
}

}  // namespace gm20b
