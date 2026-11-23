// SPDX-License-Identifier: MIT
// An indirect command of patches (icb.h icb::patches): its factor, patch index and control-point index buffers'
// descriptors and its control stage's push words are its slot's own, written as the CPU encodes it, and loaded by the GPU
// for the state an execute inherits. Each of AGXA13FamilyIndirectRenderCommand's four forms (0x18c1940ac, 0x18c194028,
// 0x18c1940ec, 0x18c194064): a form without a factor scale draws at 1, as m2v's command records it. The control points
// are the inherited state's, whatever the call names (m2v's measurement of Metal's execution). On a buffer whose
// descriptor has no patch commands there is no such buffer.
#import "TegraGM20BMetalIndirect.h"

namespace {

SwitchMetalBuffer *own(id<MTLBuffer> buffer)
{
    return [(id)buffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)buffer : nil;
}

}  // namespace

@interface SwitchMetalIndirectRenderCommand (Patches)
@end
@implementation SwitchMetalIndirectRenderCommand (Patches)

- (void)patches:(const gm20b::RenderEncoder::IcbPatches &)in
        factors:(SwitchMetalBuffer *)factors
        indices:(SwitchMetalBuffer *)patchIndices
         points:(SwitchMetalBuffer *)pointIndices
{
    // an M4 Max's driver faults encoding them
    if (!_buffer->_patches)
        return [self invalidate:PredCallIcb why:"an indirect command of patches on a buffer of no patch commands"];
    SwitchMetalICBCommand c;
    const char *why = nullptr;
    if (!gm20b::RenderEncoder::icbPatchDraw(*gRM->raw, in, c.patches, &why)) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.patches", uintptr_t(why), "indirect command %lu: %s: none drawn",
                             (unsigned long)_index, why);
        [self reset];
        return;
    }
    c.kind = SwitchMetalICBCommand::Patches;
    c.primitive = uint8_t(gm20b::icb::kPatchPrimitive);
    c.factors = factors;
    c.patchIndices = patchIndices;
    c.pointIndices = pointIndices;
    gm20b::icb::patches(gm20bCommandSlot(_buffer, _index), uint32_t(_index), c.patches);
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
    SwitchMetalBuffer *f = own(factors), *p = own(patchIndices), *q = own(pointIndices);
    if ((factors && !f) || (patchIndices && !p) || (pointIndices && !q))
        return [self invalidate:PredCallBufForeign
                            why:"an indirect command of patches' buffer not made by this device"];
    const gm20b::RenderEncoder::IcbPatches in = {
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
