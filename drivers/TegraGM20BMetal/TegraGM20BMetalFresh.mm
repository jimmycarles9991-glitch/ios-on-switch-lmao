// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalFresh.h"
#include "decoded.h"
#include "fresh.h"
#include "fresh_fill.h"
#include "fresh_queue.h"

namespace {

// One of the two
struct FreshEntry {
    __weak SwitchMetalTexture *texture;
    __weak SwitchMetalBuffer *buffer;
};

gm20b::FreshQueue<FreshEntry> gFresh;

#if SWITCH_METAL_DIAGNOSTICS
std::atomic<uint64_t> gFilled{0}, gFilledBytes{0}, gCleared{0};
std::mutex gTraceLock;
NSMapTable<SwitchMetalTexture *, NSNumber *> *gUnwritten;
std::atomic<uint64_t> gEarlyReads{0};
#endif

SwitchMetalTexture *ownerOf(SwitchMetalTexture *t)
{
    return t ? (t->_levelsOwner ?: t) : nil;
}

uint64_t writeFresh(gm20b::BlitEncoder &b, SwitchMetalTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const gm20b::Texture &x = t->_raw;
    const uint32_t levels = x.chain ? x.chain->layout.levels : 1;
    const uint32_t patterned = gm20b::patternLevels(uint64_t(d.usage), d.allowGPUOptimizedContents, uint32_t(d.width),
                                                    uint32_t(d.height), levels);
    const uint8_t zeros[16] = {};
    gm20b::CopyFill zero, pattern;
    gm20b::FreshTexel texel;
    // a decoded format's texture reads its blocks of zeros' decode, as a new one of Apple's does (decoded.h)
    if (const gm20b::DecodedFormat *f = gm20b::decodedFormat(uint32_t(d.pixelFormat))) {
        if (!gm20b::copyFillOf(reinterpret_cast<const uint8_t *>(f->fresh), x.bpp, zero))
            return 0;
        return gm20b::fillFresh(b, x, 0, zero, zero);
    }
    if (!gm20b::copyFillOf(zeros, x.bpp, zero))
        return 0;
    const bool hasPattern = patterned && gm20b::freshPatternTexel(x.format, texel) && texel.size == x.bpp &&
                            gm20b::copyFillOf(texel.bytes, texel.size, pattern);
    return gm20b::fillFresh(b, x, hasPattern ? patterned : 0, pattern, zero);
}

enum class FirstUse {
    None,
    ClearsAll,
    Other
};

// The first encoder of [from, to) that names `t` (or a view of it): a pass that clears and stores all of it before
// anything reads it (fresh.h firstPassWritesAll), or any other use
FirstUse firstUse(SwitchMetalCommandBuffer *cb, size_t from, size_t to, SwitchMetalTexture *t)
{
    for (size_t at = from; at < to;) {
        const auto op = cb->_plan.operation(at).op;
        if (op != OpRender && op != OpCompute && op != OpBlit) {
            at++;
            continue;
        }
        const size_t end = cb->_plan.encoderEnd(at);
        uint8_t access = 0;
        bool named = false;
        cb->_plan.forEachResource(at, end, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
            if (ownerOf((__bridge SwitchMetalTexture *)use.object) == t) {
                named = true;
                access |= use.access;
            }
        });
        if (!named) {
            at = std::max(end, at + 1);
            continue;
        }
        if (op != OpRender)
            return FirstUse::Other;
        const SwitchMetalPass &pass = cb->_passes[cb->_plan.legacy(at).x];
        const gm20b::Texture &x = t->_raw;
        uint32_t smallestWidth = UINT32_MAX, smallestHeight = UINT32_MAX;
        const auto attachment = [&](__unsafe_unretained id object, NSUInteger level) {
            if (__unsafe_unretained SwitchMetalTexture *a = (SwitchMetalTexture *)object) {
                smallestWidth = std::min(smallestWidth, std::max(a->_raw.width >> level, 1u));
                smallestHeight = std::min(smallestHeight, std::max(a->_raw.height >> level, 1u));
            }
        };
        for (const SwitchMetalPass::Color &c : pass.color)
            attachment(c.texture, c.level);
        attachment(pass.depth, 0);
        const uint32_t areaWidth = gm20b::renderAreaSide(uint32_t(pass.width), smallestWidth),
                       areaHeight = gm20b::renderAreaSide(uint32_t(pass.height), smallestHeight);
        for (const SwitchMetalPass::Color &c : pass.color)
            if (c.texture == t && c.level == 0 &&
                gm20b::firstPassWritesAll(
                    x.width, x.height, x.chain ? x.chain->layout.levels : 1,
                    x.type == gm20b::fx::kTexThreeD ? x.depth : x.layers, c.load == MTLLoadActionClear,
                    c.store == MTLStoreActionStore || c.store == MTLStoreActionStoreAndMultisampleResolve,
                    access & gm20b::ResourceUse::Sample, areaWidth, areaHeight))
                return FirstUse::ClearsAll;
        return FirstUse::Other;
    }
    return FirstUse::None;
}

// Whether an encoder of [from, to) names buffer `b`: its first use, which the fill goes ahead of whatever it does
bool namesBuffer(SwitchMetalCommandBuffer *cb, size_t from, size_t to, SwitchMetalBuffer *b)
{
    bool named = false;
    cb->_plan.forEachResource(from, to, SwitchMetalResourceKind::Buffer, [&](const SwitchMetalRangeResource &use) {
        named |= (__bridge SwitchMetalBuffer *)use.object == b;
    });
    return named;
}

uint64_t writeFreshBuffer(gm20b::BlitEncoder &blit, SwitchMetalBuffer *b)
{
    blit.fillBytes(b->_raw.va, b->_raw.length, 0);
    return b->_raw.length;
}

// An entry's first use in [from, to) (all of a whole range when `to` is not past `from`): a buffer's is any use
FirstUse entryUse(SwitchMetalCommandBuffer *cb, size_t from, size_t to, const FreshEntry &e)
{
    if (SwitchMetalBuffer *b = e.buffer)
        return to <= from || namesBuffer(cb, from, to, b) ? FirstUse::Other : FirstUse::None;
    SwitchMetalTexture *t = e.texture;
    return to > from ? firstUse(cb, from, to, t) : FirstUse::Other;
}

uint64_t writeEntry(gm20b::BlitEncoder &blit, const FreshEntry &e)
{
    if (SwitchMetalBuffer *b = e.buffer)
        return writeFreshBuffer(blit, b);
    return writeFresh(blit, e.texture);
}

bool live(const FreshEntry &e)
{
    if (e.buffer)
        return true;
    SwitchMetalTexture *t = e.texture;
    return t && !t->_heap;
}

id objectOf(const FreshEntry &e)
{
    return e.buffer ? (id)e.buffer : (id)e.texture;
}

FreshEntry entryOf(id object)
{
    if ([object isKindOfClass:[SwitchMetalBuffer class]])
        return {nil, (SwitchMetalBuffer *)object};
    return {(SwitchMetalTexture *)object, nil};
}

}  // namespace

void establishFreshBuffer(SwitchMetalBuffer *b)
{
    gFresh.add({nil, b});
}

bool freshFills(SwitchMetalTexture *t, MTLTextureDescriptor *d)
{
    return t->_gm20bOk && t->_kind == SwitchMetalTexturePool && !t->_heap && !t->_raw.memoryless && !t->_raw.arena &&
           !t->_raw.zeta && (d.storageMode == MTLStorageModePrivate || gm20b::decodedFormat(uint32_t(d.pixelFormat)));
}

void establishFresh(SwitchMetalTexture *t, MTLTextureDescriptor *d)
{
    if (!freshFills(t, d))
        return;
    gFresh.add({t, nil});
#if SWITCH_METAL_DIAGNOSTICS
    std::lock_guard<std::mutex> l(gTraceLock);
    if (!gUnwritten)
        gUnwritten = [NSMapTable weakToStrongObjectsMapTable];
    [gUnwritten setObject:@NO forKey:t];
    t->_freshUnwritten.store(true, std::memory_order_relaxed);
#endif
}

// The entries' textures written ahead of the commands of [from, to) (none: all of them), each one that range names first
// as a pass that clears all of it left to that pass; those the range never names come back in `later`
static std::vector<id> writeEntries(SwitchMetalCommandBuffer *cb, size_t from, size_t to, gm20b::CommandBuffer &rc,
                                    std::vector<FreshEntry> &entries, std::vector<FreshEntry> &later)
{
    std::vector<id> handled;
    gm20b::BlitEncoder *b = nullptr;
    for (FreshEntry &e : entries) {
        if (!live(e))
            continue;
        const FirstUse use = entryUse(cb, from, to, e);
        if (use == FirstUse::None) {
            later.push_back(e);
            continue;
        }
        handled.push_back(objectOf(e));
        if (use == FirstUse::ClearsAll) {
            SWITCH_METAL_DIAGNOSTIC(gCleared.fetch_add(1, std::memory_order_relaxed));
            continue;
        }
        if (!b)
            b = rc.blit();
        const uint64_t bytes = writeEntry(*b, e);
        SWITCH_METAL_DIAGNOSTIC(gFilledBytes.fetch_add(bytes, std::memory_order_relaxed));
        SWITCH_METAL_DIAGNOSTIC(gFilled.fetch_add(1, std::memory_order_relaxed));
        (void)bytes;
    }
    if (b) {
        b->barrier();
        b->end();
    }
    return handled;
}

FreshWrites::FreshWrites(SwitchMetalCommandBuffer *cb, size_t from, size_t to, gm20b::CommandBuffer &rc)
{
    std::vector<FreshEntry> taken = gFresh.take(), later;
    handled_ = writeEntries(cb, from, to, rc, taken, later);
    gFresh.settle(std::move(later), false);
}

bool FreshWrites::claim(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    if (!gFresh.any())
        return false;
    std::vector<FreshEntry> taken = gFresh.take(), later;
    for (FreshEntry &e : taken) {
        if (!live(e))
            continue;
        const FirstUse use = entryUse(cb, from, to, e);
        if (use == FirstUse::None) {
            later.push_back(e);
            continue;
        }
        (use == FirstUse::ClearsAll ? handled_ : pending_).push_back(objectOf(e));
        SWITCH_METAL_DIAGNOSTIC(gCleared.fetch_add(use == FirstUse::ClearsAll, std::memory_order_relaxed));
    }
    gFresh.settle(std::move(later), false);
    return !pending_.empty();
}

void FreshWrites::written(gm20b::CommandBuffer &rc)
{
    if (pending_.empty())
        return;
    gm20b::BlitEncoder *b = rc.blit();
    for (id object : pending_) {
        const uint64_t bytes = writeEntry(*b, entryOf(object));
        SWITCH_METAL_DIAGNOSTIC(gFilledBytes.fetch_add(bytes, std::memory_order_relaxed));
        SWITCH_METAL_DIAGNOSTIC(gFilled.fetch_add(1, std::memory_order_relaxed));
        (void)bytes;
    }
    b->barrier();
    b->end();
    handled_.insert(handled_.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void FreshWrites::submitted(SwitchMetalCommandBuffer *cb)
{
    submitted_ = true;
    cb->_owners.insert(cb->_owners.end(), handled_.begin(), handled_.end());
}

FreshWrites::~FreshWrites()
{
    if (submitted_ || (handled_.empty() && pending_.empty()))
        return;
    std::vector<FreshEntry> back;
    for (const std::vector<id> *list : {&handled_, &pending_})
        for (id object : *list)
            back.push_back(entryOf(object));
    gFresh.settle(std::move(back), false);
}

bool freshBeforeTransfer(SwitchMetalTexture *t)
{
    t = ownerOf(t);
    if (!t || !gFresh.extract([t](const FreshEntry &e) {
            return e.texture == t;
        }))
        return true;
    return gm20bExcludingSubmissions([t]() -> bool {
        gm20b::CommandBuffer *rc = gRM->raw->begin();
        bool submitted = rc != nullptr;
        if (submitted) {
            rc->internalCompletion();
            gm20b::BlitEncoder *b = rc->blit();
            writeFresh(*b, t);
            b->barrier();
            b->end();
            uint32_t seqno = 0;
            submitted = rc->commit(seqno);
            if (!submitted)
                rc->abandon();
        }
        if (!submitted)
            gFresh.add({t, nil});
        return submitted;
    });
}

#if SWITCH_METAL_DIAGNOSTICS
void traceFreshUses(SwitchMetalCommandBuffer *cb, size_t begin)
{
    const size_t end = cb->_plan.encoderEnd(begin);
    // Each texture it names, read with no lock, no message and no retain: an encoder of textures already written (every
    // steady frame's) costs that alone
    bool unwritten = false;
    cb->_plan.forEachResource(begin, end, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
        SwitchMetalTexture *__unsafe_unretained t = (__bridge SwitchMetalTexture *)use.object;
        t = t ? (t->_levelsOwner ?: t) : nil;
        unwritten |= t && t->_freshUnwritten.load(std::memory_order_relaxed);
    });
    if (!unwritten)
        return;
    std::lock_guard<std::mutex> l(gTraceLock);
    cb->_plan.forEachResource(begin, end, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
        SwitchMetalTexture *t = ownerOf((__bridge SwitchMetalTexture *)use.object);
        NSNumber *seen = [gUnwritten objectForKey:t];
        if (!seen)
            return;
        if (t->_heap) {
            [gUnwritten removeObjectForKey:t];
            t->_freshUnwritten.store(false, std::memory_order_relaxed);
            return;
        }
        const bool reads = (use.access & gm20b::ResourceUse::Sample) ||
                           ((use.access & gm20b::ResourceUse::Read) && !(use.access & gm20b::ResourceUse::Write));
        if (reads && !seen.boolValue) {
            gEarlyReads.fetch_add(1, std::memory_order_relaxed);
            [gUnwritten setObject:@YES forKey:t];
        }
        if (!(use.access & gm20b::ResourceUse::Write))
            return;
        [gUnwritten removeObjectForKey:t];
        t->_freshUnwritten.store(false, std::memory_order_relaxed);
    });
}
#endif

@implementation SwitchMetalDevice (Fresh)

#if SWITCH_METAL_DIAGNOSTICS
// hw_test_app's gpu.texture.fresh: textures written to their first contents by the copy engine, their bytes, those a first
// pass cleared whole instead, and reads of a fresh texture before any command wrote it
- (NSDictionary<NSString *, NSNumber *> *)switchMetalFresh
{
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"filled"] = @(gFilled.load());
    d[@"filled_bytes"] = @(gFilledBytes.load());
    d[@"cleared"] = @(gCleared.load());
    d[@"early_reads"] = @(gEarlyReads.load());
    return d;
}
#endif

@end
