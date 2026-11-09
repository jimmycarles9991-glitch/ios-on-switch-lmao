// SPDX-License-Identifier: MIT
// See raw_gpu.h (createArena). An arena's GPU VA is a reservation and its host range is never given back while it lives: a
// purge frees the BO (the kernel releases the pages) and replaces the host range with untouched pages, so neither address
// moves and a restore is one import and one bind at the same VA, no texture header or CPU pointer rewritten.
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>

namespace raw {
namespace {
constexpr uint64_t kArenaGranule = 0x10000;  // the GMMU big page every VA range starts and ends on (raw_kernel.cpp)
constexpr uint64_t kArenaPitchAlign = 256;   // a pitch render target's address and rows (linearTarget), a cbuf's base
}  // namespace

bool Device::arenaFootprint(uint32_t width, uint32_t height, uint32_t format, bool blockLinear, uint64_t &bytes,
                            uint64_t &align)
{
    Texture t;
    if (!shape(width, height, format, t) || t.blockW > 1 || t.blockH > 1)
        return false;
    if (blockLinear) {
        tgm_layout bl;
        if (!t.renderable || !tgm_layout_tiled(width, height, t.bpp, &bl))
            return false;
        bytes = (bl.bytes + kBlAlign - 1) & ~(kBlAlign - 1);
        align = std::max<uint64_t>(bl.align, kBlAlign);
        return true;
    }
    const uint64_t pitch = (uint64_t(width) * t.bpp + 255) & ~uint64_t(255);
    bytes = pitch * height;
    align = kArenaPitchAlign;
    return true;
}

// The host range's pages wired and bound at the arena's VA, under its kind (lock_ held)
bool Device::bindArena(Arena &a)
{
    if (!k_.importPages(a.cpu, a.bytes, a.bo) || !k_.bindImportAt(a.cpu, a.bo, a.va, a.kind)) {
        a.bo = {};
        return false;
    }
    stats.poolBytes += a.bytes;
    return true;
}

bool Device::createArena(uint64_t bytes, bool blockLinear, uint32_t &arena)
{
    bytes = (bytes + kArenaGranule - 1) & ~(kArenaGranule - 1);
    vm_address_t cpu = 0;
    if (!bytes || vm_allocate(mach_task_self(), &cpu, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    Arena a;
    a.bytes = bytes;
    a.kind = blockLinear ? TGM_LAYOUT_KIND : kPteKindPitch;
    a.cpu = reinterpret_cast<uint8_t *>(cpu);
    a.place = HeapPlacement(bytes);
    a.used = true;
    std::lock_guard<std::mutex> l(lock_);
    a.va = k_.reserveVa(bytes);
    if (!a.va || !bindArena(a)) {
        if (a.va)
            k_.releaseVa(a.va, bytes);
        vm_deallocate(mach_task_self(), cpu, bytes);
        return false;
    }
    uint32_t i = 0;
    while (i < arenas_.size() && arenas_[i].used)
        i++;
    if (i == arenas_.size())
        arenas_.emplace_back();
    arenas_[i] = std::move(a);
    arena = i;
    return true;
}

void Device::freeArena(Arena &a)
{
    if (a.purged) {
        purgedArenas_--;
    } else {
        k_.free(a.bo);
        stats.poolBytes -= a.bytes;
    }
    k_.releaseVa(a.va, a.bytes);
    vm_deallocate(mach_task_self(), vm_address_t(a.cpu), a.bytes);
    a = Arena();
}

void Device::destroyArena(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used)
        return;
    Arena &a = arenas_[arena];
    a.dying = true;
    if (!a.leases)
        freeArena(a);
}

void Device::arenaFree(Span &s)
{
    Arena &a = arenas_[s.chunk];
    a.place.release(s.offset, s.bytes);
    s = {};
    if (a.dying && !a.leases)
        freeArena(a);
}

bool Device::aliasArena(const Span &s, Lease *&lease, Alias &a)
{
    const Arena &ar = arenas_[s.chunk];
    if (ar.purged)
        return false;
    a.cpu = ar.cpu;
    a.bytes = ar.bytes;
    a.handle = ar.bo.handle;
    a.va = ar.va;
    a.kind = ar.kind;
    a.lease = leaseSpan(lease, s, Memory::Arena);
    return true;
}

bool Device::arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying)
        return false;
    Arena &a = arenas_[arena];
    const bool blockLinear = a.kind == TGM_LAYOUT_KIND;
    uint64_t bytes, align, offset;
    if (!arenaFootprint(width, height, format, blockLinear, bytes, align) || !shape(width, height, format, t) ||
        !a.place.place(bytes, align, offset))
        return false;
    t.span = {arena, offset, bytes, a.va + offset, a.cpu + offset};
    t.arena = true;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    tgm_layout bl;
    if (blockLinear && tgm_layout_tiled(width, height, t.bpp, &bl)) {
        t.pitch = bl.row_bytes;
        t.blockLinear = true;
        t.blockHeight = bl.y_log2;
        t.layerBytes = bl.bytes;
    } else {
        t.pitch = (width * t.bpp + 255) & ~255u;
    }
    if (!(t.tic = allocTic())) {
        arenaFree(t.span);
        t = {};
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::arenaBuffer(uint32_t arena, uint64_t length, Buffer &b)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying || arenas_[arena].kind != kPteKindPitch)
        return false;
    Arena &a = arenas_[arena];
    const uint64_t bytes = (std::max<uint64_t>(length, 16) + kArenaPitchAlign - 1) & ~(kArenaPitchAlign - 1);
    uint64_t offset;
    if (!a.place.place(bytes, kArenaPitchAlign, offset))
        return false;
    b = {};
    b.span = {arena, offset, bytes, a.va + offset, a.cpu + offset};
    b.arena = true;
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = std::max<uint64_t>(length, 16);
    return true;
}

void Device::arenaRelease(Texture &t)
{
    std::lock_guard<std::mutex> l(lock_);
    if (t.arena)
        freeSpan(t.span, Memory::Arena, t.lease);
}

void Device::arenaRelease(Buffer &b)
{
    std::lock_guard<std::mutex> l(lock_);
    if (b.arena)
        freeSpan(b.span, Memory::Arena, b.lease);
}

uint64_t Device::arenaUsed(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.used() : 0;
}

uint64_t Device::arenaLargestFree(uint32_t arena, uint64_t align)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.largestFree(std::max<uint64_t>(align, 1)) : 0;
}

void Device::setArenaVolatile(uint32_t arena, bool isVolatile)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena < arenas_.size())
        arenas_[arena].isVolatile = isVolatile;
}

uint64_t Device::arenaBytes()
{
    std::lock_guard<std::mutex> l(lock_);
    uint64_t bytes = 0;
    for (const Arena &a : arenas_)
        bytes += a.used && !a.purged ? a.bytes : 0;
    return bytes;
}

bool Device::arenaPurged(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() && arenas_[arena].purged;
}

void Device::purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size())
        return;
    Arena &a = arenas_[arena];
    if (!a.used || a.purged || a.leases || !done(submitted()))
        return;
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return;
    }
    k_.free(a.bo);
    vm_address_t at = vm_address_t(a.cpu);
    vm_allocate(mach_task_self(), &at, a.bytes, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    stats.poolBytes -= a.bytes;
    a.purged = true;
}

void Device::purgeArenas(const std::atomic<uint32_t> &committed)
{
    std::vector<uint32_t> volatileArenas;
    {
        std::lock_guard<std::mutex> l(lock_);
        for (uint32_t i = 0; i < arenas_.size(); i++)
            if (arenas_[i].used && arenas_[i].isVolatile && !arenas_[i].purged)
                volatileArenas.push_back(i);
    }
    for (const uint32_t i : volatileArenas)
        purgeArena(i, committed);
}

bool Device::restoreArena(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].purged)
        return true;
    Arena &a = arenas_[arena];
    if (!bindArena(a))
        return false;
    a.purged = false;
    purgedArenas_--;
    return true;
}

}  // namespace raw
