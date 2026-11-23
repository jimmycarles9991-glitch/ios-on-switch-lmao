// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "bound_texture.h"
#include "draw_check.h"
#include "xr_pools.h"

@implementation SwitchMetalRenderEncoder (Bindings)

// Room for `padded` bytes in the command buffer's setBytes chunks (SwitchMetalBytes): false when only heap memory had it
static bool bytesRoom(SwitchMetalBytes &s, uint32_t padded, uint32_t &chunk, uint32_t &offset)
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
    return s.chunks.own(s.at).va != 0;
}

void recordBytes(__unsafe_unretained SwitchMetalCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length,
                 bool shadow)
{
    constexpr NSUInteger kShadowed = 1024;  // a Core Animation draw's vertices and its transform
    if (shadow && length <= kShadowed) {
        m.shadow = uint32_t(cb->_shadow.size() + 1);
        cb->_shadow.append(static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length);
    }
    const uint32_t padded = std::max<uint32_t>((uint32_t(length) + 255) & ~255u, 256);
    uint32_t chunk, offset;
    // Out of memory as Metal reports it: the command buffer fails (its bytes go to the heap meanwhile, so recording goes
    // on), never another backend
    if (!bytesRoom(cb->_bytes, padded, chunk, offset)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log_error(gm20bLog(),
                         "SwitchMetal: setBytes of %lu bytes: no GPU memory for its chunk (%{public}s): its command "
                         "buffer fails out of memory; later ones not logged",
                         (unsigned long)length, gRM->raw->kernel().error().c_str());
        failCommandBuffer(cb, @"no GPU memory for setBytes", MTLCommandBufferErrorOutOfMemory);
    }
    uint8_t *to = cb->_bytes.chunks.own(chunk).cpu + offset;
    memcpy(to, bytes, length);
    memset(to + length, 0, padded - length);
    m.a = offset;
    m.b = uint32_t(length);
    m.c = chunk;
    [cb recordBinding:m];
}

static void bindBytes(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, const void *bytes, NSUInteger length,
                      NSUInteger index)
{
    SWITCH_API(ApiBind);
    if (stage >= 1 && stage <= 2 && index < 31) {
        e->_bufVa[stage][index] = 0;  // its own 256-byte-aligned upload chunk
        e->_buf[stage][index] = nil;
    }
    recordBytes(e->_cb, cmd(OpBytes, uint8_t(stage), uint16_t(index)), bytes, length, stage == 1 && !e->_drew);
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    bindBytes(self, 1, bytes, length, index);
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    bindBytes(self, 2, bytes, length, index);
}

- (void)buffer:(int)stage object:(id<MTLBuffer>)b offset:(NSUInteger)offset index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (b && ![(id)b isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bCallInvalid(_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length,
                                "a buffer not made by this device");
    if (stage >= 1 && stage <= 2 && index < 31) {
        __unsafe_unretained SwitchMetalBuffer *x = (SwitchMetalBuffer *)b;
        _buf[stage][index] = x;
        _bufVa[stage][index] = x ? x->_raw.va + offset : 0;
        _eligibility++;
    }
    setBinding(_cb, OpBuffer, stage, index, b, offset);
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:1 object:b offset:offset index:index];
}

- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:2 object:b offset:offset index:index];
}

- (void)bufferOffset:(int)stage offset:(NSUInteger)offset index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (index < 31 && _buf[stage][index]) {
        _bufVa[stage][index] = _buf[stage][index]->_raw.va + offset;
        _eligibility++;
    }
    setBinding(_cb, OpBufferOffset, stage, index, nil, offset);
}

- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:1 offset:offset index:index];
}

- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:2 offset:offset index:index];
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:1 object:b[k] offset:o[k] index:r.location + k];
}

- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:2 object:b[k] offset:o[k] index:r.location + k];
}

static gm20b::CheckedTexture checkedTexture(__unsafe_unretained SwitchMetalTexture *t)
{
    return t ? gm20b::CheckedTexture{t->_raw.addressMap != 0, t->_raw.xrPlanes} : gm20b::CheckedTexture{};
}

bool gm20bBindTexture(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, __unsafe_unretained id<MTLTexture> t,
                      NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained SwitchMetalTexture *x =
        [(id)t isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)t : nil;
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
    bool formsChanged = false;
    if ((stage == 1 || stage == 2) && index < gm20b::kStageTextures) {
        const gm20b::Pipeline *pl = e->_pipe ? e->_pipe->_raw : nullptr;
        __unsafe_unretained SwitchMetalTexture *old = e->_tex[stage - 1][index];
        const bool oldPlanes = old && old->_raw.xrPlanes && !old->_raw.memoryless;
        const bool nextPlanes = x && x->_raw.xrPlanes && !x->_raw.memoryless;
        formsChanged = oldPlanes != nextPlanes;
        if (old != x && (gm20b::hasSlot(e->_cubicTextures[stage - 1], uint32_t(index)) ||
                         (stage == 2 && pl && gm20b::writtenSlot(pl->writtenSlots, uint32_t(index)))))
            formsChanged |= (old ? old->_descriptor.pixelFormat : 0) != (x ? x->_descriptor.pixelFormat : 0);
        if (formsChanged || gm20b::textureBindChecks(checkedTexture(old), checkedTexture(x), false, false))
            e->_eligibility++;
        e->_tex[stage - 1][index] = x;
    }
    if (x && (x->_raw.sampled || gm20b::xrPlanesPooled(x->_raw) || x->_raw.texelRows)) {
        gm20bPipelineVeto(e->_cb, VetoShadows);
        e->_cb->_sampledShadows = true;
    }
    if (x && x->_raw.standIn) {
        gm20bPipelineVeto(e->_cb, VetoShadows);
        e->_cb->_standIns = true;
    }
    if (x && x->_raw.xrPlanes)
        e->_cb->_passes[e->_passAt].xrPlaneBinds = true;
    setBinding(e->_cb, OpTexture, stage, index, t, 0);
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
    __unsafe_unretained SwitchMetalSampler *r =
        [(id)s isKindOfClass:[SwitchMetalSampler class]] ? (SwitchMetalSampler *)s : nil;
    const uint16_t bit = uint16_t(index < 16 ? 1u << index : 0);
    const bool tracked = (stage == 1 || stage == 2) && bit;
    if (tracked) {
        // Core Animation binds the same sampler with each texture: the plan holds it bound already, and the
        // command buffer retained it
        if (const uint64_t epoch = e->_cb->_plan.stateEpoch(); e->_sampEpoch != epoch) {
            e->_sampEpoch = epoch;
            e->_sampRecorded[0] = e->_sampRecorded[1] = 0;
        }
        if (r && e->_samp[stage - 1][index] == r && (e->_sampRecorded[stage - 1] & bit))
            return false;
    }
    if (s && !r) {
        hit(e->_cb, PredCallSamplerForeign, index, 0, "a sampler state of another device: Metal's validation error");
        failCommandBuffer(e->_cb, @"A sampler state not made by this device was bound: Metal's validation error",
                          MTLCommandBufferErrorInternal, PredCallSamplerForeign);
        return false;
    }
    if (r && r->_raw.desc.border)
        e->_cb->_borderSamplers = true;
    bool formsChanged = false;
    if (tracked) {
        formsChanged = !sameCubicSampler(e->_samp[stage - 1][index], r) &&
                       ((e->_formSamplers[stage - 1] & bit) ||
                        !(e->_cubicMask[0] | e->_cubicMask[1] | e->_formSamplers[0] | e->_formSamplers[1]));
        if (formsChanged)
            e->_eligibility++;
        e->_samp[stage - 1][index] = r;
        auto &mask = e->_cubicMask[stage - 1];
        mask = uint16_t(r && r->_raw.desc.cubic ? mask | bit : mask & ~bit);
        auto &recorded = e->_sampRecorded[stage - 1];
        recorded = uint16_t(r ? recorded | bit : recorded & ~bit);
    }
    setBinding(e->_cb, OpSampler, stage, index, s, 0);
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
    if (![(id)s isKindOfClass:[SwitchMetalSampler class]])
        return s;
    SwitchMetalSampler *base = (SwitchMetalSampler *)s;
    const float clamps[2] = {lodMin, lodMax};
    NSValue *key = [NSValue valueWithBytes:clamps objCType:@encode(float[2])];
    @synchronized(base) {
        if (!base->_clamped)
            base->_clamped = [NSMutableDictionary dictionary];
        if (id<MTLSamplerState> made = base->_clamped[key])
            return made;
        MTLSamplerDescriptor *d = [base->_descriptor copy];
        d.lodMinClamp = lodMin;
        d.lodMaxClamp = lodMax;
        id<MTLSamplerState> made = [(id<MTLDevice>)base->_device newSamplerStateWithDescriptor:d];
        if (made)
            base->_clamped[key] = made;
        return made;
    }
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
