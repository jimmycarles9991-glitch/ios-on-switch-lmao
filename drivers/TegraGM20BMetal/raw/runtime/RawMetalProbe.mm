// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_push.h"

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
        words.push_back(raw::Push::hdr(1, raw::SUBC_3D, v[i], 1));
        words.push_back(v[i + 1]);
    }
    return true;
}

static RMTexture *rawTexture(id object)
{
    RMTexture *t = [object isKindOfClass:[RMTexture class]] ? (RMTexture *)object : nil;
    return t && t->_rawOk && !t->_raw.memoryless && t->_raw.va && t->_kind != RMTextureView ? t : nil;
}

static uint64_t number(NSDictionary *op, NSString *key, uint64_t otherwise = 0)
{
    id value = op[key];
    return [value isKindOfClass:[NSNumber class]] ? [(NSNumber *)value unsignedLongLongValue] : otherwise;
}

static raw::ProbeSurface probeSurface(const raw::Texture &t, uint32_t fmt2d)
{
    raw::ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d ? fmt2d : raw::probeTwodFormat(t.format);
    s.pitch = t.pitch;
    s.width = t.width;
    s.height = t.height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

@implementation RMDevice (Probe)

// Arming: nil, else why not. Disarming: the draws that emitted them, as a decimal string.
- (NSString *)switchRawProbeBefore:(NSData *)before after:(NSData *)after
{
    raw::Device &d = *gRM->raw;
    if (!before) {
        d.probeArmed.store(false, std::memory_order_release);
        std::lock_guard<raw::Mutex> hold(d.probeLock);
        d.probeWords.reset();
        return [NSString stringWithFormat:@"%llu", (unsigned long long)d.probeEmitted.exchange(0)];
    }
    auto words = std::make_shared<raw::Device::ProbeWords>();
    if (!appendMethods(before, words->before) || !appendMethods(after, words->after))
        return @"methods are (method, value) uint32 pairs of 3D-class methods below 0x8000";
    if (words->before.size() + words->after.size() > 1024)
        return @"more than 512 methods";
    {
        std::lock_guard<raw::Mutex> hold(d.probeLock);
        d.probeWords = std::move(words);
        d.probeEmitted.store(0, std::memory_order_relaxed);
    }
    d.probeArmed.store(true, std::memory_order_release);
    return nil;
}

// The next private render target the device makes binds compressible memory of its own (raw_comp.h kinds)
- (NSString *)switchRawProbeCompressNext
{
    gRM->raw->probeCompressNext.store(true, std::memory_order_release);
    return nil;
}

// Private textures made while `on` is true stay on plain memory (raw_private_comp.h), for a probe's uncompressed
// reference
- (NSString *)switchRawProbePlainPrivate:(NSNumber *)on
{
    gRM->raw->probePlainPrivate.store(on.boolValue, std::memory_order_release);
    return nil;
}

// The longest a draw this process encodes from now on is estimated to keep GR before it goes in scissor bands
// (raw_draw_bands.h), in nanoseconds; 0: never banded. Answers the budget it replaced.
- (NSString *)switchRawBandBudget:(NSNumber *)ns
{
    return [NSString
        stringWithFormat:@"%llu", (unsigned long long)gRM->raw->bandBudgetNs_.exchange(ns.unsignedLongLongValue)];
}

// Storage forms view compressed attachments while set, for gpu.raw.storage_compressed
- (NSString *)switchRawProbeStorageCompressed:(NSNumber *)on
{
    gRM->raw->probeStorageCompressed.store(on.boolValue, std::memory_order_release);
    return nil;
}

// MSAA resolves take the pack's resolve pass while set, for gpu.raw.resolve_twod's reference
- (NSString *)switchRawProbeResolveDraw:(NSNumber *)on
{
    gRM->raw->probeResolveDraw.store(on.boolValue, std::memory_order_release);
    return nil;
}

// Resolves resolve.twod takes go through the complement (rule resolve.exact) while set, at any sample count
- (NSString *)switchRawProbeResolveExact:(NSNumber *)on
{
    gRM->raw->probeResolveExact.store(on.boolValue, std::memory_order_release);
    return nil;
}

// A fetching draw whose state has a fetch-free form of rung `rung` (2 or 3) takes it whatever its rule; 4 keeps every draw
// on the storage form; 5 and 6 draw a blended storage form's barrier twin, a barrier a triangle or one a draw
// (raw_fetch_rungs.h); 0 gives the decision back to the rules (raw_fetch_free.h), for HW Test gpu.raw.fetch_rung2/3 and
// gpu.raw.blend_twin
- (NSString *)switchRawProbeFetchRung:(NSNumber *)rung
{
    const unsigned value = rung.unsignedIntValue;
    if (value && (value < 2 || value > 6))
        return @"rung is 0, 2, 3, 4, 5 or 6";
    gRM->raw->probeFetchRung.store(uint8_t(value), std::memory_order_release);
    return nil;
}

// What a probe's line says of a texture and the device: "raw=1 comptags=1 block_linear=1 pitch=N tiled_cache=1
// compression=1", raw=0 where the raw path does not hold it
- (NSString *)switchRawProbeFacts:(id<MTLTexture>)texture
{
    const uint32_t flags = gRM->raw->kernelInfo().flags;
    RMTexture *t = rawTexture(texture);
    id twin = nil;
    if (t)
        @synchronized(t) {
            twin = t->_twin;
        }
    return
        [NSString stringWithFormat:@"raw=%d comptags=%d block_linear=%d pitch=%u tiled_cache=%d compression=%d twin=%d",
                                   t != nil, t && t->_raw.comptags, t && t->_raw.blockLinear, t ? t->_raw.pitch : 0,
                                   (flags & TegraGM20BInfoTiledCache) != 0, (flags & TegraGM20BInfoCompression) != 0,
                                   twin != nil];
}

@end

@implementation RMCommandBuffer (Probe)

// Its barriers' forms (raw_probe_forms.h parseProbeForms), set before it commits: nil, else why not
- (NSString *)switchRawProbeForms:(NSString *)forms
{
    raw::ProbeForms f;
    if (!raw::parseProbeForms(forms.UTF8String, f))
        return [NSString stringWithFormat:@"forms not understood: %@", forms];
    std::lock_guard<raw::Mutex> lock(_lock);
    if (_committed)
        return @"committed already";
    _probeForms = f;
    return nil;
}

// Where its barriers took the forms, once it completed (raw_probe_forms.h ProbeTaken)
- (NSString *)switchRawProbeTaken
{
    std::lock_guard<raw::Mutex> lock(_lock);
    return @(_probeTaken.text().c_str());
}

// 2D-engine work at the open blit encoder (raw_probe_ops.h): {kind: "scale"|"fill", read, written: textures, dx, dy, dw,
// dh, and for scale du, dv, sx, sy (32.32), bilinear, corner, fmt2d (0: each texture's own); for fill colorFormat,
// color0..color3, reference (the DDX's state first); unbracketed: no waits around it}. Or {kind: "method", method, value,
// written}: one 3D-class method there. nil, else why not
- (NSString *)switchRawProbeEngine:(NSDictionary *)op
{
    RMTexture *written = rawTexture(op[@"written"]), *read = rawTexture(op[@"read"]);
    const bool fill = [op[@"kind"] isEqual:@"fill"], method = [op[@"kind"] isEqual:@"method"];
    if (!written || (!fill && !method && !read))
        return @"a texture the raw path does not hold (memoryless, a view, or refused)";
    std::lock_guard<raw::Mutex> lock(_lock);
    if (_committed || !_encOpen || ![_active isKindOfClass:[RMBlitEncoder class]])
        return @"no blit encoder open";
    if (method) {
        raw::ProbeOp o;
        o.kind = raw::ProbeOpKind::Method;
        o.method = uint32_t(number(op, @"method"));
        o.value = uint32_t(number(op, @"value"));
        if (o.method >= 0x8000 || o.method % 4)
            return @"a 3D-class method below 0x8000";
        _probeOps.push_back({_encAt, o, nil, written});
        return nil;
    }
    raw::ProbeOp o;
    o.kind = fill ? raw::ProbeOpKind::Fill : raw::ProbeOpKind::Scale;
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
    if (fill && !raw::probeSolidFill(o.dst.fmt2d, o.colorFormat))
        return @"a fill's colour format is its destination's own 2D format, not an sRGB one (probeSolidFill)";
    if (o.dx + o.dw > o.dst.width || o.dy + o.dh > o.dst.height)
        return @"a rectangle outside the written texture";
    _probeOps.push_back({_encAt, o, read, written});
    return nil;
}

@end
#endif
