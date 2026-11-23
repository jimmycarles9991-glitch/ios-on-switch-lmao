// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"

#include "sample_shading.h"
#include "shader_test_push.h"

@implementation SwitchMetalRenderEncoder

// Core Animation's texture-and-sampler SPI (CA::OGL::MetalContext::draw, every textured draw), as the frontend answers it
- (void)setFragmentTexture:(__unsafe_unretained id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(__unsafe_unretained id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    const bool textureChanged = gm20bBindTexture(self, 2, t, ti);
    const bool samplerChanged = gm20bBindSampler(self, 2, s, si);
    if (textureChanged || samplerChanged)
        [self prepareDrawnRecord];
}

// A store action the pass left Unknown, decided by the encoder: the pass's descriptor takes it before either driver encodes it
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index
{
    if (index < 8) {
        gm20bOwnPass(self);
        _pass.colorAttachments[index].storeAction = action;
        _cb->_passes[_passAt].color[index].store = action;
    }
}

// A depth or stencil store the pass left Unknown, decided now and kept as the pass's. A resolve there names no resolve
// texture (the pass took Unknown only without one), Metal's validation error: with nothing to resolve into, the aspect
// keeps the store the action names besides, as a colour attachment's does (TegraGM20BMetalRangePass.mm). A sample-position
// agnostic store is taken as the plain one: the depth target keeps each sample's value, which a later pass reads at its
// own positions.
static MTLStoreAction decideZetaStore(SwitchMetalRenderEncoder *e, uint32_t aspect, MTLStoreAction action)
{
    if (e->_zeta &&
        (action == MTLStoreActionMultisampleResolve || action == MTLStoreActionStoreAndMultisampleResolve)) {
        SWITCH_REFUSE("gm20b", "pass.resolve_store",
                      "a depth or stencil store action %lu resolving with no resolve texture: Metal's validation error "
                      "(MTLRenderPass.h resolveTexture), nothing resolved",
                      (unsigned long)action);
        gm20bSkipped(PredPassDepthStencil);
        action = action == MTLStoreActionMultisampleResolve ? MTLStoreActionDontCare : MTLStoreActionStore;
    }
    if (action == MTLStoreActionCustomSampleDepthStore) {
        action = MTLStoreActionStore;
    } else if (e->_zeta && action != MTLStoreActionDontCare && action != MTLStoreActionStore) {
        SWITCH_REFUSE("gm20b", "pass.zeta_store",
                      "a depth or stencil store action %lu decided by the encoder: Metal's validation error "
                      "(MTLRenderCommandEncoder.h setDepthStoreAction:, a store action other than Unknown), stored",
                      (unsigned long)action);
        gm20bSkipped(PredPassDepthStencil);
        action = MTLStoreActionStore;
    }
    e->_cb->_passes[e->_passAt].zeta[aspect].store = action;
    return action;
}

- (void)setDepthStoreAction:(MTLStoreAction)action
{
    gm20bOwnPass(self);
    _pass.depthAttachment.storeAction = decideZetaStore(self, 0, action);
}

- (void)setStencilStoreAction:(MTLStoreAction)action
{
    gm20bOwnPass(self);
    _pass.stencilAttachment.storeAction = decideZetaStore(self, 1, action);
}

void visibilityError(SwitchMetalCommandBuffer *cb, uint64_t mode, uint64_t offset, const char *why)
{
    if (!predLogged(PredCallVisibility))
        logPred(PredCallVisibility, mode, offset,
                [NSString stringWithFormat:@"%s: Metal's validation error (MTLRenderCommandEncoder.h "
                                           @"setVisibilityResultMode:offset:, MTLVisibilityResultMode)",
                                           why]);
    hit(cb, PredCallVisibility, mode, offset);
}

// Apple's encoder SPI as metal2vulkan answers it (metal2vulkan-runtime apple_only.rs, render/restart.rs, from Metal's measured
// answers): at Metal's default each changes nothing a draw does; any other value is answered zero with its error line
// (forwardInvocation:)
static void forwardCall(SwitchMetalEncoder *e, SEL selector, const void *argument)
{
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[e methodSignatureForSelector:selector]];
    inv.selector = selector;
    [inv setArgument:const_cast<void *>(argument) atIndex:2];
    [e forwardInvocation:inv];
}

- (void)setAlphaTestReferenceValue:(float)value
{
    (void)value;
}

- (void)setPrimitiveRestartEnabled:(BOOL)enabled
{
    if (!enabled)
        forwardCall(self, _cmd, &enabled);
}

- (void)setViewportTransformEnabled:(BOOL)enabled
{
    if (!enabled)
        forwardCall(self, _cmd, &enabled);
}

- (void)setProvokingVertexMode:(NSUInteger)mode
{
    if (mode)
        forwardCall(self, _cmd, &mode);
}

// Core Animation sets the mode Disabled when its encoder state changes (update_encoder_state); a counting mode is the 3D
// engine's sample counter (visibility.h). What Metal's validation stops here (MTLRenderCommandEncoder.h
// setVisibilityResultMode:offset: and MTLVisibilityResultMode: a mode it lacks, no result buffer, an offset past it, a
// result buffer of another device's) fails the command buffer.
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    id<MTLBuffer> results = _visibility;
    bool accumulate = false;
    if (mode != MTLVisibilityResultModeDisabled) {
        gm20b::VisibilityAsk ask;
        ask.mode = uint32_t(mode);
        ask.offset = offset;
        ask.gm20bBuffer = [(id)results isKindOfClass:[SwitchMetalBuffer class]];
        ask.bufferLength = ask.gm20bBuffer ? ((SwitchMetalBuffer *)results)->_raw.length : 0;
        accumulate = [_pass respondsToSelector:@selector(visibilityResultType)] &&
                     [[(id)_pass valueForKey:@"visibilityResultType"] integerValue] != 0;
        if (const char *why = gm20b::visibilityRefusal(ask)) {
            visibilityError(_cb, mode, offset, why);
            return failCommandBuffer(_cb,
                                     [NSString stringWithFormat:@"setVisibilityResultMode:offset: %s: Metal's "
                                                                @"validation error (MTLRenderCommandEncoder.h)",
                                                                why],
                                     MTLCommandBufferErrorInternal, PredCallVisibility);
        }
    }
    Cmd m = cmd(OpVisibility);
    m.a = uint32_t(mode);
    m.b = accumulate;
    m.c = offset;
    m.obj = (__bridge void *)results;
    [_cb record:m];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    SWITCH_API(ApiDraw);
    if (![(id)buffer isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bCallInvalid(_cb, PredCallBufForeign, 0xffff, 0,
                                buffer ? "an index buffer not made by this device" : "a nil index buffer");
    if (indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32)
        return skip(_cb, PredEncIndexedRange, offset, indexType, "an index type Metal has none of: the draw skipped");
    if (![self checkDraw])
        return;
    [_cb retain:buffer];
    Cmd m = cmd(OpDrawIndexed, uint8_t(type));
    m.a = uint32_t(count);
    m.b = uint32_t(indexType);
    m.c = offset;
    m.obj = (__bridge void *)buffer;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(instances);
    _cb->_extra.push_back(uint64_t(baseVertex));
    _cb->_extra.push_back(baseInstance);
    [_cb record:m];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
{
    SWITCH_API(ApiDraw);
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:instances
                     baseVertex:0
                   baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    SWITCH_API(ApiState);
    SwitchMetalPipeline *p = (SwitchMetalPipeline *)state;
    if (![(id)p isKindOfClass:[SwitchMetalPipeline class]])
        return gm20bCallInvalid(_cb, PredStateForeign, 0, 0,
                                state ? "a render state not made by this device" : "a nil render state");
    __unsafe_unretained SwitchMetalPipeline *next = p;
    const bool changed = _pipe != next;
    if (changed)
        _formSamplers[0] = _formSamplers[1] = 0;
    _pipe = next;
    _drawn = _pipe ? _pipe->_raw : nullptr;
    _drawnVariant = nullptr;
    _eligibility++;
    setBinding(_cb, OpPipeline, 0, 0, state, 0);
    if (changed)
        [self prepareDrawnRecord];
}

// What gm20b::RenderEncoder::prepare would refuse of a draw with the encoder's bindings, decided here (the same tests)
- (BOOL)checkDraw
{
    _drew = true;
    // no state set, or the encoder failed its command buffer already (a failed pass's formats are not recorded, and every
    // draw would hit enc.colorfmt for nothing, r7p5's "a=0 b=0"), or a parallel encoder's sub-encoder whose parent
    // ended, whose draws are refused
    if (!_pipe || _cb->_encFailed)
        return !gm20bDrawAfterParallelEnd(_cb);
    if (_checkedEligibility == _eligibility && _checkedEpoch == _cb->_plan.stateEpoch() &&
        _checkedPipeline == _pipe->_raw)
        return YES;
    const gm20b::Pipeline &pl = *_pipe->_raw;
    for (uint32_t j = 0; j < 8; j++) {
        const uint32_t f = j < _count ? _formats[j] : 0;
        if (f != pl.colorFormats[j] && !(f && pl.colorFormats[j] == 0) &&
            !(gm20b::targetOrdersChannels(pl.colorFormats[j], f) && !(pl.storageBits >> j & 1))) {
            skip(_cb, PredEncColorFormats, j, f, "a state's colour format another than its pass's: the draw skipped");
            return NO;
        }
    }
    if (_turn && pl.turnRefused) {
        gm20bTurnRefused(_cb, pl.rec->keyHash, _turn, _pipe->_key.c_str());
        return NO;
    }
    uint32_t sampleMasks[4];
    if (!gm20b::samplesDrawn(pl.samples, _samples, pl.samplePasses) ||
        !gm20b::sampleShadingMasks(_samples, pl.samples == _samples ? pl.samplePasses : 1, sampleMasks)) {
        skip(_cb, PredEncSamples, pl.samples, _samples | uint64_t(pl.samplePasses) << 32,
             "a state's samples its pass draws nothing defined for: the draw skipped");
        return NO;
    }
    [self drawnRecord];
    _checkedEligibility = _eligibility;
    _checkedEpoch = _cb->_plan.stateEpoch();
    _checkedPipeline = _pipe->_raw;
    return YES;
}

- (void)setScissorRect:(MTLScissorRect)r
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpScissor);
    m.a = uint32_t(r.x);
    m.b = uint32_t(r.y);
    m.c = uint64_t(r.width) << 32 | uint32_t(r.height);
    [_cb recordBinding:m];
}

- (void)setViewport:(MTLViewport)v
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpViewport);
    m.x = _cb->_extra.size();
    const double d[6] = {v.originX, v.originY, v.width, v.height, v.znear, v.zfar};
    for (double x : d) {
        uint64_t u;
        memcpy(&u, &x, 8);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

- (void)setCullMode:(MTLCullMode)mode
{
    Cmd m = cmd(OpCull);
    m.a = uint32_t(mode);
    [_cb record:m];
}

- (void)setFrontFacingWinding:(MTLWinding)w
{
    Cmd m = cmd(OpWinding);
    m.a = uint32_t(w);
    [_cb record:m];
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
    if (s && ![(id)s isKindOfClass:[SwitchMetalDepthStencil class]])
        return gm20bCallInvalid(_cb, PredStateForeign, 0, 0, "a depth/stencil state not made by this device");
    _ds = s ? ((SwitchMetalDepthStencil *)s)->_raw.desc() : gm20b::DepthStencilDesc();
    _eligibility++;
    if (gm20b::testsHiddenZeta(_ds))
        _cb->_passes[_passAt].testsHiddenZeta = true;
    setBinding(_cb, OpDepthStencil, 0, 0, s, 0);
    const uint32_t zeta = _zeta ? _zeta : _cb->_passes[_passAt].testsHiddenZeta ? gm20b::kZetaFormat : 0;
    if (const gm20b::Pipeline *pl = _pipe ? _pipe->_raw : nullptr;
        pl && _forms.fragmentTests != gm20b::drawnTests(pl->fetchBits && pl->depthFormat && !pl->storageBits,
                                                        pl->rec->shaderTests, _ds, zeta))
        [self prepareDrawnRecord];
}

// SPI: lines one pixel wide are what the driver draws (its default); any other width is answered zero with its error line
- (void)setLineWidth:(float)width
{
    if (width == 1.0f)
        return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[self methodSignatureForSelector:_cmd]];
    inv.selector = _cmd;
    [inv setArgument:&width atIndex:2];
    [self forwardInvocation:inv];
}

// No tile shaders: tile shading is Apple4's, a family the device does not claim; their calls are answered zero
- (NSUInteger)tileWidth
{
    return 0;
}

- (NSUInteger)tileHeight
{
    return 0;
}

- (void)setTriangleFillMode:(MTLTriangleFillMode)mode
{
    Cmd m = cmd(OpFillMode);
    m.a = uint32_t(mode);
    [_cb record:m];
}

- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
    Cmd m = cmd(OpBlendColor);
    m.x = _cb->_extra.size();
    const float f[4] = {r, g, b, a};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

- (void)setStencilReferenceValue:(uint32_t)value
{
    Cmd m = cmd(OpStencilRef);
    m.a = value;
    m.b = value;
    [_cb record:m];
}

- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
    Cmd m = cmd(OpStencilRef);
    m.a = front;
    m.b = back;
    [_cb record:m];
}

- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp
{
    Cmd m = cmd(OpDepthBias);
    m.x = _cb->_extra.size();
    const float f[3] = {bias, slope, clamp};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}

static void recordDraw(__unsafe_unretained SwitchMetalRenderEncoder *e, MTLPrimitiveType type, NSUInteger start,
                       NSUInteger count, NSUInteger instances, NSUInteger base)
{
    SWITCH_API(ApiDraw);
    if (![e checkDraw])
        return;
    Cmd m = cmd(OpDraw, uint8_t(type));
    m.a = uint32_t(start);
    m.b = uint32_t(count);
    m.c = uint64_t(instances) << 32 | uint32_t(base);
    [e->_cb record:m];
}

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)base
{
    recordDraw(self, type, start, count, instances, base);
}

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
{
    recordDraw(self, type, start, count, instances, 0);
}

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
    recordDraw(self, type, start, count, 1, 0);
}

- (void)drawPrimitives:(MTLPrimitiveType)type
          indirectBuffer:(id<MTLBuffer>)arguments
    indirectBufferOffset:(NSUInteger)offset
{
    [self drawIndirect:type
               indexed:NO
             indexType:MTLIndexTypeUInt16
           indexBuffer:nil
           indexOffset:0
             arguments:arguments
                offset:offset];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indices
            indexBufferOffset:(NSUInteger)indexOffset
               indirectBuffer:(id<MTLBuffer>)arguments
         indirectBufferOffset:(NSUInteger)offset
{
    [self drawIndirect:type
               indexed:YES
             indexType:indexType
           indexBuffer:indices
           indexOffset:indexOffset
             arguments:arguments
                offset:offset];
}

- (void)drawIndirect:(MTLPrimitiveType)type
             indexed:(BOOL)indexed
           indexType:(MTLIndexType)indexType
         indexBuffer:(id<MTLBuffer>)indices
         indexOffset:(NSUInteger)indexOffset
           arguments:(id<MTLBuffer>)arguments
              offset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    const uint64_t bytes = indexed ? 20 : 16;
    SwitchMetalBuffer *buffer =
        [(id)arguments isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)arguments : nil;
    SwitchMetalBuffer *index =
        [(id)indices isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)indices : nil;
    if (!buffer)
        return gm20bCallInvalid(_cb, PredCallBufForeign, offset, bytes,
                                arguments ? "indirect arguments of a buffer not made by this device"
                                          : "indirect arguments of a nil buffer");
    if (indexed && !index)
        return gm20bCallInvalid(_cb, PredCallBufForeign, 0xffff, indexOffset,
                                indices ? "an index buffer not made by this device" : "a nil index buffer");
    if ((offset & 3) || (indexed && ((indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) ||
                                     indexOffset % (indexType == MTLIndexTypeUInt16 ? 2 : 4))))
        return skip(_cb, PredEncIndexedRange, offset, bytes,
                    "indirect arguments or indices Metal reads nothing of: the draw skipped");
    if (offset > buffer->_raw.length || bytes > buffer->_raw.length - offset)
        skip(_cb, PredEncIndexedRange, offset, bytes, "indirect arguments past their buffer read as zeros");
    if (![self checkDraw])
        return;
    [_cb retain:arguments];
    [_cb retain:indices];
    Cmd operation = cmd(indexed ? OpDrawIndexedIndirect : OpDrawIndirect, uint8_t(type));
    operation.obj = (__bridge void *)indices;
    operation.obj2 = (__bridge void *)arguments;
    operation.a = uint32_t(indexType);
    operation.c = indexOffset;
    operation.x = offset;
    [_cb record:operation];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
                   afterStages:(MTLRenderStages)after
                  beforeStages:(MTLRenderStages)before
{
    Cmd m = cmd(OpBarrier);
    m.a = uint32_t(scope);
    m.b = uint32_t(after);
    m.c = uint64_t(before);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
    if (!resource)
        return;
    if (![(id)resource isKindOfClass:[SwitchMetalObject class]])
        return gm20bCallInvalid(
            _cb, [(id)resource conformsToProtocol:@protocol(MTLTexture)] ? PredTexForeign : PredCallBufForeign, usage,
            stages, "a resource used (useResource:) not made by this device");
    [_cb retain:resource];
    Cmd m = cmd(OpUse);
    m.obj = (__bridge void *)resource;
    m.a = uint32_t(usage);
    m.b = uint32_t(stages);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    [self useResource:resource usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

// The heap as one use for reading (an argument buffer may reach any of its resources): its jobs name what it holds when
// they are submitted (heap_members.h), whatever it holds
- (void)useHeap:(id<MTLHeap>)heap
{
    [self useHeap:heap stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    if ([(id)heap isKindOfClass:[SwitchMetalHeap class]])
        [self useResource:(id<MTLResource>)heap usage:MTLResourceUsageRead stages:stages];
    else if (heap)
        gm20bCallInvalid(_cb, PredCallBufForeign, 0, stages, "a heap used (useHeap:) not made by this device");
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
              stages:(MTLRenderStages)stages
{
    for (NSUInteger k = 0; k < count; k++)
        [self useResource:resources[k] usage:usage stages:stages];
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
{
    [self useResources:resources count:count usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count
{
    for (NSUInteger k = 0; k < count; k++)
        [self useHeap:heaps[k]];
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages
{
    for (NSUInteger k = 0; k < count; k++)
        [self useHeap:heaps[k] stages:stages];
}

// A barrier on some resources is the scope barrier over all of them (iOS's texture scope covers render attachments)
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])resources
                             count:(NSUInteger)count
                       afterStages:(MTLRenderStages)after
                      beforeStages:(MTLRenderStages)before
{
    (void)resources;
    (void)count;
    // iOS has no MTLBarrierScopeRenderTargets; its texture scope covers render attachments
    [self memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures
                     afterStages:after
                    beforeStages:before];
}

// Every viewport and scissor the encoder sets, as both identities' drivers keep them (the A10X's setViewports:count:
// 0x21c9614cc stores its count unclamped and converts each one, though its device answers maxViewportCount 1), up to the
// 16 GM20B holds (SET_VIEWPORT_*(j))
static NSUInteger viewportCount(NSUInteger count)
{
    return std::min<NSUInteger>(count, gm20b::kMaxViewports);
}

- (void)setViewports:(const MTLViewport[])viewports count:(NSUInteger)count
{
    SWITCH_API(ApiState);
    count = viewportCount(count);
    if (count <= 1) {
        if (count)
            [self setViewport:viewports[0]];
        return;
    }
    Cmd m = cmd(OpViewport, 0, uint16_t(count));
    m.x = _cb->_extra.size();
    for (NSUInteger j = 0; j < count; j++) {
        const MTLViewport &v = viewports[j];
        for (double x : {v.originX, v.originY, v.width, v.height, v.znear, v.zfar}) {
            uint64_t u;
            memcpy(&u, &x, 8);
            _cb->_extra.push_back(u);
        }
    }
    [_cb record:m];
}

- (void)setScissorRects:(const MTLScissorRect[])rects count:(NSUInteger)count
{
    SWITCH_API(ApiState);
    count = viewportCount(count);
    if (count <= 1) {
        if (count)
            [self setScissorRect:rects[0]];
        return;
    }
    Cmd m = cmd(OpScissor, 0, uint16_t(count));
    m.x = _cb->_extra.size();
    for (NSUInteger j = 0; j < count; j++)
        for (NSUInteger word : {rects[j].x, rects[j].y, rects[j].width, rects[j].height})
            _cb->_extra.push_back(uint32_t(word));
    [_cb record:m];
}

- (void)setDepthClipMode:(MTLDepthClipMode)mode
{
    Cmd m = cmd(OpDepthClip);
    m.a = uint32_t(mode);
    [_cb record:m];
}

- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    (void)stages;
    [self updateFence:fence];
}

- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    (void)stages;
    [self waitForFence:fence];
}

@end
