// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

@implementation RMRenderEncoder (Bindings)

// Room for `padded` bytes in the command buffer's setBytes chunks (RMBytes): false when only heap memory had it
static bool bytesRoom(RMBytes &s, uint32_t padded, uint32_t &chunk, uint32_t &offset)
{
    constexpr uint32_t kChunk = 64 << 10;  // a Core Animation frame's setBytes (~55 KB of 256-byte copies)
    while (s.at < s.chunks.size() && s.used + padded > s.chunks[s.at].length) {
        s.at++;
        s.used = 0;
    }
    if (s.at == s.chunks.size()) {
        raw::Buffer b;
        if (!gRM->raw->createBuffer(std::max(padded, kChunk), b, true)) {
            b = {};
            b.length = std::max(padded, kChunk);
            b.cpu = static_cast<uint8_t *>(malloc(size_t(b.length)));
        }
        s.chunks.push_back(b);
        s.used = 0;
    }
    chunk = uint32_t(s.at);
    offset = s.used;
    s.used += padded;
    return s.chunks[s.at].va != 0;
}

- (void)bytes:(int)stage data:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (stage >= 1 && stage <= 2 && index < 31) {
        _bufVa[stage][index] = 0;  // its own 256-byte-aligned upload chunk
        _buf[stage][index] = nil;
    }
    RMCommandBuffer *cb = _cb;
    Cmd m = cmd(OpBytes, uint8_t(stage), uint16_t(index));
    const uint32_t padded = std::max<uint32_t>((uint32_t(length) + 255) & ~255u, 256);
    uint32_t chunk, offset;
    if (!bytesRoom(cb->_bytes, padded, chunk, offset)) {
        [cb takeReason:WhyCall why:@"setBytes with no GPU pool memory for its bytes"];
        hit(cb, PredCallBytes, length);
    }
    uint8_t *to = cb->_bytes.chunks[chunk].cpu + offset;
    memcpy(to, bytes, length);
    memset(to + length, 0, padded - length);
    m.a = offset;
    m.b = uint32_t(length);
    m.c = chunk;
    [cb record:m];
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:1 data:bytes length:length index:index];
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:2 data:bytes length:length index:index];
}

- (void)buffer:(int)stage object:(id<MTLBuffer>)b offset:(NSUInteger)offset index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (b && ![(id)b isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCall why:@"a buffer not made by this device"];
        hit(_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length);
    }
    if (stage >= 1 && stage <= 2 && index < 31) {
        RMBuffer *x = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
        _buf[stage][index] = x;
        _bufVa[stage][index] = x ? x->_raw.va + offset : 0;
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
    if (index < 31 && _buf[stage][index])
        _bufVa[stage][index] = _buf[stage][index]->_raw.va + offset;
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

- (void)texture:(int)stage object:(id<MTLTexture>)t index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    // an extended-range texture is sampled through its stand-in (raw_xr.h): one without (no ShaderRead usage) is m2v's to sample
    const bool unsampled = x && !x->_raw.standIn && raw::xrForm(x->_raw.format) != raw::XrForm::None;
    if (t && (!x || !x->_rawOk || !x->_raw.tic || unsampled)) {
        [_cb takeReason:WhyTexture why:@"a texture the raw path does not keep"];
        if (!x) {
            if (!predLogged(PredTexForeign))
                logPred(PredTexForeign, t.pixelFormat, 0,
                        [NSString stringWithFormat:@"class %s", class_getName(object_getClass(t))]);
            hit(_cb, PredTexForeign, t.pixelFormat);
        } else
            hit(_cb, RMPred(x->_pred ?: (x->_rawOk ? PredEncNoTic : PredTexShape)), x->_descriptor.pixelFormat,
                x->_kind);
    }
    if (stage == 2 && index < 16)
        _tex[index] = x;
    if (x && x->_raw.sampled)
        _cb->_sampledShadows = true;
    if (x && x->_raw.standIn)
        _cb->_standIns = true;
    setBinding(_cb, OpTexture, stage, index, t, 0);
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:2 object:t index:index];
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:1 object:t index:index];
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self texture:2 object:t[k] index:r.location + k];
}

- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self texture:1 object:t[k] index:r.location + k];
}

- (void)sampler:(int)stage object:(id<MTLSamplerState>)s index:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (s && ![(id)s isKindOfClass:[RMSampler class]]) {
        [_cb takeReason:WhyCall why:@"a sampler not made by this device"];
        hit(_cb, PredCallSamplerForeign, index);
    }
    if (stage == 1 && s) {
        [_cb takeReason:WhyCall why:@"a vertex sampler"];
        hit(_cb, PredCallVertexSampler, index);
    }
    setBinding(_cb, OpSampler, stage, index, s, 0);
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:2 object:s index:index];
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:1 object:s index:index];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self sampler:2 object:s[k] index:r.location + k];
}

// A bind's LOD clamps replace the state's (Metal's rule): the state of its descriptor with those clamps, made once per state and
// clamps, bound as any state is (the frontend's the same)
static id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax)
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
    [self sampler:1 object:clampedSampler(s, lodMin, lodMax) index:index];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s
                    lodMinClamp:(float)lodMin
                    lodMaxClamp:(float)lodMax
                        atIndex:(NSUInteger)index
{
    [self sampler:2 object:clampedSampler(s, lodMin, lodMax) index:index];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                    lodMinClamps:(const float[])lodMin
                    lodMaxClamps:(const float[])lodMax
                       withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self sampler:2 object:clampedSampler(s[k], lodMin[k], lodMax[k]) index:r.location + k];
}

@end
