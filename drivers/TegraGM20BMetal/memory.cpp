// SPDX-License-Identifier: MIT
// See gpu.h (the pool, block-linear and write-combined chunks, imports, buffers).
#include "lock.h"
#include "follow.h"
#include "gpu.h"
#include "surface_view.h"
#include "TegraGM20BLayout.h"
#include "grow_ahead.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <iterator>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
constexpr size_t kDedicatedSpares = 2;
}  // namespace

// ---- device: memory ----

// A span of `bytes` (page multiple) from the pool: first fit in a chunk, else a new chunk (newChunk). A span of more
// than half a chunk gets a chunk of its own, given back whole when the span is freed. The shared chunks grow ahead of need: once their
// free bytes fall below a chunk's worth the grower thread makes the next one, so this (the render thread's resource creation) finds room;
// one that does not waits for a chunk the grower is making, or makes one itself (Stats::poolStalls: a burst outran the mark).

// The size of a new shared chunk of a kind that must hold `need` bytes: the role's (`cap`) for the render server; for a client,
// what it already holds of that kind, from a sixteenth of the role's to all of it, so its chunks double as it draws more
uint64_t Device::chunkSize(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t need, uint64_t align) const
{
    uint64_t size = cap;
    if (role_ == Role::Client) {
        uint64_t held = 0;
        for (const Chunk &ch : chunks)
            if (ch.park == ChunkPark::Live && ch.memory && !ch.dedicated)
                held += ch.bytes;
        size = std::clamp(held, cap / kClientFirstShare, cap);
    }
    size = std::max(size, need);
    return (size + align - 1) & ~(align - 1);
}

// lock_ held through `l` (released across the kernel's call); the chunk's index in chunks_
bool Device::newChunk(uint64_t size, bool dedicated, std::unique_lock<gm20b::Mutex> &l, uint32_t &index)
{
    if (closing_)
        return false;
    Chunk ch;
    ch.dedicated = dedicated;
    ++offLockChunkOps_;
    l.unlock();
    const bool made = k_.alloc(size, false, ch.bo, kPteKindPitch, kChunkVaAlign);
    if (made)
        set_.add(k_, ch.bo, TegraGM20BSurfaceRead);
    l.lock();
    if (!made) {
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        return false;
    }
    if (closing_) {
        l.unlock();
        k_.free(ch.bo);
        l.lock();
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        return false;
    }
    if (--offLockChunkOps_ == 0)
        chunkReady_.notify_all();
    ch.memory = ch.bo.cpu;
    ch.bytes = size;
    uint32_t c = 0;
    while (c < chunks_.size() && chunkSlotOccupied(chunks_[c]))
        c++;
    if (c == chunks_.size())
        chunks_.emplace_back();
    if (!dedicated) {
        ch.free[0] = size;
        poolFree_ += size;
    }
    chunks_[c] = std::move(ch);
    SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += size);
    SWITCH_METAL_DIAGNOSTIC(stats.poolChunks++);
    index = c;
    return true;
}

// The next chunk the grower makes: the render server's chunk; a client's next chunk of its own size (chunkSize), so a
// client's chunk ahead is no bigger than the one it would stall for
uint64_t Device::growChunk(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t align) const
{
    return role_ == Role::Client ? chunkSize(chunks, cap, 0, align) : cap;
}

bool Device::growAheadRoom(uint64_t chunk)
{
    if (role_ != Role::Client)
        return true;
    uint64_t footprint = 0, available = 0;
    return footprintProbe(footprint, available) && clientGrowAhead(footprint, available, chunk);
}

bool Device::readFootprint(uint64_t &footprint, uint64_t &available)
{
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
        count < TASK_VM_INFO_REV4_COUNT)
        return false;
    footprint = info.phys_footprint;
    available = info.limit_bytes_remaining;
    return true;
}

// lock_ held: a client's grower, at its kClientGrowerStall-th stall. The thread takes lock_ first, so it runs once this
// allocation lets go of it.
void Device::clientStalled()
{
    if (role_ == Role::Client && ++clientStalls_ == kClientGrowerStall && !grower_.joinable())
        startGrower();
}

void Device::startGrower()
{
    grower_ = std::thread([this] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.gm20b.pool");
        followers().join();
        growLoop();
    });
}

void Device::growLoop()
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    for (;;) {
        grow_.wait(l, [&] {
            return stopping_ || growing_ || !tiledWanted_.empty();
        });
        if (stopping_) {
            grown_.notify_all();
            return;
        }
        uint32_t c;
        if (growing_) {
            bool resident = false;
            const uint64_t size = growChunk(chunks_, sizes_.chunk, pageSize());
            while (!stopping_ && !closing_ &&
                   tryUnparkChunk(l, ChunkVec::Pool, 0, size, false, 0, TegraGM20BSurfaceRead, c, resident, size) &&
                   !resident) {
            }
            if (!resident && !stopping_ && !closing_)
                newChunk(size, false, l, c);  // failing, the render thread's next allocation tries itself
            growing_ = false;
        } else {
            // the want stays listed while it is made, so an allocation that finds no room waits for it
            const TiledWant w = tiledWanted_.front();
            const ChunkVec vec = w.chunks == &blChunks_ ? ChunkVec::Bl : ChunkVec::Zeta;
            const uint32_t zetaIndex = vec == ChunkVec::Zeta ? uint32_t(w.chunks - zChunks_) : 0;
            const uint64_t want = chunkSize(*w.chunks, sizes_.blChunk, 0, kBlAlign);
            bool resident = false;
            while (
                !stopping_ && !closing_ &&
                tryUnparkChunk(l, vec, zetaIndex, want, false, kBlAlign, TegraGM20BSurfaceWrite, c, resident, want) &&
                !resident) {
            }
            if (!resident && !stopping_ && !closing_)
                newTiledChunk(*w.chunks, w.kind, want, false, l, c);
            tiledWanted_.erase(tiledWanted_.begin());
        }
        grown_.notify_all();
    }
}

bool Device::poolAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty)
{
    bytes = (bytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    s = {};
    if (dirty)
        *dirty = false;
    // over half a chunk: its own chunk, made now (in the render server rare: no Core Animation texture of the 720p screen is
    // that large)
    if (bytes > sizes_.chunk / 2) {
        for (;;) {
            if (closing_)
                return false;
            uint32_t c = ~0u;
            for (auto it = spares_.begin(); it != spares_.end(); ++it)
                if (chunks_[it->chunk].park == ChunkPark::Live && chunks_[it->chunk].bytes == bytes) {
                    c = it->chunk;
                    spares_.erase(it);
                    break;
                }
            if (c != ~0u) {
                chunks_[c].used = bytes;
                if (dirty)
                    *dirty = true;
                s = {c, 0, bytes, chunks_[c].bo.va, static_cast<uint8_t *>(chunks_[c].memory)};
                return true;
            }
            bool resident = false;
            if (tryUnparkChunk(l, ChunkVec::Pool, 0, bytes, true, 0, TegraGM20BSurfaceRead, c, resident)) {
                if (!resident)
                    continue;
                chunks_[c].used = bytes;
                if (dirty)
                    *dirty = true;
                s = {c, 0, bytes, chunks_[c].bo.va, static_cast<uint8_t *>(chunks_[c].memory)};
                return true;
            }
            if (closing_ || !newChunk(bytes, true, l, c))
                return false;
            chunks_[c].used = bytes;
            chunks_[c].highWater = bytes;
            s = {c, 0, bytes, chunks_[c].bo.va, static_cast<uint8_t *>(chunks_[c].memory)};
            return true;
        }
    }
    for (int pass = 0; pass < 3;) {
        if (closing_)
            return false;
        for (uint32_t c = 0; c < chunks_.size(); c++) {
            Chunk &ch = chunks_[c];
            if (ch.dedicated || ch.park != ChunkPark::Live || !ch.memory)
                continue;
            for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                if (it->second < bytes)
                    continue;
                const uint64_t offset = it->first, left = it->second - bytes;
                ch.free.erase(it);
                if (left)
                    ch.free[offset + bytes] = left;
                if (dirty)
                    *dirty = offset < ch.highWater;
                ch.highWater = std::max(ch.highWater, offset + bytes);
                ch.used += bytes;
                poolFree_ -= bytes;
                s = {c, offset, bytes, ch.bo.va + offset, static_cast<uint8_t *>(ch.memory) + offset};
                const uint64_t next = growChunk(chunks_, sizes_.chunk, pageSize());
                if (poolFree_ < next && !growing_ && grower_.joinable() &&
                    growAheadRoom(next)) {  // the next chunk, off this thread
                    growing_ = true;
                    grow_.notify_one();
                }
                return true;
            }
        }
        uint32_t unparked = ~0u;
        bool resident = false;
        if (tryUnparkChunk(l, ChunkVec::Pool, 0, bytes, false, 0, TegraGM20BSurfaceRead, unparked, resident,
                           chunkSize(chunks_, sizes_.chunk, bytes, pageSize())))
            continue;
        if (closing_ || ++pass == 3)
            break;
        SWITCH_METAL_DIAGNOSTIC(stats.poolStalls++);
        clientStalled();
        if (growing_) {  // the grower is making one: wait for it rather than wire a second
            // bounded, as every wait of the plugin's: a chunk not wired in 20 s is a stuck kernel import, said once and
            // stopped for the stack (a wait with no bound froze r7p8's compositor with nothing in the log)
            ++offLockChunkOps_;
            const bool ready = grown_.wait_for(l, std::chrono::seconds(20), [&] {
                return !growing_ || closing_;
            });
            if (--offLockChunkOps_ == 0)
                chunkReady_.notify_all();
            if (!ready) {
                os_log_fault(
                    gm20b::log(),
                    "SwitchMetal: pool chunk not wired after 20 s (the grower's import): abort() for the stack");
                abort();
            }
            continue;
        }
        uint32_t c;
        if (!newChunk(chunkSize(chunks_, sizes_.chunk, bytes, pageSize()), false, l, c))
            return false;
    }
    return false;
}

void Device::warm()
{
    ensurePackWindow();
    {
        std::unique_lock<gm20b::Mutex> l(lock_);
        if (closing_)
            return;
        uint32_t c;
        while (!closing_ && poolFree_ <= sizes_.chunk && newChunk(sizes_.chunk, false, l, c)) {
        }  // the first and the one ahead of it
        while (!closing_ && tiledFreeBytes(blChunks_) <= sizes_.blChunk &&
               newTiledChunk(blChunks_, fx::kBlockLinearKind, sizes_.blChunk, false, l, c)) {
        }  // and of block-linear images: the first frames' memoryless backings find room before the grower runs
        if (closing_)
            return;
    }
    if (!grower_.joinable())
        startGrower();
}

void Device::poolFree(Span &s, DeferredFrees &deferred)
{
    if (!s.valid() || s.chunk >= chunks_.size())
        return;
    Chunk &ch = chunks_[s.chunk];
    ch.used -= s.bytes;
    if (ch.dedicated) {  // nothing else is in it
        if (spares_.size() < kDedicatedSpares) {
            spares_.push_back({s.chunk, commitCount_});
            s = {};
            return;
        }
        freeChunk(ch, deferred);
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    poolFree_ += bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back while the others keep a chunk's worth free (the grower's own mark), so the pool
    // shrinks after a burst instead of holding its peak for good (r8a: 320 MB in 20 chunks from 15:18 on, the screen idle)
    if (!ch.used && poolFree_ >= ch.bytes + sizes_.chunk) {
        poolFree_ -= ch.bytes;
        freeChunk(ch, deferred);
    }
}

// lock_ held: a chunk's BO is detached for off-lock free (the kernel keeps the pages until the work submitted before has
// completed)
void Device::freeChunk(Chunk &ch, DeferredFrees &deferred)
{
    SWITCH_METAL_DIAGNOSTIC(stats.poolBytes -= ch.bytes);
    SWITCH_METAL_DIAGNOSTIC(stats.poolChunks--);
    deferred.add({ch.bo, ch.pitchVa, ch.pitchVa ? ch.bytes : 0, nullptr, 0, nullptr, ch.bytes, 0, RetireKind::Chunk});
    ch = Chunk();
}

Device::Trimmed Device::trim()
{
    std::vector<ParkCandidate> toPark;
    Trimmed trimmed = {};
    {
        DeferredFrees deferred(*this);
        std::lock_guard<gm20b::Mutex> l(lock_);
        retireScrubs(&deferred);
        trimmed = dropKeptImports(deferred);
        std::vector<Bo> nativeBos;
        reapNativeSurfaces(nativeBos);
        for (const Bo &bo : nativeBos)
            deferred.add({bo});
        auto freeUnusedWc = [&](std::vector<Chunk> &pool) {
            for (Chunk &ch : pool)
                if (ch.park == ChunkPark::Live && ch.bo.handle && !ch.used) {
                    deferred.add({ch.bo, 0, 0, nullptr, 0, nullptr, ch.bytes, ch.bo.mapped,
                                  ch.dedicated ? RetireKind::WcDedicated : RetireKind::WcShared});
                    ch = Chunk();
                }
        };
        if (k_.supportsBoPark() && !closing_) {
            collectParkableChunksLocked(toPark);
        } else {
            for (const Spare &sp : spares_)
                freeChunk(chunks_[sp.chunk], deferred);
            spares_.clear();
            for (Chunk &ch : chunks_)
                if (ch.park == ChunkPark::Live && ch.memory && !ch.dedicated && !ch.used) {
                    poolFree_ -= ch.bytes;
                    freeChunk(ch, deferred);
                }
            auto freeUnused = [&](std::vector<Chunk> &tiled) {
                for (Chunk &ch : tiled)
                    if (ch.park == ChunkPark::Live && ch.memory && !ch.used)
                        freeChunk(ch, deferred);
            };
            freeUnused(blChunks_);
            for (std::vector<Chunk> &z : zChunks_)
                freeUnused(z);
            freeUnusedWc(wcChunks_);
            freeUnusedWc(volatileWcChunks_);
        }
    }
    finishParkChunks(toPark);
    return trimmed;
}

// lock_ held through `l`: a surface's one import in this task. A new one asks the kernel its layout (`ask`: the first
// texture's shape and whether it writes; none: a pitch decision, as for buffers over a surface) before binding it under
// that layout's kind; one already made is shared whatever its layout. A new import's kernel calls run with lock_
// released, so the submit worker and other threads' creates do not wait for them; a thread asking for a surface another
// is importing waits for that import and shares it (or makes its own where it failed), so a surface has one import and
// one layout ask: the kernel decides the layout at the first ask and takes a second import of its pages as sharing.
// lock_ held through `l`, and let go while another thread's import of a surface runs: its importer inherits this
// thread's priority through the word's turnstile, as a held lock's owner would
void Device::waitImport(std::unique_lock<gm20b::Mutex> &l, std::shared_ptr<gm20b::OwnedWord> import)
{
    l.unlock();
    import->waitWhileOwned([&] {
        return !import->hasOwner();
    });
    l.lock();
}

Import *Device::importSurface(std::unique_lock<gm20b::Mutex> &l, uint64_t id, void *base, uint64_t bytes,
                              const void *owner, const LayoutAsk *ask)
{
    const auto key = std::make_tuple(id, uint64_t(uintptr_t(base)), bytes);
    if (id) {
        for (auto at = importing_.find(key); at != importing_.end(); at = importing_.find(key))
            waitImport(l, at->second);
        auto it = surfaces_.find(key);
        if (it != surfaces_.end()) {
            Import *kept = it->second;
            if (!kept->refs++) {
                keptImports_.erase(std::find(keptImports_.begin(), keptImports_.end(), kept));
                keptImportBytes_ -= kept->bo.bytes;
            }
            SWITCH_METAL_DIAGNOSTIC(stats.importHits++);
            return kept;
        }
        auto word = std::make_shared<gm20b::OwnedWord>();
        word->setOwner(gm20b::OwnedWord::currentOwner());
        importing_[key] = std::move(word);
    }
    l.unlock();
    Import *i = makeImport(id, base, bytes, owner, ask);
    l.lock();
    if (id) {
        const auto at = importing_.find(key);
        const std::shared_ptr<gm20b::OwnedWord> word = std::move(at->second);
        importing_.erase(at);
        word->clear();
    }
    if (!i)
        return nullptr;
    if (id)
        surfaces_[key] = i;
    SWITCH_METAL_DIAGNOSTIC(stats.imports++);
    SWITCH_METAL_DIAGNOSTIC(stats.importsMade++);
    SWITCH_METAL_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    return i;
}

// lock_ not held: a new import's kernel calls; the import is no other thread's until importSurface publishes it
Import *Device::makeImport(uint64_t id, void *base, uint64_t bytes, const void *owner, const LayoutAsk *ask)
{
    Import *i = new Import;
    if (!k_.importPages(base, bytes, i->bo, uint32_t(id))) {
        delete i;
        return nullptr;
    }
    const LayoutAsk none = {};
    const LayoutAsk &a = ask ? *ask : none;
    if (id) {
        uint32_t kind = 0;
        const uint32_t layout =
            k_.surfaceLayout(i->bo, a.width, a.height, a.bpp, a.bytesPerRow, a.writes, &i->layout, &kind);
        if (layout == TegraGM20BLayoutOtherShape) {
            k_.free(i->bo);
            delete i;
            return nullptr;
        }
        if (layout == TegraGM20BLayoutTiled)
            i->kind = kind ? kind : TGM_LAYOUT_KIND;
    }
    if (!k_.bindImport(base, i->bo, i->kind)) {
        delete i;
        return nullptr;
    }
    i->refs = 1;
    i->id = id;
    i->base = uintptr_t(base);
    i->bytes = bytes;
    if (owner)
        i->owner = CFRetain(owner);
    return i;
}

void Device::release(Import *i, DeferredFrees &deferred)
{
    if (!i || --i->refs)
        return;
    if (i->id && i->owner)
        keepImport(i, deferred);
    else
        dropImport(i, deferred);
}

void Device::keepImport(Import *i, DeferredFrees &deferred)
{
    keptImports_.push_back(i);
    keptImportBytes_ += i->bo.bytes;
    while (keptImports_.size() > kKeptImports || keptImportBytes_ > kKeptImportBytes) {
        Import *oldest = keptImports_.front();
        keptImports_.erase(keptImports_.begin());
        keptImportBytes_ -= oldest->bo.bytes;
        dropImport(oldest, deferred, true);
    }
}

Device::Trimmed Device::keptImports()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return {keptImports_.size(), keptImportBytes_};
}

Device::Trimmed Device::dropKeptImports(DeferredFrees &deferred)
{
    const Trimmed trimmed = {keptImports_.size(), keptImportBytes_};
    for (Import *i : keptImports_)
        dropImport(i, deferred, true);
    keptImports_.clear();
    keptImportBytes_ = 0;
    return trimmed;
}

void Device::dropImport(Import *i, DeferredFrees &deferred, bool wasKept)
{
    if (i->id)
        if (auto it = surfaces_.find(std::make_tuple(i->id, i->base, i->bytes));
            it != surfaces_.end() && it->second == i)
            surfaces_.erase(it);
    SWITCH_METAL_DIAGNOSTIC(stats.imports--);
    SWITCH_METAL_DIAGNOSTIC(stats.importBytes -= i->bo.bytes);
    if (i->native) {
        i->native->cached = false;
        nativeStale_.store(true, std::memory_order_release);
    }
    deferred.add({i->bo, 0, 0, nullptr, 0, i->owner, wasKept ? i->bo.bytes : 0, 0,
                  wasKept ? RetireKind::KeptImport : RetireKind::None});
    delete i;
}

// ---- block-linear memory (fx mip chains: kind 0xFE; zeta: kZetaPteKind), made on demand ----

uint64_t Device::tiledFreeBytes(const std::vector<Chunk> &chunks)
{
    uint64_t free = 0;
    for (const Chunk &ch : chunks)
        if (ch.park == ChunkPark::Live && ch.memory && !ch.dedicated)
            free += ch.bytes - ch.used;
    return free;
}

bool Device::tiledWanted(const std::vector<Chunk> &chunks) const
{
    return std::any_of(tiledWanted_.begin(), tiledWanted_.end(), [&](const TiledWant &w) {
        return w.chunks == &chunks;
    });
}

bool Device::newTiledChunk(std::vector<Chunk> &chunks, uint32_t kind, uint64_t size, bool dedicated,
                           std::unique_lock<gm20b::Mutex> &l, uint32_t &index)
{
    if (closing_)
        return false;
    Chunk ch;
    ch.dedicated = dedicated;
    ++offLockChunkOps_;
    l.unlock();
    bool made = k_.alloc(size, false, ch.bo, kind, kChunkVaAlign);
    if (made && (ch.bo.va & (kBlAlign - 1))) {
        k_.free(ch.bo);
        made = false;
    }
    if (made)
        set_.add(k_, ch.bo, TegraGM20BSurfaceWrite);
    l.lock();
    if (!made) {
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        return false;
    }
    if (closing_) {
        l.unlock();
        k_.free(ch.bo);
        l.lock();
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        return false;
    }
    if (--offLockChunkOps_ == 0)
        chunkReady_.notify_all();
    ch.memory = ch.bo.cpu;
    ch.bytes = size;
    uint32_t c = 0;
    while (c < chunks.size() && chunkSlotOccupied(chunks[c]))
        c++;
    if (c == chunks.size())
        chunks.emplace_back();
    if (!dedicated)
        ch.free[0] = size;
    chunks[c] = std::move(ch);
    SWITCH_METAL_DIAGNOSTIC(stats.poolBytes += size);
    SWITCH_METAL_DIAGNOSTIC(stats.poolChunks++);
    index = c;
    return true;
}

// A span's first fit in a shared chunk, else a chunk made now: of its own over half a chunk, else a shared one. Where the grower
// thread runs (the render server, warm()), the shared chunks grow ahead of need as the pool's do, so a pass's memoryless
// backings, made at commit, find room rather than wiring a chunk on the commit path (r115: boot 15's keyboard took 64 such
// imports of 16 MiB at ~8.5 ms each as ~10 ms encodes).
bool Device::tiledAlloc(std::unique_lock<gm20b::Mutex> &l, std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes,
                        uint64_t align, Span &s)
{
    align = std::max(align, kBlAlign);
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    s = {};
    const bool dedicated = bytes > sizes_.blChunk / 2;
    const ChunkVec vec = &chunks == &blChunks_ ? ChunkVec::Bl : ChunkVec::Zeta;
    const uint32_t zetaIndex = vec == ChunkVec::Zeta ? uint32_t(&chunks - zChunks_) : 0;
    if (dedicated) {
        for (;;) {
            if (closing_)
                return false;
            uint32_t c = ~0u;
            bool resident = false;
            if (tryUnparkChunk(l, vec, zetaIndex, bytes, true, align, TegraGM20BSurfaceWrite, c, resident)) {
                if (!resident)
                    continue;
                chunks[c].used = bytes;
                s = {c, 0, bytes, chunks[c].bo.va, static_cast<uint8_t *>(chunks[c].memory)};
                return true;
            }
            if (closing_ || !newTiledChunk(chunks, kind, bytes, true, l, c))
                return false;
            chunks[c].used = bytes;
            s = {c, 0, bytes, chunks[c].bo.va, static_cast<uint8_t *>(chunks[c].memory)};
            return true;
        }
    }
    for (int pass = 0; pass < 3;) {
        if (closing_)
            return false;
        for (uint32_t c = 0; c < chunks.size(); c++) {
            Chunk &ch = chunks[c];
            if (ch.park != ChunkPark::Live || !ch.memory || ch.dedicated)
                continue;
            for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                const uint64_t start = it->first, len = it->second, at = (start + align - 1) & ~(align - 1);
                if (at + bytes > start + len)
                    continue;
                ch.free.erase(it);
                if (at > start)
                    ch.free[start] = at - start;
                if (at + bytes < start + len)
                    ch.free[at + bytes] = start + len - at - bytes;
                ch.used += bytes;
                s = {c, at, bytes, ch.bo.va + at, static_cast<uint8_t *>(ch.memory) + at};
                const uint64_t next = growChunk(chunks, sizes_.blChunk, kBlAlign);
                if (grower_.joinable() && !tiledWanted(chunks) && tiledFreeBytes(chunks) < next &&
                    growAheadRoom(next)) {
                    tiledWanted_.push_back({&chunks, kind});  // the next chunk, off this thread
                    grow_.notify_one();
                }
                return true;
            }
        }
        uint32_t unparked = ~0u;
        bool resident = false;
        if (tryUnparkChunk(l, vec, zetaIndex, bytes, false, align, TegraGM20BSurfaceWrite, unparked, resident,
                           chunkSize(chunks, sizes_.blChunk, bytes, kBlAlign)))
            continue;
        if (closing_ || ++pass == 3)
            break;
        SWITCH_METAL_DIAGNOSTIC(stats.poolStalls++);
        clientStalled();
        if (tiledWanted(chunks)) {  // the grower is making one: wait for it rather than wire a second
            // bounded, as the pool's wait (a chunk not wired in 20 s is a stuck kernel import)
            ++offLockChunkOps_;
            const bool ready = grown_.wait_for(l, std::chrono::seconds(20), [&] {
                return !tiledWanted(chunks) || closing_;
            });
            if (--offLockChunkOps_ == 0)
                chunkReady_.notify_all();
            if (!ready) {
                os_log_fault(gm20b::log(),
                             "SwitchMetal: block-linear chunk not wired after 20 s (the grower's import): abort() for "
                             "the stack");
                abort();
            }
            continue;
        }
        uint32_t c;
        if (!newTiledChunk(chunks, kind, chunkSize(chunks, sizes_.blChunk, bytes, kBlAlign), false, l, c))
            return false;
    }
    return false;
}

void Device::tiledFree(std::vector<Chunk> &chunks, Span &s, DeferredFrees &deferred)
{
    if (!s.valid() || s.chunk >= chunks.size())
        return;
    Chunk &ch = chunks[s.chunk];
    ch.used -= s.bytes;
    if (ch.dedicated) {
        freeChunk(ch, deferred);
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back while the others keep a chunk's worth free, as the pool's do: a burst's peak is not held for
    // good (boot 15: backboardd's chunks stayed at 912 MB after its keyboard's burst)
    if (!ch.used && tiledFreeBytes(chunks) >= ch.bytes + sizes_.blChunk)
        freeChunk(ch, deferred);
}

uint64_t Device::blPitchVa(const Span &s)
{
    if (!s.valid() || s.chunk >= blChunks_.size())
        return 0;
    Chunk &ch = blChunks_[s.chunk];
    if (!ch.pitchVa) {
        const uint64_t va = k_.reserveVa(ch.bytes);
        if (!va)
            return 0;
        if (!k_.bindAlias(ch.bo, va, kPteKindPitch)) {
            k_.releaseVa(va, ch.bytes);
            return 0;
        }
        ch.pitchVa = va;
        Bo view = ch.bo;
        view.va = view.vaBase = va;
        // the chunk is a set member: the kernel takes no use of its backing, so a job names this binding by the set too
        set_.add(k_, view, TegraGM20BSurfaceWrite);
    }
    return ch.pitchVa + s.offset;
}

bool Device::createBuffer(uint64_t length, Buffer &b, bool wc, bool hostVisible, bool gpuVolatile, bool zero)
{
    b = {};
    if (!length)
        length = 16;
    bool cleaned = hostVisible;
    bool dirty = false;
    {
        std::unique_lock<gm20b::Mutex> l(lock_);
        if (wc && gpuVolatile && volatileWcAlloc(l, length, b.span, &dirty)) {
            b.wc = true;
            b.gpuVolatile = true;
            // VOL=1 WC chunks carry no dirty GPU L2 lines and no CPU D-cache lines; skipping hostAccess keeps the chunk
            // BO in ResourceSet and avoids host1x-bridge flushes and host_epoch whole-L2 invalidates.
            cleaned = false;
            SWITCH_METAL_DIAGNOSTIC(stats.wcBytes += b.span.bytes);
        } else if (wc && wcAlloc(l, length, b.span, &dirty)) {
            b.wc = true;
            // write-combined pages hold no CPU line to clean, and a reused span's last writer ended External: its GPU
            // lines are in DRAM. A lease would sleep this thread on its chunk's GPU work and take the chunk out of the set
            cleaned = false;
            SWITCH_METAL_DIAGNOSTIC(stats.wcBytes += b.span.bytes);
        } else {
            if (wc)
                SWITCH_METAL_DIAGNOSTIC(stats.wcFallbacks++);
            if (!poolAlloc(l, length, b.span, &dirty))
                return false;
            Chunk &chunk = chunks_[b.span.chunk];
            cleaned = hostVisible || chunk.hostTouched;
            chunk.hostTouched |= hostVisible;
        }
    }
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = length;
    b.hostVisible = hostVisible;
    // A reused span may still have dirty GPU lines from its previous owner. Acquire before exposing CPU bytes. A buffer no
    // CPU reads or writes exposes none: it takes the lease (whose release cleans the CPU's lines) only where a host-visible
    // buffer was in its chunk before it. A lease takes the whole chunk out of the resource set for good
    // (SwitchGpuResourceSet.cpp cpu_reached), every resource in it a job use of its own from then on
    if (!cleaned) {
        if (zero && dirty && b.wc && b.cpu) {
            memset(b.cpu, 0, length);
#if defined(__aarch64__)
            __asm__ volatile("dsb st" ::: "memory");
#endif
        }
        b.freshFill = zero && dirty && !b.wc;
        return true;
    }
    HostAccess access;
    if (!hostAccess(b, true, access)) {
        destroyBuffer(b);
        return false;
    }
    if (zero && dirty && b.cpu)
        memset(b.cpu, 0, length);
    if (!access.finish()) {
        destroyBuffer(b);
        return false;
    }
    return true;
}

bool Device::createControlBuffer(uint64_t length, Buffer &buffer)
{
    buffer = {};
    if (!length)
        length = 16;
    std::lock_guard<gm20b::Mutex> lock(lock_);
    Chunk chunk;
    if (!k_.allocControl(length, chunk.bo, ControlUse::Bytes))
        return false;
    set_.add(k_, chunk.bo, TegraGM20BSurfaceRead);
    chunk.memory = chunk.bo.cpu;
    chunk.bytes = chunk.used = chunk.bo.bytes;
    chunk.dedicated = true;
    std::vector<Chunk> &pool = chunk.bo.gpuVolatile ? volatileWcChunks_ : wcChunks_;
    uint32_t index = 0;
    while (index < pool.size() && chunkSlotOccupied(pool[index]))
        ++index;
    if (index == pool.size())
        pool.emplace_back();
    buffer.span = {index, 0, chunk.bytes, chunk.bo.va, chunk.bo.cpu};
    buffer.va = chunk.bo.va;
    buffer.cpu = chunk.bo.cpu;
    buffer.length = length;
    buffer.wc = true;
    buffer.gpuVolatile = chunk.bo.gpuVolatile;
    buffer.hostVisible = false;
    SWITCH_METAL_DIAGNOSTIC(stats.wcBytes += chunk.bytes);
    pool[index] = std::move(chunk);
    return true;
}

// ---- write-combined buffers: the GPU pool (BoAlloc coherent), mapped write-combined for the CPU ----

// lock_ held through `l`. First fit by page; a new chunk when none has room (a BoAlloc: tens of microseconds, the GPU pool's own)
bool Device::wcPoolAlloc(std::unique_lock<gm20b::Mutex> &l, std::vector<Chunk> &pool, uint64_t bytes, bool gpuVolatile,
                         Span &s, bool *dirty)
{
    bytes = (bytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    s = {};
    if (dirty)
        *dirty = false;
    const bool dedicated = bytes > sizes_.wcChunk / 2;
    // The CPU never leases a write-combined chunk (createBuffer, Device::hostAccess, invalidateForCpu), the only reason a
    // member starts read: written from the start, the first GPU write to one of its buffers does not make the set again
    constexpr uint32_t memberAccess = TegraGM20BSurfaceWrite;
    for (int pass = 0; pass < 2;) {
        if (closing_)
            return false;
        if (!dedicated) {
            for (uint32_t c = 0; c < pool.size(); c++) {
                Chunk &ch = pool[c];
                if (ch.park != ChunkPark::Live || !ch.bo.handle || ch.dedicated)
                    continue;
                for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                    if (it->second < bytes)
                        continue;
                    const uint64_t at = it->first, left = it->second - bytes;
                    ch.free.erase(it);
                    if (left)
                        ch.free[at + bytes] = left;
                    if (dirty)
                        *dirty = at < ch.highWater && !gpuVolatile;
                    ch.highWater = std::max(ch.highWater, at + bytes);
                    ch.used += bytes;
                    s = {c, at, bytes, ch.bo.va + at, ch.bo.cpu + at};
                    return true;
                }
            }
            if (gpuVolatile && !scrubbing_.empty()) {
                const size_t before = scrubbing_.size();
                retireScrubs(nullptr);
                if (scrubbing_.size() != before)
                    continue;
            }
            uint32_t unparked = ~0u;
            bool resident = false;
            if (tryUnparkChunk(l, gpuVolatile ? ChunkVec::VolatileWc : ChunkVec::Wc, 0, bytes, false, 0, memberAccess,
                               unparked, resident, chunkSize(pool, sizes_.wcChunk, bytes, pageSize())))
                continue;
        }
        if (closing_ || pass++)
            break;
        const uint64_t size = dedicated ? bytes : chunkSize(pool, sizes_.wcChunk, bytes, pageSize());
        Chunk ch;
        ++offLockChunkOps_;
        l.unlock();
        const bool made = k_.alloc(size, true, ch.bo, 0, 0x10000, gpuVolatile);
        if (made)
            set_.add(k_, ch.bo, memberAccess);
        l.lock();
        if (!made) {
            if (--offLockChunkOps_ == 0)
                chunkReady_.notify_all();
            return false;  // the GPU pool is full: pool memory instead (createBuffer)
        }
        if (closing_) {
            l.unlock();
            k_.free(ch.bo);
            l.lock();
            if (--offLockChunkOps_ == 0)
                chunkReady_.notify_all();
            return false;
        }
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        ch.memory = ch.bo.cpu;
        ch.bytes = size;
        ch.dedicated = dedicated;
        uint32_t c = 0;
        while (c < pool.size() && chunkSlotOccupied(pool[c]))
            c++;
        if (c == pool.size())
            pool.emplace_back();
        if (dedicated) {
            ch.used = bytes;
            ch.highWater = bytes;
            s = {c, 0, bytes, ch.bo.va, ch.bo.cpu};
            pool[c] = std::move(ch);
            return true;
        }
        ch.free[0] = size;
        pool[c] = std::move(ch);
    }
    return false;
}

// lock_ held
void Device::wcPoolFree(std::vector<Chunk> &pool, Span &s, DeferredFrees &deferred)
{
    if (!s.valid() || s.chunk >= pool.size())
        return;
    Chunk &ch = pool[s.chunk];
    SWITCH_METAL_DIAGNOSTIC(stats.wcBytes -= s.bytes);
    // A Shared span is zeroed before it is free again (scrubs.cpp), unless its free empties the chunk and the chunk goes
    // back to the kernel below, whose next BoAlloc of the pages comes zeroed
    if (&pool == &volatileWcChunks_ && !ch.dedicated && (ch.used > s.bytes || !otherEmptyChunk(pool, ch))) {
        scrubWanted_.push_back({s.chunk, s.offset, s.bytes, s.va});
        scrubDue_.store(true, std::memory_order_relaxed);
        s = {};
        return;
    }
    ch.used -= s.bytes;
    if (ch.dedicated) {
        deferred.add({ch.bo, 0, 0, nullptr, 0, nullptr, ch.bytes, ch.bo.mapped, RetireKind::WcDedicated});
        ch = Chunk();
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back to the GPU pool while another empty one stays for the next burst: the compositor's pool
    // share follows what Core Animation holds, not its peak (r8a: the compositor 107 MiB of the pool at 15:16, 63 after,
    // and Maps refused a 4 MiB BO with 33 MiB free)
    if (!ch.used && otherEmptyChunk(pool, ch)) {
        deferred.add({ch.bo, 0, 0, nullptr, 0, nullptr, ch.bytes, ch.bo.mapped, RetireKind::WcShared});
        ch = Chunk();
    }
}

bool Device::otherEmptyChunk(const std::vector<Chunk> &pool, const Chunk &ch)
{
    return std::any_of(pool.begin(), pool.end(), [&](const Chunk &other) {
        return &other != &ch && other.park == ChunkPark::Live && other.bo.handle && !other.dedicated && !other.used;
    });
}

bool Device::wcAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty)
{
    return wcPoolAlloc(l, wcChunks_, bytes, false, s, dirty);
}

void Device::wcFree(Span &s, DeferredFrees &deferred)
{
    wcPoolFree(wcChunks_, s, deferred);
}

bool Device::volatileWcAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty)
{
    if (!k_.volatileWrites() || !k_.exactJobs())
        return false;
    return wcPoolAlloc(l, volatileWcChunks_, bytes, true, s, dirty);
}

void Device::volatileWcFree(Span &s, DeferredFrees &deferred)
{
    wcPoolFree(volatileWcChunks_, s, deferred);
}

uint32_t Device::wcChunksHeld()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    uint32_t n = 0;
    for (const Chunk &ch : wcChunks_)
        n += ch.park == ChunkPark::Live && ch.bo.handle && !ch.dedicated;
    for (const Chunk &ch : volatileWcChunks_)
        n += ch.park == ChunkPark::Live && ch.bo.handle && !ch.dedicated;
    return n;
}

uint64_t Device::heapInPlaceBytes()
{
    ensurePackWindow();
    std::lock_guard<gm20b::Mutex> l(lock_);
    uint64_t bytes = packPages_.boundBytes();
    for (const StoreRegion &r : storeRegions_)
        bytes += r.pages.boundBytes();
    return bytes;
}

// The kernel's count of the control memory mapped, which the threads that make it keep (the submit worker grows the rings
// under the channel's lock, not lock_), less the write-combined chunks, which are control memory named apart
uint64_t Device::controlMappedBytes()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    uint64_t wc = retiring_.wcMapped.load(std::memory_order_acquire);
    for (const Chunk &ch : wcChunks_)
        if (ch.park == ChunkPark::Live)
            wc += ch.bo.mapped;
    for (const Chunk &ch : volatileWcChunks_)
        if (ch.park == ChunkPark::Live)
            wc += ch.bo.mapped;
    const uint64_t control = k_.controlMappedBytes();
    return control > wc ? control - wc : 0;
}

bool Device::held(Held &h, bool wait)
{
    std::unique_lock<gm20b::Mutex> l(lock_, std::defer_lock);
    if (wait)
        l.lock();
    else if (!l.try_lock())
        return false;
    h = {};
    const auto count = [&](const Chunk &ch) {
#if SWITCH_METAL_DIAGNOSTICS
        if (ch.park == ChunkPark::Parked) {
            h.parkedChunks += ch.bytes;
            h.parkedChunkCount++;
            return;
        }
#endif
        if (ch.park != ChunkPark::Live || !ch.memory)
            return;
        h.chunks += ch.bytes;
        h.chunkCount++;
        if (!ch.dedicated && ch.used && ch.used * 4 <= ch.bytes) {
            h.pinned += ch.bytes;
            h.pinnedCount++;
        }
    };
    for (const Chunk &ch : chunks_)
        count(ch);
    for (const Chunk &ch : blChunks_)
        count(ch);
    for (const std::vector<Chunk> &z : zChunks_)
        for (const Chunk &ch : z)
            count(ch);
    h.chunks += retiring_.chunkBytes.load(std::memory_order_relaxed);
    h.chunkCount += retiring_.chunkCount.load(std::memory_order_relaxed);
    for (const Spare &sp : spares_)
        h.reuse += chunks_[sp.chunk].bytes;
    uint64_t wcMapped = retiring_.wcMapped.load(std::memory_order_relaxed);
    h.writeCombined = retiring_.wcBytes.load(std::memory_order_relaxed);
    const auto countWc = [&](const Chunk &ch) {
#if SWITCH_METAL_DIAGNOSTICS
        if (ch.park == ChunkPark::Parked) {
            h.parkedChunks += ch.bytes;
            h.parkedChunkCount++;
            return;
        }
#endif
        if (ch.park != ChunkPark::Live)
            return;
        h.writeCombined += ch.bo.handle ? ch.bytes : 0;
        wcMapped += ch.bo.mapped;
    };
    for (const Chunk &ch : wcChunks_)
        countWc(ch);
    for (const Chunk &ch : volatileWcChunks_)
        countWc(ch);
    for (const Arena &a : arenas_) {
        if (a.used && !a.purged) {
            (a.wc ? h.heaps : h.gpuHeaps) += a.bytes;
            h.volatileHeaps += a.isVolatile ? a.bytes : 0;
        }
#if SWITCH_METAL_DIAGNOSTICS
        else if (a.used && a.purged && a.boParked) {
            h.parkedHeaps += a.bytes;
            h.parkedHeapCount++;
        }
#endif
    }
    h.heaps += retiring_.heapBytes.load(std::memory_order_relaxed);
    h.gpuHeaps += retiring_.gpuHeapBytes.load(std::memory_order_relaxed);
    h.kept = keptImportBytes_ + retiring_.keptBytes.load(std::memory_order_relaxed);
    const uint64_t control = k_.controlMappedBytes();
    h.tables = control > wcMapped ? control - wcMapped : 0;
    return true;
}

uint64_t Device::wcChunkBytesHeld()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    uint64_t n = retiring_.wcSharedBytes.load(std::memory_order_relaxed);
    for (const Chunk &ch : wcChunks_)
        n += ch.park == ChunkPark::Live && ch.bo.handle && !ch.dedicated ? ch.bytes : 0;
    for (const Chunk &ch : volatileWcChunks_)
        n += ch.park == ChunkPark::Live && ch.bo.handle && !ch.dedicated ? ch.bytes : 0;
    return n;
}

uint32_t Device::surfaceImporter(uint64_t id, void *base, uint64_t bytes)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    const auto at = importing_.find(std::make_tuple(id, uint64_t(uintptr_t(base)), bytes));
    return at == importing_.end() ? 0 : at->second->owner();
}

uint64_t Device::blChunkFreeBytes()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return tiledFreeBytes(blChunks_);
}

// lock_ held. A chunk is a cached BO, whose pages the kernel hands to any process's next BoAlloc once it is freed, though
// an import of them that begins past its first page still holds them: a backing is found by its first page
// (SwitchGpuBacking.cpp). So no import may take a range that reaches into one; `what` names the call refused.
bool Device::ownChunkMemory(const void *bytes, uint64_t length, const char *what) const
{
    const auto overlapsRange = [&](uint64_t base, uint64_t size) {
        return base && size && uintptr_t(bytes) < base + size && base < uintptr_t(bytes) + length;
    };
    const auto overlaps = [&](const std::vector<Chunk> &chunks) {
        return std::any_of(chunks.begin(), chunks.end(), [&](const Chunk &ch) {
            uint64_t base = ch.parkedVa;
            if (ch.memory)
                base = uint64_t(uintptr_t(ch.memory));
            else if (ch.bo.cpu)
                base = uint64_t(uintptr_t(ch.bo.cpu));
            return overlapsRange(base, ch.bytes);
        });
    };
    const bool inArena = std::any_of(arenas_.begin(), arenas_.end(), [&](const Arena &a) {
        return a.used && a.kernelBo && (!a.purged || a.boParked) && overlapsRange(uint64_t(uintptr_t(a.cpu)), a.bytes);
    });
    if (!overlaps(chunks_) && !overlaps(blChunks_) && !overlaps(wcChunks_) && !overlaps(volatileWcChunks_) &&
        std::none_of(std::begin(zChunks_), std::end(zChunks_), overlaps) && !inArena)
        return false;
    os_log_error(gm20b::log(),
                 "SwitchMetal: %{public}s: %{public}s over %llu bytes at %p: they lie in this process's own GPU memory "
                 "(a Metal buffer's contents), which no import may take",
                 getprogname(), what, (unsigned long long)length, bytes);
    return true;
}

// The caller's pages around [bytes, bytes + length): the frontend answers minBufferNoCopyAlignmentBytes with the kernel's 4 KiB
// page, not getpagesize()'s 16 KiB, and Apple's own callers (iconservicesagent on 27) pass pointers inside a page; the import
// binds whole kernel pages and the buffer's address is its first byte's inside them
bool Device::createBufferNoCopy(void *bytes, uint64_t length, Buffer &b, bool &tiled)
{
    b = {};
    tiled = false;
    if (!bytes || !length)
        return false;
    std::unique_lock<gm20b::Mutex> l(lock_);
    // a surface another thread is importing has its layout once that import is published: wait for one over these pages
    const auto importingOver = [&] {
        return std::find_if(importing_.begin(), importing_.end(), [&](const auto &at) {
            return surfaceBackingAliases(0, std::get<1>(at.first), std::get<2>(at.first), 0, uintptr_t(bytes), length);
        });
    };
    for (auto at = importingOver(); at != importing_.end(); at = importingOver())
        waitImport(l, at->second);
    for (const auto &[key, i] : surfaces_)
        if (i->kernelTiled() && surfaceBackingAliases(0, i->base, i->bytes, 0, uintptr_t(bytes), length)) {
            tiled = true;
            return false;
        }
    if (ownChunkMemory(bytes, length, "newBufferWithBytesNoCopy"))
        return false;
    if (!(b.import = importSurface(l, 0, bytes, length, nullptr)))
        return false;
    b.va = b.import->bo.va;
    b.cpu = static_cast<uint8_t *>(bytes);
    b.length = length;
    return true;
}

bool Device::createSurfaceBuffer(uint64_t id, void *base, uint64_t length, const void *owner, Buffer &b, bool &tiled)
{
    b = {};
    tiled = false;
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    Import *i = importSurface(l, id, base, length, owner);
    if (!i)
        return false;
    if (i->kernelTiled()) {
        tiled = true;
        release(i, deferred);
        return false;
    }
    b.import = i;
    b.va = i->bo.va;
    b.cpu = static_cast<uint8_t *>(base);
    b.length = length;
    return true;
}

void Device::destroyBuffer(Buffer &b)
{
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (b.import)
        release(b.import, deferred);
    freeSpan(l, b.span, memoryOf(b), b.lease, deferred);
    SWITCH_METAL_DIAGNOSTIC(stats.textureBytes -= b.keptBytes);
    b = {};
}

}  // namespace gm20b
