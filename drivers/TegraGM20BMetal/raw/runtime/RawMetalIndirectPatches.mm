// SPDX-License-Identifier: MIT
// An indirect command of patches (raw_icb.h icb::patches): its factor, patch index and control-point index buffers'
// descriptors and its control stage's push words are its slot's own, written as the CPU encodes it, and loaded by the GPU
// for the state an execute inherits. Each of AGXA13FamilyIndirectRenderCommand's four forms (0x18c1940ac, 0x18c194028,
// 0x18c1940ec, 0x18c194064): a form without a factor scale draws at 1, as m2v's command records it. The control points
// are the inherited state's, whatever the call names (m2v's measurement of Metal's execution). On a buffer whose
// descriptor has no patch commands, or a device that answers supportsIndirectTessellation NO, there is no such buffer.
#import "RawMetalIndirect.h"

namespace {

RMBuffer *own(id<MTLBuffer> buffer)
{
    return [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
}

}  // namespace

@interface RMIndirectRenderCommand (Patches)
@end
@implementation RMIndirectRenderCommand (Patches)

- (void)patches:(const raw::RenderEncoder::IcbPatches &)in
        factors:(RMBuffer *)factors
        indices:(RMBuffer *)patchIndices
         points:(RMBuffer *)pointIndices
{
    if (!_buffer->_patches) {
        SWITCH_REFUSE("raw", "icb.patches", "indirect command %lu: patches on a buffer of no patch commands",
                      (unsigned long)_index);
        return;
    }
    RMICBCommand c;
    const char *why = nullptr;
    if (!raw::RenderEncoder::icbPatchDraw(*gRM->raw, in, c.patches, &why)) {
        SWITCH_REFUSE_DETAIL("raw", "icb.patches", uintptr_t(why), "indirect command %lu: %s: none drawn",
                             (unsigned long)_index, why);
        [self reset];
        return;
    }
    c.kind = RMICBCommand::Patches;
    c.primitive = uint8_t(raw::icb::kPatchPrimitive);
    c.factors = factors;
    c.patchIndices = patchIndices;
    c.pointIndices = pointIndices;
    raw::icb::patches(rmCommandSlot(_buffer, _index), uint32_t(_index), c.patches);
    [_buffer take:c at:_index];
}

- (void)patchesFrom:(NSUInteger)start
               count:(NSUInteger)count
           instances:(NSUInteger)instances
        baseInstance:(NSUInteger)baseInstance
        patchIndices:(id<MTLBuffer>)patchIndices
    patchIndexOffset:(NSUInteger)patchIndexOffset
        pointIndices:(id<MTLBuffer>)pointIndices
    pointIndexOffset:(NSUInteger)pointIndexOffset
             factors:(id<MTLBuffer>)factors
        factorOffset:(NSUInteger)factorOffset
        factorStride:(NSUInteger)factorStride
               scale:(float)scale
{
    RMBuffer *f = own(factors), *p = own(patchIndices), *q = own(pointIndices);
    if ((factors && !f) || (patchIndices && !p) || (pointIndices && !q)) {
        SWITCH_REFUSE("raw", "icb.patches", "indirect command %lu: a buffer not this device's: none drawn",
                      (unsigned long)_index);
        [self reset];
        return;
    }
    const raw::RenderEncoder::IcbPatches in = {
        uint32_t(start),        uint32_t(count),  uint32_t(instances),    uint32_t(baseInstance),
        f ? &f->_raw : nullptr, factorOffset,     factorStride,           scale,
        p ? &p->_raw : nullptr, patchIndexOffset, q ? &q->_raw : nullptr, pointIndexOffset};
    [self patches:in factors:f indices:p points:q];
}

- (void)drawPatches:(NSUInteger)controlPoints
                                patchStart:(NSUInteger)start
                                patchCount:(NSUInteger)count
                          patchIndexBuffer:(id<MTLBuffer>)patchIndices
                    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
                             instanceCount:(NSUInteger)instances
                              baseInstance:(NSUInteger)baseInstance
                  tessellationFactorBuffer:(id<MTLBuffer>)factors
            tessellationFactorBufferOffset:(NSUInteger)factorOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)factorStride
{
    (void)controlPoints;
    [self patchesFrom:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:nil
        pointIndexOffset:0
                 factors:factors
            factorOffset:factorOffset
            factorStride:factorStride
                   scale:1.0f];
}

- (void)drawPatches:(NSUInteger)controlPoints
                                patchStart:(NSUInteger)start
                                patchCount:(NSUInteger)count
                          patchIndexBuffer:(id<MTLBuffer>)patchIndices
                    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
                             instanceCount:(NSUInteger)instances
                              baseInstance:(NSUInteger)baseInstance
                  tessellationFactorBuffer:(id<MTLBuffer>)factors
            tessellationFactorBufferOffset:(NSUInteger)factorOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)factorStride
             tessellationFactorBufferScale:(float)scale
{
    (void)controlPoints;
    [self patchesFrom:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:nil
        pointIndexOffset:0
                 factors:factors
            factorOffset:factorOffset
            factorStride:factorStride
                   scale:scale];
}

- (void)drawIndexedPatches:(NSUInteger)controlPoints
                                patchStart:(NSUInteger)start
                                patchCount:(NSUInteger)count
                          patchIndexBuffer:(id<MTLBuffer>)patchIndices
                    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
                   controlPointIndexBuffer:(id<MTLBuffer>)pointIndices
             controlPointIndexBufferOffset:(NSUInteger)pointIndexOffset
                             instanceCount:(NSUInteger)instances
                              baseInstance:(NSUInteger)baseInstance
                  tessellationFactorBuffer:(id<MTLBuffer>)factors
            tessellationFactorBufferOffset:(NSUInteger)factorOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)factorStride
{
    (void)controlPoints;
    [self patchesFrom:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:pointIndices
        pointIndexOffset:pointIndexOffset
                 factors:factors
            factorOffset:factorOffset
            factorStride:factorStride
                   scale:1.0f];
}

- (void)drawIndexedPatches:(NSUInteger)controlPoints
                                patchStart:(NSUInteger)start
                                patchCount:(NSUInteger)count
                          patchIndexBuffer:(id<MTLBuffer>)patchIndices
                    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
                   controlPointIndexBuffer:(id<MTLBuffer>)pointIndices
             controlPointIndexBufferOffset:(NSUInteger)pointIndexOffset
                             instanceCount:(NSUInteger)instances
                              baseInstance:(NSUInteger)baseInstance
                  tessellationFactorBuffer:(id<MTLBuffer>)factors
            tessellationFactorBufferOffset:(NSUInteger)factorOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)factorStride
             tessellationFactorBufferScale:(float)scale
{
    (void)controlPoints;
    [self patchesFrom:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:pointIndices
        pointIndexOffset:pointIndexOffset
                 factors:factors
            factorOffset:factorOffset
            factorStride:factorStride
                   scale:scale];
}

@end
