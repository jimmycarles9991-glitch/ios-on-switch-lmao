// SPDX-License-Identifier: MIT
// The binding forms of a per-bind stride (setVertexBuffer:offset:attributeStride:atIndex: and the compute encoder's), which a
// layout of MTLBufferLayoutStrideDynamic reads, and the compute encoder's stage-in region, which each dispatch records. Each records its binding command with the stride in its x (stride + 1; 0 is
// MTLAttributeStrideStatic, no stride: semantic_setters.cpp bindStride).
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalIndirectCompute.h"

static uint64_t strideWord(NSUInteger stride)
{
    return stride >= UINT32_MAX ? 0 : uint64_t(stride) + 1;
}

static void stridedBinding(SwitchMetalCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t offset,
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

@interface SwitchMetalRenderEncoder (Strides)
@end
@implementation SwitchMetalRenderEncoder (Strides)

- (void)setVertexBuffer:(id<MTLBuffer>)b
                 offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride
                atIndex:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    SwitchMetalBuffer *x = [(id)b isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)b : nil;
    if (b && !x)
        return gm20bCallInvalid(_cb, PredCallBufForeign, uint64_t(1) << 8 | index, b.length,
                                "a buffer not made by this device");
    if (index < 31) {
        _buf[1][index] = x;
        _bufVa[1][index] = x ? x->_raw.va + offset : 0;
        _eligibility++;
    }
    stridedBinding(_cb, OpBuffer, 1, index, b, offset, stride);
}

- (void)setVertexBufferOffset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index
{
    SWITCH_API(ApiBind);
    if (index < 31 && _buf[1][index]) {
        _bufVa[1][index] = _buf[1][index]->_raw.va + offset;
        _eligibility++;
    }
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

@interface SwitchMetalComputeEncoder (Strides)
@end
@implementation SwitchMetalComputeEncoder (Strides)

- (void)setStageInRegion:(MTLRegion)region
{
    _stageIn = region;
    _stageInBuffer = nil;
}

- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset
{
    SwitchMetalBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)buffer : nil;
    if (!b || (offset & 3) || offset > b->_raw.length || b->_raw.length - offset < 24)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, offset,
                                 "a stage-in region buffer of another device, off 4 bytes or the region past it");
    [_cb retain:buffer];
    _stageInBuffer = b;
    _stageInOffset = offset;
}

- (void)setBuffer:(id<MTLBuffer>)buffer
             offset:(NSUInteger)offset
    attributeStride:(NSUInteger)stride
            atIndex:(NSUInteger)index
{
    if (buffer && ![(id)buffer isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, index,
                                 "a kernel's buffer of another device");
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
