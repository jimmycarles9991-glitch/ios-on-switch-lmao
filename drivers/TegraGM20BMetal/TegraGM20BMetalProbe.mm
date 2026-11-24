// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BGrPreemptABI.h"
#include "push.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// GM20B methods Metal cannot reach, for hw_test_app's hardware probes (app27/X1RawProbes.swift): each NSData holds
// (method, value) uint32 pairs of the 3D class. Every draw of one instance this process encodes emits `before` after
// its state and `after` once it ended, until a call with `before` nil disarms them and answers how many draws emitted
// them. The probe's command buffers must complete before it disarms: their encoding may run after commit returns.
static bool appendMethods(NSData *pairs, std::vector<uint32_t> &words)
{
    if (!pairs)
        return true;
    if (pairs.length % 8)
        return false;
    const auto *v = static_cast<const uint32_t *>(pairs.bytes);
    for (NSUInteger i = 0; i < pairs.length / 4; i += 2) {
        if (v[i] >= 0x8000 || v[i] % 4)
            return false;
        words.push_back(gm20b::Push::hdr(1, gm20b::SUBC_3D, v[i], 1));
        words.push_back(v[i + 1]);
    }
    return true;
}

static SwitchMetalTexture *gm20bTexture(id object)
{
    SwitchMetalTexture *t = [object isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)object : nil;
    if (!t || !t->_gm20bOk || t->_raw.memoryless || t->_kind == SwitchMetalTextureView)
        return nil;
    return gRM->raw->lockedCopy(t->_raw).va ? t : nil;
}

static uint64_t number(NSDictionary *op, NSString *key, uint64_t otherwise = 0)
{
    id value = op[key];
    return [value isKindOfClass:[NSNumber class]] ? [(NSNumber *)value unsignedLongLongValue] : otherwise;
}

static gm20b::ProbeSurface probeSurface(const gm20b::Texture &t, uint32_t fmt2d)
{
    gm20b::ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d ? fmt2d : gm20b::probeTwodFormat(t.format);
    s.pitch = t.pitch;
    s.width = t.width;
    s.height = t.height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

@implementation SwitchMetalDevice (Probe)

// Arming: nil, else why not. Disarming: the draws that emitted them, as a decimal string.
- (NSString *)switchMetalProbeBefore:(NSData *)before after:(NSData *)after
{
    gm20b::Device &d = *gRM->raw;
    if (!before) {
        d.probeArmed.store(false, std::memory_order_release);
        std::lock_guard<gm20b::Mutex> hold(d.probeLock);
        d.probeWords.reset();
        return [NSString stringWithFormat:@"%llu", (unsigned long long)d.probeEmitted.exchange(0)];
    }
    auto words = std::make_shared<gm20b::Device::ProbeWords>();
    if (!appendMethods(before, words->before) || !appendMethods(after, words->after))
        return @"methods are (method, value) uint32 pairs of 3D-class methods below 0x8000";
    if (words->before.size() + words->after.size() > 1024)
        return @"more than 512 methods";
    {
        std::lock_guard<gm20b::Mutex> hold(d.probeLock);
        d.probeWords = std::move(words);
        d.probeEmitted.store(0, std::memory_order_relaxed);
    }
    d.probeArmed.store(true, std::memory_order_release);
    return nil;
}

// The next private render target the device makes binds compressible memory of its own (comp.h kinds)
- (NSString *)switchMetalProbeCompressNext
{
    gRM->raw->probeCompressNext.store(true, std::memory_order_release);
    return nil;
}

// `settled` called once every compressed memory and ZBC entry the device was asked for so far is made or refused (gpu.h
// compWhenSettled): on the compression thread with no driver lock held, or on this thread before the call returns when
// none is asked. Nil, else why not.
- (NSString *)switchMetalProbeCompressionSettled:(void (^)(void))settled
{
    if (!settled)
        return @"a block to call";
    gRM->raw->compWhenSettled([settled] {
        settled();
    });
    return nil;
}

// While `on`: the calling thread's next getBytes or replaceRegion the copy engine moves fails as the kernel's refusal of
// its submission does (transfer.h failNextTransfer). @YES where the arm before this call fired.
- (NSNumber *)switchMetalProbeFailTransfer:(NSNumber *)on
{
    return @(gm20b::failNextTransfer(on.boolValue));
}

// The event clock (submit_queue.h EventDeadline::now) held from the first call, moved on by `ms`, and the submit
// worker woken to judge its parked waits against it: a test's way to Metal's 5 s bound on a held wait without sleeping,
// with no real time in what it measures. One test thread calls it.
- (NSNumber *)switchMetalProbeAdvanceEventClock:(NSNumber *)ms
{
    using namespace std::chrono;
    int64_t held = gm20b::gEventClockHeld.load(std::memory_order_relaxed);
    if (!held)
        held = duration_cast<nanoseconds>(gm20b::EventDeadline::Clock::now().time_since_epoch()).count();
    gm20b::gEventClockHeld.store(held + ms.longLongValue * 1000000, std::memory_order_release);
    wakeSubmit(nullptr);
    return ms;
}

// HW Test's probes of the compression and zcull cases left (gpu.h probeZetaLayers .. probeZcullCriterion), each while
// `on`: switches, not one-shot arms, since a probe's texture is made twice where its first finds no compressed memory
// made (CompressionSettled.remade). Nil.
- (NSString *)switchMetalProbeZetaLayers:(NSNumber *)on
{
    gRM->raw->probeZetaLayers.store(on.boolValue, std::memory_order_release);
    return nil;
}

- (NSString *)switchMetalProbeZetaShadow:(NSNumber *)on
{
    gRM->raw->probeZetaShadow.store(on.boolValue, std::memory_order_release);
    return nil;
}

// `row`: a mixed pass's probe row (mixed_samples.h kTirProbeZcull .. TirEnd), 0 off
- (NSString *)switchMetalProbeTir:(NSNumber *)row
{
    gRM->raw->probeTir.store(row.unsignedIntValue, std::memory_order_release);
    return nil;
}

// `packed`: byte 0 SET_ZCULL_CRITERION's SFUNC, byte 1 SREF, byte 2 SMASK; 0 (or nil) off: NVK's NEVER
- (NSString *)switchMetalProbeZcullCriterion:(NSNumber *)packed
{
    gRM->raw->probeZcullCriterion.store(packed.unsignedIntValue & 0xffffff, std::memory_order_release);
    return nil;
}

// Private textures made while `on` is true stay on plain memory (private_comp.h), for a probe's uncompressed
// reference
- (NSString *)switchMetalProbePlainPrivate:(NSNumber *)on
{
    gRM->raw->probePlainPrivate.store(on.boolValue, std::memory_order_release);
    return nil;
}

// The longest a draw this process encodes from now on is estimated to keep GR before it goes in scissor bands
// (draw_bands.h), in nanoseconds; 0: never banded. Answers the budget it replaced.
- (NSString *)switchMetalBandBudget:(NSNumber *)ns
{
    return [NSString
        stringWithFormat:@"%llu", (unsigned long long)gRM->raw->bandBudgetNs_.exchange(ns.unsignedLongLongValue)];
}

- (NSString *)switchMetalProbeComputeCta:(NSNumber *)on
{
    int kr = kIOReturnNotReady;
    TegraGM20BAbiGrPreemptReply reply = {};
    gm20bRunOnSubmitWorker([&] {
        const uint32_t channel = gRM->raw->probeChannel();
        if (!channel)
            return;
        const TegraGM20BAbiGrPreempt in = {
            channel, on.boolValue ? uint32_t(TegraGM20BGrPreemptCta) : uint32_t(TegraGM20BGrPreemptWfi), {0, 0}};
        kr = gRM->raw->kernelDebugCall(TegraGM20BMethodGrPreempt, &in, sizeof in, &reply, sizeof reply);
    });
    if (kr != KERN_SUCCESS)
        return [NSString stringWithFormat:@"error=0x%x", unsigned(kr)];
    return [NSString stringWithFormat:@"old=%u readback=%u", reply.old_mode, reply.readback];
}

- (NSString *)switchMetalProbeMacroBands:(NSNumber *)on
{
    gRM->raw->probeMacroBands.store(on.boolValue, std::memory_order_release);
    return nil;
}

// Storage forms view compressed attachments while set, for gpu.gm20b.storage_compressed
- (NSString *)switchMetalProbeStorageCompressed:(NSNumber *)on
{
    gRM->raw->probeStorageCompressed.store(on.boolValue, std::memory_order_release);
    return nil;
}

// MSAA resolves take the pack's resolve pass while set, for gpu.gm20b.resolve_twod's reference
- (NSString *)switchMetalProbeResolveDraw:(NSNumber *)on
{
    gRM->raw->probeResolveDraw.store(on.boolValue, std::memory_order_release);
    return nil;
}

// Resolves of resolve.twod's shape, readable or not, go through the grid's complement while set: the engine's reading
// under ROP3 NOTSRCCOPY, which resolve_rule.h routes nothing through
- (NSString *)switchMetalProbeResolveExact:(NSNumber *)on
{
    gRM->raw->probeResolveExact.store(on.boolValue, std::memory_order_release);
    return nil;
}

// A fetching draw whose state has a fetch-free form of rung `rung` (2 or 3) takes it whatever its rule; 4 keeps every draw
// on the storage form; 5 and 6 draw a blended storage form's barrier twin, a barrier a triangle or one a draw
// (fetch_rungs.h); 0 gives the decision back to the rules (fetch_free.h), for HW Test gpu.gm20b.fetch_rung2/3 and
// gpu.gm20b.blend_twin
- (NSString *)switchMetalProbeFetchRung:(NSNumber *)rung
{
    const unsigned value = rung.unsignedIntValue;
    if (value && (value < 2 || value > 6))
        return @"rung is 0, 2, 3, 4, 5 or 6";
    gRM->raw->probeFetchRung.store(uint8_t(value), std::memory_order_release);
    return nil;
}

// gpu.x1.job_boundary_idle's header cell: [a, b] makes texture a's header describe b's image from the next submission's
// header invalidate on, [a] gives a its own back; nil, else why not
- (NSString *)switchMetalProbeRetarget:(NSArray *)textures
{
    SwitchMetalTexture *a = textures.count ? gm20bTexture(textures[0]) : nil;
    SwitchMetalTexture *to = textures.count > 1 ? gm20bTexture(textures[1]) : nil;
    if (!a || (textures.count > 1 && !to))
        return @"not raw textures";
    return gRM->raw->probeRetarget(a->_raw, to ? &to->_raw : nullptr) ? nil : @"a texture with no header";
}

// What a probe's line says of a texture and the device: "raw=1 comptags=1 block_linear=1 pitch=N tiled_cache=1
// compression=1", raw=0 where the driver does not hold it
- (NSString *)switchMetalProbeFacts:(id<MTLTexture>)texture
{
    const uint32_t flags = gRM->raw->kernelInfo().flags;
    SwitchMetalTexture *t = gm20bTexture(texture);
    const gm20b::Texture now = t ? gRM->raw->lockedCopy(t->_raw) : gm20b::Texture();
    return
        [NSString stringWithFormat:@"raw=%d comptags=%d block_linear=%d pitch=%u tiled_cache=%d compression=%d",
                                   t != nil, t && now.comptags, t && now.blockLinear, t ? now.pitch : 0,
                                   (flags & TegraGM20BInfoTiledCache) != 0, (flags & TegraGM20BInfoCompression) != 0];
}

// A texture's header as the texture unit reads it, its eight words dot-separated; nil where the driver holds no header
// for it
- (NSString *)switchMetalProbeHeader:(id<MTLTexture>)texture
{
    SwitchMetalTexture *t = gm20bTexture(texture);
    const gm20b::Texture now = t ? gRM->raw->lockedCopy(t->_raw) : gm20b::Texture();
    if (!now.tic)
        return nil;
    const uint32_t *w = gRM->raw->headerWords(now.tic);
    return [NSString
        stringWithFormat:@"%08x.%08x.%08x.%08x.%08x.%08x.%08x.%08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]];
}

@end

@implementation SwitchMetalCommandBuffer (Probe)

// Its barriers' forms (probe_forms.h parseProbeForms), set before it commits: nil, else why not
- (NSString *)switchMetalProbeForms:(NSString *)forms
{
    gm20b::ProbeForms f;
    if (!gm20b::parseProbeForms(forms.UTF8String, f))
        return [NSString stringWithFormat:@"forms not understood: %@", forms];
    std::lock_guard<gm20b::Mutex> lock(_lock);
    if (_committed)
        return @"committed already";
    gm20bPipelineVeto(self, VetoProbes);
    _probeForms = f;
    return nil;
}

// Where its barriers took the forms, once it completed (probe_forms.h ProbeTaken)
- (NSString *)switchMetalProbeTaken
{
    std::lock_guard<gm20b::Mutex> lock(_lock);
    return @(_probeTaken.text().c_str());
}

// 2D-engine work at the open blit encoder (probe_ops.h): {kind: "scale"|"fill", read, written: textures, dx, dy, dw,
// dh, and for scale du, dv, sx, sy (32.32), bilinear, corner, fmt2d (0: each texture's own); for fill colorFormat,
// color0..color3, reference (the DDX's state first); unbracketed: no waits around it}. Or {kind: "method", method, value,
// written}: one 3D-class method there. nil, else why not
- (NSString *)switchMetalProbeEngine:(NSDictionary *)op
{
    SwitchMetalTexture *written = gm20bTexture(op[@"written"]), *read = gm20bTexture(op[@"read"]);
    const bool fill = [op[@"kind"] isEqual:@"fill"], method = [op[@"kind"] isEqual:@"method"];
    if (!written || (!fill && !method && !read))
        return @"a texture the driver does not hold (memoryless, a view, or refused)";
    std::lock_guard<gm20b::Mutex> lock(_lock);
    if (_committed || !_encOpen || ![_active isKindOfClass:[SwitchMetalBlitEncoder class]])
        return @"no blit encoder open";
    if (method) {
        gm20b::ProbeOp o;
        o.kind = gm20b::ProbeOpKind::Method;
        o.method = uint32_t(number(op, @"method"));
        o.value = uint32_t(number(op, @"value"));
        if (o.method >= 0x8000 || o.method % 4)
            return @"a 3D-class method below 0x8000";
        gm20bPipelineVeto(self, VetoProbes);
        _probeOps.push_back({_encAt, o, nil, written});
        return nil;
    }
    gm20b::ProbeOp o;
    o.kind = fill ? gm20b::ProbeOpKind::Fill : gm20b::ProbeOpKind::Scale;
    const uint32_t fmt2d = uint32_t(number(op, @"fmt2d"));
    o.dst = probeSurface(written->_raw, fmt2d);
    if (read)
        o.src = probeSurface(read->_raw, fmt2d);
    o.dx = uint32_t(number(op, @"dx"));
    o.dy = uint32_t(number(op, @"dy"));
    o.dw = uint32_t(number(op, @"dw", written->_raw.width));
    o.dh = uint32_t(number(op, @"dh", written->_raw.height));
    o.du = number(op, @"du", 1ull << 32);
    o.dv = number(op, @"dv", 1ull << 32);
    o.sx = number(op, @"sx");
    o.sy = number(op, @"sy");
    o.bilinear = number(op, @"bilinear") != 0;
    o.corner = number(op, @"corner") != 0;
    o.colorFormat = uint32_t(number(op, @"colorFormat"));
    o.referenceFill = number(op, @"reference") != 0;
    o.bracketed = number(op, @"unbracketed") == 0;
    for (int c = 0; c < 4; ++c)
        o.color[c] = uint32_t(number(op, [NSString stringWithFormat:@"color%d", c]));
    if (!o.dst.fmt2d || (read && !o.src.fmt2d))
        return @"a format the 2D engine names none of";
    if (fill && !gm20b::probeSolidFill(o.dst.fmt2d, o.colorFormat))
        return @"a fill's colour format is its destination's own 2D format, not an sRGB one (probeSolidFill)";
    if (o.dx + o.dw > o.dst.width || o.dy + o.dh > o.dst.height)
        return @"a rectangle outside the written texture";
    gm20bPipelineVeto(self, VetoProbes);
    _probeOps.push_back({_encAt, o, read, written});
    // The blit encoder's uses, as a copy's: a new texture's first contents (TegraGM20BMetalFresh.mm) then go ahead of this
    // encoder, not ahead of the next command buffer that names the texture, over what the 2D engine wrote
    std::vector<gm20b::ResourceUse> uses = {{(__bridge void *)written, gm20b::ResourceUse::Write}};
    [self retain:written];
    if (read) {
        uses.push_back({(__bridge void *)read, gm20b::ResourceUse::Read});
        [self retain:read];
    }
    _plan.addUses(_plan.size() - 1, uses);
    return nil;
}

@end
#endif
