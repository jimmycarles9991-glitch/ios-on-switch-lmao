// SPDX-License-Identifier: MIT
// A render encoder's patch draws (raw_tessellation.cpp draws them): each records the encoder's factor buffer and scale with
// it, so a draw's plan holds everything its control stage reads.
#import "RawMetalInternal.h"
#include <cstring>
#include <functional>
#include "raw_icb.h"

static RMBuffer *ownBuffer(RMCommandBuffer *cb, id<MTLBuffer> buffer)
{
    if (!buffer)
        return nil;
    if (![(id)buffer isKindOfClass:[RMBuffer class]]) {
        [cb takeReason:WhyCall why:@"a patch draw's buffer not made by this device"];
        hit(cb, PredCallBufForeign, 0xffff, 0);
        return nil;
    }
    [cb retain:buffer];
    return (RMBuffer *)buffer;
}

@interface RMRenderEncoder (Tessellation)
@end
@implementation RMRenderEncoder (Tessellation)

- (void)setTessellationFactorBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset instanceStride:(NSUInteger)stride
{
    _tessFactors = [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
    if (buffer && !_tessFactors) {
        [_cb takeReason:WhyCall why:@"a tessellation factor buffer not made by this device"];
        hit(_cb, PredCallBufForeign, 0xffff, 0);
    }
    _tessFactorOffset = offset;
    _tessFactorStride = stride;
}

- (void)setTessellationFactorScale:(float)scale
{
    _tessScale = scale;
    _tessScaleSet = YES;
}

- (void)recordPatches:(Op)op
        controlPoints:(NSUInteger)controlPoints
                start:(NSUInteger)start
                count:(NSUInteger)count
            instances:(NSUInteger)instances
         baseInstance:(NSUInteger)baseInstance
         patchIndices:(id<MTLBuffer>)patchIndices
     patchIndexOffset:(NSUInteger)patchIndexOffset
         pointIndices:(id<MTLBuffer>)pointIndices
     pointIndexOffset:(NSUInteger)pointIndexOffset
            arguments:(id<MTLBuffer>)arguments
       argumentOffset:(NSUInteger)argumentOffset
{
    SWITCH_API(ApiDraw);
    [self checkDraw];
    if (_tessFactors)
        [_cb retain:_tessFactors];
    Cmd m = cmd(op, uint8_t(raw::icb::kPatchPrimitive));
    m.a = uint32_t(start);
    m.b = uint32_t(count);
    m.c = uint64_t(instances) << 32 | uint32_t(baseInstance);
    m.obj = (__bridge void *)ownBuffer(_cb, patchIndices);
    m.obj2 = (__bridge void *)ownBuffer(_cb, arguments);
    const float scale = _tessScaleSet ? _tessScale : 1.0f;
    uint32_t scaleBits;
    memcpy(&scaleBits, &scale, sizeof scaleBits);
    uint64_t words[PatchExtraWords] = {};
    words[PatchIndexOffset] = patchIndexOffset;
    words[PatchArgumentOffset] = argumentOffset;
    words[PatchFactors] = uintptr_t((__bridge void *)_tessFactors);
    words[PatchPointIndices] = uintptr_t((__bridge void *)ownBuffer(_cb, pointIndices));
    words[PatchFactorOffset] = _tessFactorOffset;
    words[PatchPointIndexOffset] = pointIndexOffset;
    words[PatchFactorStride] = _tessFactorStride;
    words[PatchControlPoints] = controlPoints;
    words[PatchScale] = scaleBits;
    m.x = _cb->_extra.size();
    _cb->_extra.insert(_cb->_extra.end(), words, words + PatchExtraWords);
    [_cb record:m];
}

- (void)drawPatches:(NSUInteger)controlPoints
                patchStart:(NSUInteger)start
                patchCount:(NSUInteger)count
          patchIndexBuffer:(id<MTLBuffer>)patchIndices
    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
             instanceCount:(NSUInteger)instances
              baseInstance:(NSUInteger)baseInstance
{
    [self recordPatches:OpDrawPatches
           controlPoints:controlPoints
                   start:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:nil
        pointIndexOffset:0
               arguments:nil
          argumentOffset:0];
}

- (void)drawPatches:(NSUInteger)controlPoints
          patchIndexBuffer:(id<MTLBuffer>)patchIndices
    patchIndexBufferOffset:(NSUInteger)patchIndexOffset
            indirectBuffer:(id<MTLBuffer>)arguments
      indirectBufferOffset:(NSUInteger)argumentOffset
{
    [self recordPatches:OpDrawPatchesIndirect
           controlPoints:controlPoints
                   start:0
                   count:0
               instances:0
            baseInstance:0
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:nil
        pointIndexOffset:0
               arguments:arguments
          argumentOffset:argumentOffset];
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
{
    [self recordPatches:OpDrawPatches
           controlPoints:controlPoints
                   start:start
                   count:count
               instances:instances
            baseInstance:baseInstance
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:pointIndices
        pointIndexOffset:pointIndexOffset
               arguments:nil
          argumentOffset:0];
}

- (void)drawIndexedPatches:(NSUInteger)controlPoints
                 patchIndexBuffer:(id<MTLBuffer>)patchIndices
           patchIndexBufferOffset:(NSUInteger)patchIndexOffset
          controlPointIndexBuffer:(id<MTLBuffer>)pointIndices
    controlPointIndexBufferOffset:(NSUInteger)pointIndexOffset
                   indirectBuffer:(id<MTLBuffer>)arguments
             indirectBufferOffset:(NSUInteger)argumentOffset
{
    [self recordPatches:OpDrawPatchesIndirect
           controlPoints:controlPoints
                   start:0
                   count:0
               instances:0
            baseInstance:0
            patchIndices:patchIndices
        patchIndexOffset:patchIndexOffset
            pointIndices:pointIndices
        pointIndexOffset:pointIndexOffset
               arguments:arguments
          argumentOffset:argumentOffset];
}

@end

bool encodePatches(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const RMPlanPatches &patches,
                   bool indirect, const char **why)
{
    RMBuffer *factors = (__bridge RMBuffer *)patches.factors, *patchIndices = (__bridge RMBuffer *)draw.indices,
             *pointIndices = (__bridge RMBuffer *)patches.pointIndices,
             *arguments = (__bridge RMBuffer *)draw.arguments;
    RMBuffer *const read[] = {factors, patchIndices, pointIndices, arguments};
    for (RMBuffer *b : read)
        if (b)
            useBuffer(rc, b, false);
    re->setTessellationFactors(factors ? &factors->_raw : nullptr, patches.factorOffset, patches.factorStride);
    re->setTessellationFactorScale(patches.scale);
    const raw::Buffer *patchBuffer = patchIndices ? &patchIndices->_raw : nullptr;
    const raw::Buffer *pointBuffer = pointIndices ? &pointIndices->_raw : nullptr;
    if (indirect) {
        if (!arguments) {
            *why = "an indirect patch draw without arguments";
            return false;
        }
        return re->drawPatchesIndirect(patches.controlPoints, patchBuffer, draw.indexOffset, pointBuffer,
                                       patches.pointIndexOffset, &arguments->_raw, draw.argumentOffset, why);
    }
    return re->drawPatches(patches.controlPoints, draw.first, draw.count, draw.instances, draw.baseInstance,
                           patchBuffer, draw.indexOffset, pointBuffer, patches.pointIndexOffset, why);
}

void replayPatches(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const RMPlanPatches &patches,
                   bool indirect, const std::function<id(void *)> &use)
{
    [encoder setTessellationFactorBuffer:use(patches.factors)
                                  offset:patches.factorOffset
                          instanceStride:patches.factorStride];
    [encoder setTessellationFactorScale:patches.scale];
    id patchIndices = use(draw.indices), pointIndices = use(patches.pointIndices);
    if (indirect && pointIndices)
        [encoder drawIndexedPatches:patches.controlPoints
                         patchIndexBuffer:patchIndices
                   patchIndexBufferOffset:draw.indexOffset
                  controlPointIndexBuffer:pointIndices
            controlPointIndexBufferOffset:patches.pointIndexOffset
                           indirectBuffer:use(draw.arguments)
                     indirectBufferOffset:draw.argumentOffset];
    else if (indirect)
        [encoder drawPatches:patches.controlPoints
                  patchIndexBuffer:patchIndices
            patchIndexBufferOffset:draw.indexOffset
                    indirectBuffer:use(draw.arguments)
              indirectBufferOffset:draw.argumentOffset];
    else if (pointIndices)
        [encoder drawIndexedPatches:patches.controlPoints
                               patchStart:draw.first
                               patchCount:draw.count
                         patchIndexBuffer:patchIndices
                   patchIndexBufferOffset:draw.indexOffset
                  controlPointIndexBuffer:pointIndices
            controlPointIndexBufferOffset:patches.pointIndexOffset
                            instanceCount:draw.instances
                             baseInstance:draw.baseInstance];
    else
        [encoder drawPatches:patches.controlPoints
                        patchStart:draw.first
                        patchCount:draw.count
                  patchIndexBuffer:patchIndices
            patchIndexBufferOffset:draw.indexOffset
                     instanceCount:draw.instances
                      baseInstance:draw.baseInstance];
}
