// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "bound_texture.h"
#include "draw_check.h"
#include "xr_pools.h"

@implementation SwitchMetalRenderEncoder (Bindings)

// Room for `padded` bytes in the command buffer's setBytes chunks (SwitchMetalBytes): ok=false when only heap memory had it
__attribute__((noinline)) static uint8_t *bytesRoomSlow(SwitchMetalBytes &s, uint32_t padded, uint32_t &chunk,
                                                        uint32_t &offset, bool &ok)
{
    while (s.at < s.chunks.size() && s.used + padded > s.chunks.own(s.at).length) {
        s.at++;
        s.used = 0;
    }
    if (s.at == s.chunks.size()) {
        gm20b::Buffer b;
        if (!gm20bTakeBytesChunk(std::max(padded, kRMBytesChunk), b)) {
            b = {};
            b.length = std::max(padded, kRMBytesChunk);
            b.cpu = static_cast<uint8_t *>(malloc(size_t(b.length)));
        }
        s.chunks.push_back(b);
        s.used = 0;
    }
    chunk = uint32_t(s.at);
    offset = s.used;
    s.used += padded;
    const gm20b::Buffer &cur = s.chunks.own(s.at);
    s.curCpu = cur.cpu;
    s.curLen = uint32_t(std::min<uint64_t>(cur.length, UINT32_MAX));
    s.curOk = cur.va != 0;
    ok = s.curOk;
    return s.curCpu + offset;
}

static inline uint8_t *bytesRoom(SwitchMetalBytes &s, uint32_t padded, uint32_t &chunk, uint32_t &offset, bool &ok)
{
    if (padded <= s.curLen - s.used) {
        chunk = uint32_t(s.at);
        offset = s.used;
        s.used += padded;
        ok = s.curOk;
        return s.curCpu + offset;
    }
    return bytesRoomSlow(s, padded, chunk, offset, ok);
}

__attribute__((noinline)) static void reportBytesOom(__unsafe_unretained SwitchMetalCommandBuffer *cb,
                                                     NSUInteger length)
{
    static std::atomic<bool> said{false};
    if (!said.exchange(true, std::memory_order_relaxed))
        os_log_error(gm20bLog(),
                     "SwitchMetal: setBytes of %lu bytes: no GPU memory for its chunk (%{public}s): its command "
                     "buffer fails out of memory; later ones not logged",
                     (unsigned long)length, gRM->raw->kernel().error().c_str());
    failCommandBuffer(cb, @"no GPU memory for setBytes", MTLCommandBufferErrorOutOfMemory);
}

void recordBytes(__unsafe_unretained SwitchMetalCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length,
                 bool shadow)
{
    constexpr NSUInteger kShadowed = 1024;  // a Core Animation draw's vertices and its transform
    if (shadow && length <= kShadowed) {
        m.shadow = uint32_t(cb->_shadow.size() + 1);
        cb->_shadow.appendBytes(static_cast<const uint8_t *>(bytes), length);
    }
    const uint32_t padded = std::max<uint32_t>((uint32_t(length) + 255) & ~255u, 256);
    uint32_t chunk, offset;
    bool ok = false;
    // Out of memory as Metal reports it: the command buffer fails (its bytes go to the heap meanwhile, so recording goes
    // on), never another backend
    uint8_t *to = bytesRoom(cb->_bytes, padded, chunk, offset, ok);
    if (!ok)
        reportBytesOom(cb, length);
    if (length)
        memcpy(to, bytes, length);
    if (padded > length)
        memset(to + length, 0, padded - length);
    if (gm20b::Buffer &buf = cb->_bytes.chunks.own(chunk); !buf.gpuVolatile)
        buf.markCpuDirty();
    m.a = offset;
    m.b = uint32_t(length);
    m.c = chunk;
    [cb recordBinding:m];
}

static void bindBytes(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, const void *bytes, NSUInteger length,
                      NSUInteger index)
{
    SWITCH_API(ApiBind);
    if (stage >= 1 && stage <= 2 && index < 31)
        e->_bufBound[stage - 1] &= ~(1u << index);
    __unsafe_unretained SwitchMetalCommandBuffer *cb = e->_cb;
    if (!cb->_commandLog && (stage == 1 || stage == 2) && index < 31) {
        uint32_t shadowOff = 0;
        constexpr NSUInteger kShadowed = 1024;
        if (stage == 1 && (!e->_drew || index <= 2) && length <= kShadowed) {
            shadowOff = uint32_t(cb->_shadow.size() + 1);
            cb->_shadow.appendBytes(static_cast<const uint8_t *>(bytes), length);
        }
        const uint32_t padded = std::max<uint32_t>((uint32_t(length) + 255) & ~255u, 256);
        uint32_t chunk, offset;
        bool ok = false;
        uint8_t *to = bytesRoom(cb->_bytes, padded, chunk, offset, ok);
        if (!ok)
            reportBytesOom(cb, length);
        if (length)
            memcpy(to, bytes, length);
        if (padded > length)
            memset(to + length, 0, padded - length);
        if (gm20b::Buffer &buf = cb->_bytes.chunks.own(chunk); !buf.gpuVolatile)
            buf.markCpuDirty();
        cb->_plan.bindBytes(uint32_t(stage - 1), uint32_t(index), chunk, offset, uint32_t(length), shadowOff);
        return;
    }
    recordBytes(cb, cmd(OpBytes, uint8_t(stage), uint16_t(index)), bytes, length,
                stage == 1 && (!e->_drew || index <= 2));
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    bindBytes(self, 1, bytes, length, index);
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    bindBytes(self, 2, bytes, length, index);
}

static inline void bindBuffer(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage,
                              __unsafe_unretained id<MTLBuffer> b, NSUInteger offset, NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained SwitchMetalBuffer *x = gm20bAsBuffer(b);
    if (b && !x)
        return gm20bCallInvalid(e->_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length,
                                "a buffer not made by this device");
    if (stage >= 1 && stage <= 2 && index < 31) {
        if (b)
            e->_bufBound[stage - 1] |= 1u << index;
        else
            e->_bufBound[stage - 1] &= ~(1u << index);
        e->_eligibility++;
    }
    setBinding(e->_cb, OpBuffer, stage, index, b, offset);
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    bindBuffer(self, 1, b, offset, index);
}

- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    bindBuffer(self, 2, b, offset, index);
}

static inline void bindBufferOffset(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, NSUInteger offset,
                                    NSUInteger index)
{
    SWITCH_API(ApiBind);
    if (stage >= 1 && stage <= 2 && index < 31 && (e->_bufBound[stage - 1] & (1u << index)))
        e->_eligibility++;
    setBinding(e->_cb, OpBufferOffset, stage, index, nil, offset);
}

- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    bindBufferOffset(self, 1, offset, index);
}

- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    bindBufferOffset(self, 2, offset, index);
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        bindBuffer(self, 1, b[k], o[k], r.location + k);
}

- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        bindBuffer(self, 2, b[k], o[k], r.location + k);
}

static gm20b::CheckedTexture checkedTexture(__unsafe_unretained SwitchMetalTexture *t)
{
    return t ? gm20b::CheckedTexture{t->_raw.addressMap != 0, t->_raw.xrPlanes} : gm20b::CheckedTexture{};
}

bool gm20bBindTexture(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, __unsafe_unretained id<MTLTexture> t,
                      NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained SwitchMetalTexture *x = gm20bAsTexture(t);
    if (x && x->_gm20bOk && x->_raw.rotation)
        SWITCH_REFUSE("gm20b", "tex.turned_bound",
                      "a %ux%u plane of rotation %u bound as a texture argument, read as it lies (MTLTexture.h "
                      "framebufferOnly: no texture argument)",
                      x->_raw.width, x->_raw.height, unsigned(x->_raw.rotation));
    if (t && !x) {
        if (!predLogged(PredTexForeign))
            logPred(PredTexForeign, t.pixelFormat, 0,
                    [NSString stringWithFormat:@"class %s", class_getName(object_getClass(t))]);
        hit(e->_cb, PredTexForeign, t.pixelFormat);
        failCommandBuffer(e->_cb, @"A texture not made by this device was bound: Metal's validation error",
                          MTLCommandBufferErrorInternal, PredTexForeign);
        return false;
    }
    __unsafe_unretained SwitchMetalCommandBuffer *cb = e->_cb;
    bool formsChanged = false;
    if ((stage == 1 || stage == 2) && index < gm20b::kStageTextures) {
        const gm20b::Pipeline *pl = e->_pipe ? e->_pipe->_raw : nullptr;
        SwitchMetalRenderSlots &slots = cb->_renderSlots;
        __unsafe_unretained SwitchMetalTexture *old = slots.tex[stage - 1][index];
        const bool oldPlanes = old && old->_raw.xrPlanes && !old->_raw.memoryless;
        const bool nextPlanes = x && x->_raw.xrPlanes && !x->_raw.memoryless;
        formsChanged = oldPlanes != nextPlanes;
        if (old != x && (gm20b::hasSlot(e->_cubicTextures[stage - 1], uint32_t(index)) ||
                         (stage == 2 && pl && gm20b::writtenSlot(pl->writtenSlots, uint32_t(index)))))
            formsChanged |= (old ? old->_descriptor.pixelFormat : 0) != (x ? x->_descriptor.pixelFormat : 0);
        if (formsChanged || gm20b::textureBindChecks(checkedTexture(old), checkedTexture(x), false, false))
            e->_eligibility++;
        slots.tex[stage - 1][index] = x;
        gm20b::addSlot(slots.texTouched[stage - 1], uint32_t(index));
    }
    if (x && (x->_raw.sampled || gm20b::xrPlanesPooled(x->_raw) || x->_raw.texelRows)) {
        gm20bPipelineStop(cb, VetoShadows);
        cb->_sampledShadows = true;
    }
    if (x && x->_raw.standIn) {
        gm20bPipelineStop(cb, VetoShadows);
        cb->_standIns = true;
    }
    if (x && x->_raw.xrPlanes)
        cb->_passes.own(e->_passAt).xrPlaneBinds = true;
    if (!cb->_commandLog && (stage == 1 || stage == 2)) {
        if (t)
            gm20bRetain(cb, t);
        cb->_plan.bindTexture(uint32_t(stage - 1), uint32_t(index), (__bridge void *)t);
        return formsChanged;
    }
    setBinding(cb, OpTexture, stage, index, t, 0);
    return formsChanged;
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    if (gm20bBindTexture(self, 2, t, index))
        [self prepareDrawnRecord];
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    if (gm20bBindTexture(self, 1, t, index))
        [self prepareDrawnRecord];
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindTexture(self, 2, t[k], r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindTexture(self, 1, t[k], r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

static bool sameCubicSampler(__unsafe_unretained SwitchMetalSampler *a, __unsafe_unretained SwitchMetalSampler *b)
{
    if (a == b)
        return true;
    const bool ac = a && a->_raw.desc.cubic, bc = b && b->_raw.desc.cubic;
    if (!ac || !bc)
        return ac == bc;
    const gm20b::SamplerDesc &x = a->_raw.desc, &y = b->_raw.desc;
    return x.sAddress == y.sAddress && x.tAddress == y.tAddress && x.rAddress == y.rAddress && x.pixel == y.pixel &&
           (a->_descriptor.borderColor & 3) == (b->_descriptor.borderColor & 3);
}

bool gm20bBindSampler(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage,
                      __unsafe_unretained id<MTLSamplerState> s, NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained SwitchMetalSampler *r = gm20bAsSampler(s);
    __unsafe_unretained SwitchMetalCommandBuffer *cb = e->_cb;
    SwitchMetalRenderSlots &slots = cb->_renderSlots;
    const uint16_t bit = uint16_t(index < 16 ? 1u << index : 0);
    const bool tracked = (stage == 1 || stage == 2) && bit;
    if (tracked) {
        // Core Animation binds the same sampler with each texture: the plan holds it bound already, and the
        // command buffer retained it
        if (const uint64_t epoch = cb->_plan.stateEpoch(); e->_sampEpoch != epoch) {
            e->_sampEpoch = epoch;
            e->_sampRecorded[0] = e->_sampRecorded[1] = 0;
        }
        if (r && slots.samp[stage - 1][index] == r && (e->_sampRecorded[stage - 1] & bit))
            return false;
    }
    if (s && !r) {
        hit(cb, PredCallSamplerForeign, index, 0, "a sampler state of another device: Metal's validation error");
        failCommandBuffer(cb, @"A sampler state not made by this device was bound: Metal's validation error",
                          MTLCommandBufferErrorInternal, PredCallSamplerForeign);
        return false;
    }
    if (r && r->_raw.desc.border)
        cb->_borderSamplers = true;
    bool formsChanged = false;
    if (tracked) {
        formsChanged = !sameCubicSampler(slots.samp[stage - 1][index], r) &&
                       ((e->_formSamplers[stage - 1] & bit) ||
                        !(e->_cubicMask[0] | e->_cubicMask[1] | e->_formSamplers[0] | e->_formSamplers[1]));
        if (formsChanged)
            e->_eligibility++;
        slots.samp[stage - 1][index] = r;
        slots.sampTouched[stage - 1] |= bit;
        auto &mask = e->_cubicMask[stage - 1];
        mask = uint16_t(r && r->_raw.desc.cubic ? mask | bit : mask & ~bit);
        auto &recorded = e->_sampRecorded[stage - 1];
        recorded = uint16_t(r ? recorded | bit : recorded & ~bit);
    }
    if (!cb->_commandLog && (stage == 1 || stage == 2)) {
        if (s)
            gm20bRetain(cb, s);
        cb->_plan.bindSampler(uint32_t(stage - 1), uint32_t(index), (__bridge void *)s);
        return formsChanged;
    }
    setBinding(cb, OpSampler, stage, index, s, 0);
    return formsChanged;
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    if (gm20bBindSampler(self, 2, s, index))
        [self prepareDrawnRecord];
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    if (gm20bBindSampler(self, 1, s, index))
        [self prepareDrawnRecord];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindSampler(self, 2, s[k], r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindSampler(self, 1, s[k], r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

// A bind's LOD clamps replace the state's (Metal's rule): the state of its descriptor with those clamps, made once per state and
// clamps, bound as any state is (the frontend's the same)
id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax)
{
    SwitchMetalSampler *base = gm20bAsSampler(s);
    if (!base)
        return s;
    uint32_t minBits = 0, maxBits = 0;
    memcpy(&minBits, &lodMin, sizeof minBits);
    memcpy(&maxBits, &lodMax, sizeof maxBits);
    auto findLocked = [&]() -> id<MTLSamplerState> {
        for (uint8_t i = 0; i < base->_clampedInlineCount; ++i)
            if (base->_clampedInline[i].lodMinBits == minBits && base->_clampedInline[i].lodMaxBits == maxBits)
                return base->_clampedInline[i].sampler;
        for (const SwitchMetalClampedSampler &entry : base->_clampedOverflow)
            if (entry.lodMinBits == minBits && entry.lodMaxBits == maxBits)
                return entry.sampler;
        return nil;
    };
    os_unfair_lock_lock(&base->_clampedLock);
    if (id<MTLSamplerState> made = findLocked()) {
        os_unfair_lock_unlock(&base->_clampedLock);
        return made;
    }
    os_unfair_lock_unlock(&base->_clampedLock);
    MTLSamplerDescriptor *d = [base->_descriptor copy];
    d.lodMinClamp = lodMin;
    d.lodMaxClamp = lodMax;
    id<MTLSamplerState> made = [(id<MTLDevice>)base->_device newSamplerStateWithDescriptor:d];
    if (!made)
        return nil;
    os_unfair_lock_lock(&base->_clampedLock);
    if (id<MTLSamplerState> existing = findLocked()) {
        os_unfair_lock_unlock(&base->_clampedLock);
        return existing;
    }
    if (base->_clampedInlineCount < base->_clampedInline.size())
        base->_clampedInline[base->_clampedInlineCount++] = {minBits, maxBits, made};
    else
        base->_clampedOverflow.push_back({minBits, maxBits, made});
    os_unfair_lock_unlock(&base->_clampedLock);
    return made;
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s
                  lodMinClamp:(float)lodMin
                  lodMaxClamp:(float)lodMax
                      atIndex:(NSUInteger)index
{
    if (gm20bBindSampler(self, 1, clampedSampler(s, lodMin, lodMax), index))
        [self prepareDrawnRecord];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s
                    lodMinClamp:(float)lodMin
                    lodMaxClamp:(float)lodMax
                        atIndex:(NSUInteger)index
{
    if (gm20bBindSampler(self, 2, clampedSampler(s, lodMin, lodMax), index))
        [self prepareDrawnRecord];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                    lodMinClamps:(const float[])lodMin
                    lodMaxClamps:(const float[])lodMax
                       withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindSampler(self, 2, clampedSampler(s[k], lodMin[k], lodMax[k]), r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                  lodMinClamps:(const float[])lodMin
                  lodMaxClamps:(const float[])lodMax
                     withRange:(NSRange)r
{
    bool changed = false;
    for (NSUInteger k = 0; k < r.length; k++)
        changed |= gm20bBindSampler(self, 1, clampedSampler(s[k], lodMin[k], lodMax[k]), r.location + k);
    if (changed)
        [self prepareDrawnRecord];
}

@end
