// SPDX-License-Identifier: MIT
// The binding forms of a per-bind stride (setVertexBuffer:offset:attributeStride:atIndex: and the compute encoder's), which a
// layout of MTLBufferLayoutStrideDynamic reads, and the compute encoder's stage-in region, which each dispatch records. Each records its binding command with the stride in its x (stride + 1; 0 is
// MTLAttributeStrideStatic, no stride: raw_semantic_setters.cpp bindStride).
#import "RawMetalInternal.h"

static uint64_t strideWord(NSUInteger stride)
{
    return stride >= UINT32_MAX ? 0 : uint64_t(stride) + 1;
}

static void stridedBinding(RMCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t offset,
                           NSUInteger stride)
{
    if (object)
        [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = offset;
    m.x = strideWord(stride);
    [cb record:m];
}

@interface RMRenderEncoder (Strides)
@end
@implementation RMRenderEncoder (Strides)

- (void)setVertexBuffer:(id<MTLBuffer>)b
                 offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride
                atIndex:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    RMBuffer *x = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
    if (b && !x) {
        [_cb takeReason:WhyCall why:@"a buffer not made by this device"];
        hit(_cb, PredCallBufForeign, uint64_t(1) << 8 | index, b.length);
    }
    if (index < 31) {
        _buf[1][index] = x;
        _bufVa[1][index] = x ? x->_raw.va + offset : 0;
    }
    stridedBinding(_cb, OpBuffer, 1, index, b, offset, stride);
}

- (void)setVertexBufferOffset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (index < 31 && _buf[1][index])
        _bufVa[1][index] = _buf[1][index]->_raw.va + offset;
    stridedBinding(_cb, OpBufferOffset, 1, index, nil, offset, stride);
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                 offsets:(const NSUInteger[])o
        attributeStrides:(const NSUInteger[])strides
               withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setVertexBuffer:b[k] offset:o[k] attributeStride:strides[k] atIndex:r.location + k];
}

- (void)setVertexBytes:(const void *)bytes
                length:(NSUInteger)length
       attributeStride:(NSUInteger)stride
               atIndex:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (index < 31) {
        _bufVa[1][index] = 0;
        _buf[1][index] = nil;
    }
    Cmd m = cmd(OpBytes, 1, uint16_t(index));
    m.x = strideWord(stride);
    recordBytes(_cb, m, bytes, length);
}

@end

@interface RMComputeEncoder (Strides)
@end
@implementation RMComputeEncoder (Strides)

- (void)setStageInRegion:(MTLRegion)region
{
    _stageIn = region;
}

- (void)setBuffer:(id<MTLBuffer>)buffer
             offset:(NSUInteger)offset
    attributeStride:(NSUInteger)stride
            atIndex:(NSUInteger)index
{
    if (buffer && ![(id)buffer isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCompute why:@"a kernel's buffer not made by this device"];
        hit(_cb, PredCompute, index);
    }
    stridedBinding(_cb, OpKernelBuffer, 3, index, buffer, offset, stride);
}

- (void)setBufferOffset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index
{
    stridedBinding(_cb, OpKernelBufferOffset, 3, index, nil, offset, stride);
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers
             offsets:(const NSUInteger[])offsets
    attributeStrides:(const NSUInteger[])strides
           withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setBuffer:buffers[k] offset:offsets[k] attributeStride:strides[k] atIndex:range.location + k];
}

- (void)setBytes:(const void *)bytes
             length:(NSUInteger)length
    attributeStride:(NSUInteger)stride
            atIndex:(NSUInteger)index
{
    Cmd m = cmd(OpKernelBytes, 3, uint16_t(index));
    m.x = strideWord(stride);
    recordBytes(_cb, m, bytes, length);
}

@end
