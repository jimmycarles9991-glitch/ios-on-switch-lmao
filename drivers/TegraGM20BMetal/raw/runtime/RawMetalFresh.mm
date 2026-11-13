// SPDX-License-Identifier: MIT
#import "RawMetalFresh.h"
#include "raw_fresh.h"
#include "raw_fresh_queue.h"

namespace {

struct FreshEntry {
    __weak RMTexture *texture;
};

raw::FreshQueue<FreshEntry> gFresh;

#if SWITCH_RAW_DIAGNOSTICS
std::atomic<uint64_t> gFilled{0}, gFilledBytes{0}, gCleared{0}, gAhead{0};
std::mutex gTraceLock;
NSMapTable<RMTexture *, NSString *>
    *gUnwritten;  // fresh textures no command wrote yet: the read before it, if one came
std::atomic<uint64_t> gEarlyReads{0};
#endif

RMTexture *ownerOf(RMTexture *t)
{
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    return t;
}

void fillLevel(raw::BlitEncoder &b, const raw::Texture &image, const raw::CopyFill &fill, uint32_t planes)
{
    for (uint32_t z = 0; z < planes; z++)
        b.fillTexels(image, z, image.width, image.height, fill);
}

uint64_t writeFresh(raw::BlitEncoder &b, RMTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const raw::Texture &x = t->_raw;
    const uint32_t levels = x.chain ? x.chain->layout.levels : 1;
    const uint32_t patterned = raw::patternLevels(uint64_t(d.usage), d.allowGPUOptimizedContents, uint32_t(d.width),
                                                  uint32_t(d.height), levels);
    const uint8_t zeros[16] = {};
    raw::CopyFill zero, pattern;
    raw::FreshTexel texel;
    if (!raw::copyFillOf(zeros, x.bpp, zero))
        return 0;
    const bool hasPattern = patterned && raw::freshPatternTexel(x.format, texel) && texel.size == x.bpp &&
                            raw::copyFillOf(texel.bytes, texel.size, pattern);
    const uint32_t planes = x.type == raw::fx::kTexThreeD ? x.depth : x.layers;
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < levels; level++) {
        raw::Texture image;
        if (x.chain && !raw::chainLevelImage(x, level, image))
            continue;
        const raw::Texture &target = x.chain ? image : x;
        fillLevel(b, target, hasPattern && level < patterned ? pattern : zero, planes);
        bytes += uint64_t(target.width) * target.height * planes * x.bpp;
    }
    return bytes;
}

enum class FirstUse {
    None,
    ClearsAll,
    Other
};

// The first encoder of [from, to) that names `t` (or a view of it): a pass that clears and stores all of it before
// anything reads it (raw_fresh.h firstPassWritesAll), or any other use
FirstUse firstUse(RMCommandBuffer *cb, size_t from, size_t to, RMTexture *t)
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
        cb->_plan.forEachResource(at, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            if (ownerOf((__bridge RMTexture *)use.object) == t) {
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
        const RMPass &pass = cb->_passes[cb->_plan.legacy(at).x];
        const raw::Texture &x = t->_raw;
        for (const RMPass::Color &c : pass.color)
            if (c.texture == t && c.level == 0 &&
                raw::firstPassWritesAll(x.width, x.height, x.chain ? x.chain->layout.levels : 1,
                                        x.type == raw::fx::kTexThreeD ? x.depth : x.layers,
                                        c.load == MTLLoadActionClear, c.store == MTLStoreActionStore,
                                        access & raw::ResourceUse::Sample, uint32_t(pass.width), uint32_t(pass.height)))
                return FirstUse::ClearsAll;
        return FirstUse::Other;
    }
    return FirstUse::None;
}

}  // namespace

void establishFresh(RMTexture *t, MTLTextureDescriptor *d)
{
    if (!t->_rawOk || t->_kind != RMTexturePool || t->_heap || t->_raw.memoryless || t->_raw.arena || t->_raw.zeta ||
        d.storageMode != MTLStorageModePrivate)
        return;
    gFresh.add({t});
#if SWITCH_RAW_DIAGNOSTICS
    std::lock_guard<std::mutex> l(gTraceLock);
    if (!gUnwritten)
        gUnwritten = [NSMapTable weakToStrongObjectsMapTable];
    [gUnwritten setObject:@"" forKey:t];
    t->_freshUnwritten.store(true, std::memory_order_relaxed);
#endif
}

// The entries' textures written ahead of the commands of [from, to) (none: all of them), each one that range names first
// as a pass that clears all of it left to that pass; those the range never names come back in `later`
static std::vector<id> writeEntries(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc,
                                    std::vector<FreshEntry> &entries, std::vector<FreshEntry> &later)
{
    std::vector<id> handled;
    raw::BlitEncoder *b = nullptr;
    for (FreshEntry &e : entries) {
        RMTexture *t = e.texture;
        if (!t || t->_heap)
            continue;
        const FirstUse use = to > from ? firstUse(cb, from, to, t) : FirstUse::Other;
        if (use == FirstUse::None) {
            later.push_back({t});
            continue;
        }
        handled.push_back(t);
        if (use == FirstUse::ClearsAll) {
            RAW_DIAGNOSTIC(gCleared.fetch_add(1, std::memory_order_relaxed));
            continue;
        }
        if (!b)
            b = rc.blit();
        const uint64_t bytes = writeFresh(*b, t);
        RAW_DIAGNOSTIC(gFilledBytes.fetch_add(bytes, std::memory_order_relaxed));
        RAW_DIAGNOSTIC(gFilled.fetch_add(1, std::memory_order_relaxed));
        (void)bytes;
    }
    if (b) {
        b->barrier();
        b->end();
    }
    return handled;
}

FreshWrites::FreshWrites(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc)
{
    std::vector<FreshEntry> taken = gFresh.take(), later;
    handled_ = writeEntries(cb, from, to, rc, taken, later);
    gFresh.settle(std::move(later), false);
}

void FreshWrites::submitted(RMCommandBuffer *cb)
{
    submitted_ = true;
    cb->_owners.insert(cb->_owners.end(), handled_.begin(), handled_.end());
}

FreshWrites::~FreshWrites()
{
    if (submitted_ || handled_.empty())
        return;
    std::vector<FreshEntry> back;
    for (id t : handled_)
        back.push_back({(RMTexture *)t});
    gFresh.settle(std::move(back), false);
}

bool writeFreshAhead(RMCommandBuffer *cb, size_t from, size_t to)
{
#if SWITCH_RAW_DIAGNOSTICS
    for (size_t at = from; at < to;) {
        const auto op = cb->_plan.operation(at).op;
        if (op != OpRender && op != OpCompute && op != OpBlit) {
            at++;
            continue;
        }
        traceFreshUses(cb, at);
        at = std::max(cb->_plan.encoderEnd(at), at + 1);
    }
#else
    (void)from;
    (void)to;
#endif
    return gFresh.writeAhead(gRM->gpuOrder, [&](std::vector<FreshEntry> &taken) {
        raw::CommandBuffer *rc = gRM->raw->begin();
        if (!rc)
            return false;
        rc->internalCompletion();
        std::vector<FreshEntry> later;
        const std::vector<id> handled = writeEntries(cb, 0, 0, *rc, taken, later);
        uint32_t seqno = 0;
        if (!rc->commit(seqno)) {
            rc->abandon();
            return false;
        }
        cb->_owners.insert(cb->_owners.end(), handled.begin(), handled.end());
        gRM->rawSinceM2v = true;
        RAW_DIAGNOSTIC(gAhead.fetch_add(1, std::memory_order_relaxed));
        return true;
    });
}

#if SWITCH_RAW_DIAGNOSTICS
void traceFreshUses(RMCommandBuffer *cb, size_t begin)
{
    const size_t end = cb->_plan.encoderEnd(begin);
    // Each texture it names, read with no lock, no message and no retain: an encoder of textures already written (every
    // steady frame's) costs that alone
    bool unwritten = false;
    cb->_plan.forEachResource(begin, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        RMTexture *__unsafe_unretained t = (__bridge RMTexture *)use.object;
        while (t && t->_kind == RMTextureView && t->_parent)
            t = t->_parent;
        unwritten |= t && t->_freshUnwritten.load(std::memory_order_relaxed);
    });
    if (!unwritten)
        return;
    std::lock_guard<std::mutex> l(gTraceLock);
    NSString *here = nil;
    auto describe = [&] {
        if (here)
            return here;
        const auto op = cb->_plan.operation(begin).op;
        NSString *state = @"-";
        if (op == OpRender)
            for (void *p : cb->_plan.renderPipelines(begin, end))
                if (NSString *label = ((__bridge RMPipeline *)p).label) {
                    state = label;
                    break;
                }
        here = [NSString stringWithFormat:@"%s %zu of command buffer %p (%@)",
                                          op == OpRender ? "pass"
                                          : op == OpBlit ? "blit"
                                                         : "compute",
                                          begin, cb, state];
        return here;
    };
    cb->_plan.forEachResource(begin, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        RMTexture *t = ownerOf((__bridge RMTexture *)use.object);
        NSString *readBy = [gUnwritten objectForKey:t];
        if (!readBy)
            return;
        if (t->_heap) {
            [gUnwritten removeObjectForKey:t];
            t->_freshUnwritten.store(false, std::memory_order_relaxed);
            return;
        }
        const raw::Texture &x = t->_raw;
        const bool reads = (use.access & raw::ResourceUse::Sample) ||
                           ((use.access & raw::ResourceUse::Read) && !(use.access & raw::ResourceUse::Write));
        if (reads && !readBy.length) {
            const uint64_t n = gEarlyReads.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 64 || n % 256 == 0)
                os_log(rawLog(),
                       "raw: fresh %ux%u format %u texture %{public}@ read by %{public}@ before any command wrote it "
                       "(#%llu)",
                       x.width, x.height, x.format, t.label ?: @"-", describe(), (unsigned long long)n);
            readBy = describe();
            [gUnwritten setObject:readBy forKey:t];
        }
        if (!(use.access & raw::ResourceUse::Write))
            return;
        if (readBy.length)
            os_log(
                rawLog(),
                "raw: fresh %ux%u format %u texture %{public}@ first written by %{public}@, after %{public}@ read it",
                x.width, x.height, x.format, t.label ?: @"-", describe(), readBy);
        [gUnwritten removeObjectForKey:t];
        t->_freshUnwritten.store(false, std::memory_order_relaxed);
    });
}
#endif

@implementation RMDevice (Fresh)

#if SWITCH_RAW_DIAGNOSTICS
// hw_test_app's gpu.texture.fresh: textures written to their first contents by the copy engine, their bytes, those a first
// pass cleared whole instead, submissions made ahead of the other driver's for them, and reads of a fresh texture before any
// command wrote it
- (NSDictionary<NSString *, NSNumber *> *)switchRawFresh
{
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"filled"] = @(gFilled.load());
    d[@"filled_bytes"] = @(gFilledBytes.load());
    d[@"cleared"] = @(gCleared.load());
    d[@"ahead"] = @(gAhead.load());
    d[@"early_reads"] = @(gEarlyReads.load());
    return d;
}
#endif

@end
