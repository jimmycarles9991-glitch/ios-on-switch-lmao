// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_draw_check.h"

@implementation RMRenderEncoder (Bindings)

// Room for `padded` bytes in the command buffer's setBytes chunks (RMBytes): false when only heap memory had it
static bool bytesRoom(RMBytes &s, uint32_t padded, uint32_t &chunk, uint32_t &offset)
{
    while (s.at < s.chunks.size() && s.used + padded > s.chunks.own(s.at).length) {
        s.at++;
        s.used = 0;
    }
    if (s.at == s.chunks.size()) {
        raw::Buffer b;
        if (!rmTakeBytesChunk(std::max(padded, kRMBytesChunk), b)) {
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

void recordBytes(__unsafe_unretained RMCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length, bool shadow)
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
            os_log_error(rawLog(),
                         "raw: setBytes of %lu bytes: no GPU memory for its chunk (%{public}s): its command "
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

static void bindBytes(__unsafe_unretained RMRenderEncoder *e, int stage, const void *bytes, NSUInteger length,
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
    if (b && ![(id)b isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCall why:@"a buffer not made by this device"];
        hit(_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length);
    }
    if (stage >= 1 && stage <= 2 && index < 31) {
        __unsafe_unretained RMBuffer *x = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
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

static raw::CheckedTexture checkedTexture(__unsafe_unretained RMTexture *t)
{
    return t ? raw::CheckedTexture{t->_raw.addressMap != 0, t->_raw.xrPlanes} : raw::CheckedTexture{};
}

void rmBindTexture(__unsafe_unretained RMRenderEncoder *e, int stage, __unsafe_unretained id<MTLTexture> t,
                   NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    // an extended-range texture is sampled through its stand-in (raw_xr.h): one without (no ShaderRead usage) is m2v's to sample
    const bool unsampled = x && !x->_raw.standIn && raw::xrForm(x->_raw.format) != raw::XrForm::None;
    if (t && (!x || !x->_rawOk || !x->_raw.tic || unsampled || x->_raw.rotation)) {
        [e->_cb takeReason:WhyTexture why:@"a texture the raw path does not keep"];
        if (!x) {
            if (!predLogged(PredTexForeign))
                logPred(PredTexForeign, t.pixelFormat, 0,
                        [NSString stringWithFormat:@"class %s", class_getName(object_getClass(t))]);
            hit(e->_cb, PredTexForeign, t.pixelFormat);
        } else
            hit(e->_cb, RMPred(x->_pred ?: (x->_rawOk && !x->_raw.rotation ? PredEncNoTic : PredTexShape)),
                x->_descriptor.pixelFormat, x->_kind);
    }
    if ((stage == 1 || stage == 2) && index < raw::kStageTextures) {
        const raw::Pipeline *pl = e->_pipe ? e->_pipe->_raw : nullptr;
        if (raw::textureBindChecks(checkedTexture(e->_tex[stage - 1][index]), checkedTexture(x), e->_cubicMask,
                                   pl && pl->writtenSlots))
            e->_eligibility++;
        e->_tex[stage - 1][index] = x;
    }
    if (x && x->_raw.sampled) {
        rmPipelineVeto(e->_cb, VetoShadows);
        e->_cb->_sampledShadows = true;
    }
    if (x && x->_raw.standIn) {
        rmPipelineVeto(e->_cb, VetoShadows);
        e->_cb->_standIns = true;
    }
    setBinding(e->_cb, OpTexture, stage, index, t, 0);
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    rmBindTexture(self, 2, t, index);
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    rmBindTexture(self, 1, t, index);
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindTexture(self, 2, t[k], r.location + k);
}

- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindTexture(self, 1, t[k], r.location + k);
}

void rmBindSampler(__unsafe_unretained RMRenderEncoder *e, int stage, __unsafe_unretained id<MTLSamplerState> s,
                   NSUInteger index)
{
    SWITCH_API(ApiBind);
    __unsafe_unretained RMSampler *r = [(id)s isKindOfClass:[RMSampler class]] ? (RMSampler *)s : nil;
    const uint16_t bit = uint16_t(index < 16 ? 1u << index : 0);
    if (stage == 2 && bit) {
        // Core Animation binds the same sampler with each texture: the plan holds it bound already, and the
        // command buffer retained it
        if (const uint64_t epoch = e->_cb->_plan.stateEpoch(); e->_sampEpoch != epoch) {
            e->_sampEpoch = epoch;
            e->_sampRecorded = 0;
        }
        if (r && e->_samp[index] == r && (e->_sampRecorded & bit))
            return;
    }
    if (s && !r) {
        [e->_cb takeReason:WhyCall why:@"a sampler not made by this device"];
        hit(e->_cb, PredCallSamplerForeign, index);
    }
    // m2v filters through a cubic sampler in the shader, which only the fragment stage's records have variants of
    // (RawMetalCubic.mm)
    if (stage == 1 && r && r->_raw.desc.cubic) {
        [e->_cb takeReason:WhyCall why:@"a cubic vertex sampler"];
        hit(e->_cb, PredCallVertexSampler, index);
    }
    if (r && r->_raw.desc.border)
        e->_cb->_borderSamplers = true;
    if (stage == 2 && bit) {
        if (raw::samplerBindChecks(e->_cubicMask & bit, r && r->_raw.desc.cubic))
            e->_eligibility++;
        e->_samp[index] = r;
        e->_cubicMask = uint16_t(r && r->_raw.desc.cubic ? e->_cubicMask | bit : e->_cubicMask & ~bit);
        e->_sampRecorded = uint16_t(r ? e->_sampRecorded | bit : e->_sampRecorded & ~bit);
    }
    setBinding(e->_cb, OpSampler, stage, index, s, 0);
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    rmBindSampler(self, 2, s, index);
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    rmBindSampler(self, 1, s, index);
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindSampler(self, 2, s[k], r.location + k);
}

- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindSampler(self, 1, s[k], r.location + k);
}

// A bind's LOD clamps replace the state's (Metal's rule): the state of its descriptor with those clamps, made once per state and
// clamps, bound as any state is (the frontend's the same)
id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax)
{
    if (![(id)s isKindOfClass:[RMSampler class]])
        return s;
    RMSampler *base = (RMSampler *)s;
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
    rmBindSampler(self, 1, clampedSampler(s, lodMin, lodMax), index);
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s
                    lodMinClamp:(float)lodMin
                    lodMaxClamp:(float)lodMax
                        atIndex:(NSUInteger)index
{
    rmBindSampler(self, 2, clampedSampler(s, lodMin, lodMax), index);
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                    lodMinClamps:(const float[])lodMin
                    lodMaxClamps:(const float[])lodMax
                       withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindSampler(self, 2, clampedSampler(s[k], lodMin[k], lodMax[k]), r.location + k);
}

- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                  lodMinClamps:(const float[])lodMin
                  lodMaxClamps:(const float[])lodMax
                     withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        rmBindSampler(self, 1, clampedSampler(s[k], lodMin[k], lodMax[k]), r.location + k);
}

@end
