// SPDX-License-Identifier: MIT
// See gpu.h (Device::backing: the memory a pass's memoryless, shadowed and stand-in attachments render into, and its
// trimming).
#include "lock.h"
#include "fx/fx_image.h"
#include "attachment_lifetime.h"
#include "gpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// A memoryless attachment's memory: one per size, format, tag and layout, kept while passes use it (m2v 0105 gives a memoryless
// attachment linear memory in a pass that renders to linear memory; NVK renders a pass with a block-linear colour target or a
// depth/stencil attachment into block-linear colour). Called with the device lock held.
Device::PlainMem *Device::largestPlain(uint32_t tag, uint32_t memory, bool layered) const
{
    PlainMem *best = nullptr;
    for (const auto &m : plainMems_)
        if (m->holds(tag, memory, layered) && (!best || m->span.bytes > best->span.bytes))
            best = m.get();
    return best;
}

Device::Backing *Device::plainBacking(const Texture &m, uint32_t tag, bool blockLinear)
{
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0, align = kBlAlign;
    uint32_t memory = kPlainPool;
    const FormatInfo *f = formatInfo(m.format);
    if (m.zeta) {
        memory = kPlainZeta + zetaFormatIndex(m.format);
        uint64_t zetaAlign = 0;
        if (!zetaShape(m.width, m.height, m.format, m.samples, t, bytes, zetaAlign))
            return nullptr;
        align = std::max(align, zetaAlign);
    } else if (blockLinear) {
        memory = kPlainBl;
        if (!f || !shape(m.width, m.height, m.format, t) || !fx::msLayout(m.width, m.height, m.samples, t.bpp, c))
            return nullptr;
        bytes = c.bytes;
        align = std::max<uint64_t>(align, c.align);
        t.pitch = c.level[0].rowBytes;
        t.blockLinear = true;
        t.blockHeight = c.level[0].yLog2;
        t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
        t.samples = m.samples;
    } else if (m.layers > 1) {
        return nullptr;
    } else {
        if (!f || !shape(m.width, m.height, m.format, t))
            return nullptr;
        t.pitch = (m.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
    }
    // a layered pass's: its layers one layer's whole tiles apart (NIL's array stride of a one-level image)
    t.layers = std::max(m.layers, 1u);
    const bool layered = t.layers > 1;
    if (layered)
        bytes = (t.layerBytes * t.layers + align - 1) / align * align;
    PlainMem *mem = largestPlain(tag, memory, layered);
    if (!mem || mem->span.bytes < bytes || (mem->span.va & (align - 1))) {
        auto made = std::make_unique<PlainMem>();
        made->tag = tag;
        made->memory = memory;
        made->layered = layered;
        made->align = align;
        const uint64_t want = sizeClassBytes(bytes, kBlAlign);
        const bool ok = memory == kPlainPool ? poolAlloc(want, made->span)
                        : memory == kPlainBl
                            ? blAlloc(want, align, made->span)
                            : tiledAlloc(zChunks_[memory - kPlainZeta], kZetaFormatInfo[memory - kPlainZeta].kind, want,
                                         align, made->span);
        if (!ok || (made->span.va & (align - 1))) {
            if (ok)
                freePlainSpan(*made);
            return nullptr;
        }
        SWITCH_METAL_DIAGNOSTIC(stats.backingBytes += made->span.bytes);
        SWITCH_METAL_DIAGNOSTIC(glassMemEvent("memoryless plain memory made", made->span.bytes));
        plainMems_.push_back(std::move(made));
        mem = plainMems_.back().get();
        // the views of its tag's smaller memory: no pass takes them again (an animated layer seldom asks a size twice), so it
        // goes back once their last uses complete rather than 10 s on
        for (Backing *b : backings_)
            if (b->plain && b->plain != mem && b->plain->holds(tag, memory, layered))
                b->retired = true;
        backingsChanged();
        compRearm_ = true;  // an idle process gives it back when it turns stale
        compStart();
    }
    t.va = mem->span.va;
    t.storage = {t.va, bytes, false};
    t.cpu = memory == kPlainPool ? mem->span.cpu : nullptr;
    if (m.zeta) {  // a depth resolve's or a turn copy's source samples it
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        writeTic(t);
    }
    if (!m.zeta) {
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        if (blockLinear) {
            uint32_t w[8];
            fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, 1, w);
            memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
        } else {
            t.renderable = t.renderable && linearTarget(t);
            writeTic(t);
        }
    }
    auto *b = new Backing;
    b->texture = t;
    b->tag = tag;
    b->plain = mem;
    b->usedNs = compClock();
    mem->views++;
    SWITCH_METAL_DIAGNOSTIC(stats.backings++);
    return b;
}

void Device::freePlainSpan(PlainMem &mem)
{
    if (mem.memory == kPlainPool)
        poolFree(mem.span);
    else if (mem.memory == kPlainBl)
        blFree(mem.span);
    else
        tiledFree(zChunks_[mem.memory - kPlainZeta], mem.span);
}

void Device::dropPlain(PlainMem *mem)
{
    SWITCH_METAL_DIAGNOSTIC(stats.backingBytes -= mem->span.bytes);
    SWITCH_METAL_DIAGNOSTIC(glassMemEvent("memoryless plain memory freed", mem->span.bytes));
    freePlainSpan(*mem);
    plainMems_.erase(std::find_if(plainMems_.begin(), plainMems_.end(), [&](const std::unique_ptr<PlainMem> &p) {
        return p.get() == mem;
    }));
}

void Device::plainFree(Backing &b)
{
    if (b.texture.tic)
        freeTic(b.texture.tic);
    SWITCH_METAL_DIAGNOSTIC(stats.backings--);
    PlainMem *mem = b.plain;
    b.plain = nullptr;
    if (!--mem->views)
        dropPlain(mem);
}

Texture *Device::lastBacking(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    const uint64_t epoch = backingEpoch_.load(std::memory_order_seq_cst);
    for (const BackingAsk &ask : asks_) {
        if (ask.epoch != epoch || !ask.same(m, tag, blockLinear, compression, copied, kindProbes()))
            continue;
        Backing *b = ask.backing;
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_seq_cst);
        // a trim moves the epoch before it reads the stamps: one that began since reads ours or is seen here
        return backingEpoch_.load(std::memory_order_seq_cst) == epoch ? &b->texture : nullptr;
    }
    return nullptr;
}

Texture *Device::backing(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    BackingAsk ask{m.width,     m.height,    m.format, tag,         std::max(m.layers, 1u), m.samples, m.zeta,
                   blockLinear, compression, copied,   kindProbes()};
    blockLinear |= m.zeta;
    // a layered pass's backing: plain memory of its layers (compressed backings are one layer's)
    const uint32_t kind = compression && m.layers <= 1 ? compKind(m, blockLinear, copied) : 0;
    const CompMem *mem = kind ? comp_.largest(tag, kind) : nullptr;
    Backing *plain = nullptr;
    blockLinear |= m.samples > 1;
    const auto asked = [&](Backing *b) {
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_relaxed);
        if (tag < lastBackings_.size())
            lastBackings_[tag] = b;
        ask.backing = b;
        ask.epoch = backingEpoch_.load(std::memory_order_relaxed);
        asks_[nextAsk_++ % asks_.size()] = ask;
        return &b->texture;
    };
    const auto matches = [&](const Backing *b) {
        return b && !b->retired && b->texture.width == m.width && b->texture.height == m.height &&
               b->texture.format == m.format && b->tag == tag && b->texture.blockLinear == blockLinear &&
               b->texture.samples == m.samples && b->texture.layers == std::max(m.layers, 1u);
    };
    if (Backing *last = tag < lastBackings_.size() ? lastBackings_[tag] : nullptr;
        matches(last) && backingCompressionMatches(last->comp != nullptr, last->comp, mem, kind != 0))
        return asked(last);
    for (Backing *b : backings_)
        if (matches(b)) {
            if (b->plain && b->plain != largestPlain(tag, b->plain->memory, b->plain->layered)) {
                b->retired = true;  // larger plain memory of its tag took its place: a view of that replaces it
                backingsChanged();
                continue;
            }
            if (backingCompressionMatches(b->comp != nullptr, b->comp, mem, kind != 0))
                return asked(b);
            if (b->comp && kind) {
                b->retired = true;  // larger memory of its tag and kind took its place: a view of that replaces it
                backingsChanged();
            } else if (!b->comp)
                plain = b;
        }
    if (Backing *c = kind ? compBacking(m, tag, kind) : nullptr) {
        backings_.push_back(c);
        return asked(c);
    }
    if (plain)
        return asked(plain);
    Backing *view = plainBacking(m, tag, blockLinear);
    if (!view)
        return nullptr;
    backings_.push_back(view);
    return asked(view);
}

// Backings no pass used for 600 command buffers (~10 s of frames) and whose last use completed go back to the pool; a retired
// one (a compressed backing, or one on larger memory of its tag, replaced it) as soon as its last use completed, as no pass
// takes it again; one on compressed or shared plain memory after 10 s unused too, also while the process commits nothing
// (comp.h backingStale; the compressed-memory thread trims then).
void Device::trimBackings()
{
    // the counts before the backings' stamps: a commit stores its stamps before it moves them
    const uint64_t committed = commits(), closedNow = closedCount_.load(std::memory_order_acquire);
    const uint64_t now = compClock();
    const auto stale = [&](const Backing *b) {
        return backingStale(committed, b->lastCommit.load(std::memory_order_seq_cst), closedNow,
                            b->lastClosed.load(std::memory_order_seq_cst), now,
                            b->usedNs.load(std::memory_order_relaxed), b->retired, b->comp || b->plain) &&
               (!b->submitted.load(std::memory_order_relaxed) || done(b->lastUse.load(std::memory_order_relaxed)));
    };
    const size_t ready = comp_.ready.size();
    compTrim(now);
    for (auto it = spares_.begin(); it != spares_.end();) {
        if (committed - it->freedAt > 600) {
            freeChunk(chunks_[it->chunk]);
            it = spares_.erase(it);
            continue;
        }
        ++it;
    }
    // A pass's stamps (lastBacking, with no lock) only make a backing less stale: where none is stale before the epoch
    // moves none is after, and the passes' kept asks stay good
    if (comp_.ready.size() != ready || std::any_of(backings_.begin(), backings_.end(), stale)) {
        backingsChanged();
        for (auto it = backings_.begin(); it != backings_.end();) {
            Backing *b = *it;
            if (!stale(b)) {
                ++it;
                continue;
            }
            if (b->tag < lastBackings_.size() && lastBackings_[b->tag] == b)
                lastBackings_[b->tag] = nullptr;
            if (b->comp) {
                compFree(*b);
            } else if (b->plain) {
                plainFree(*b);
            }
            trimmedBackings_.push_back(b);
            trimmedPending_.store(true, std::memory_order_release);
            it = backings_.erase(it);
        }
    }
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t retired = 0;
    for (const Backing *b : backings_)
        retired += !b->comp && b->retired;
    stats.retiredBackings = retired;
    uint64_t tiledFree = tiledFreeBytes(blChunks_);
    for (const std::vector<Chunk> &z : zChunks_)
        tiledFree += tiledFreeBytes(z);
    stats.tiledFreeBytes = tiledFree;
#endif
}

}  // namespace gm20b
