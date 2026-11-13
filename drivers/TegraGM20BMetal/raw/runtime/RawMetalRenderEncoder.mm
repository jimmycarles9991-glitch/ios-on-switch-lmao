// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalRotated.h"

#include "raw_fetch_order.h"
#include "raw_residual.h"
#include "raw_sample_shading.h"

// What raw::RenderEncoder::selfRuns refuses of a draw that fetches a color it writes before it reads any geometry
// (raw_fetch_order.h), decided as the draw is recorded: its encoder goes to m2v, the command buffer's others stay raw
static void checkFetch(RMRenderEncoder *e, MTLPrimitiveType type, NSUInteger count, NSUInteger instances, bool indirect,
                       bool indexed)
{
    const raw::Pipeline *pl = e->_drawn;
    if (!pl || !e->_pipe || !e->_pipe->_raw || e->_cb->_encRefused)
        return;
    const raw::FetchRefusal refusal =
        raw::fetchRefusal(raw::fetchesOwnColor(pl->fetchBits, pl->writeBits, pl->storageBits), pl->primitiveId,
                          {uint32_t(type), uint32_t(count), uint32_t(instances), indirect, indexed});
    if (refusal == raw::FetchRefusal::None)
        return;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gRM->raw->stats.fetch.refusedAtRecord[size_t(refusal)]++;
    gRM->raw->stats.fetch.recordKeys.note(pl->rec ? pl->rec->keyHash : 0);
#endif
    [e->_cb takeReason:WhyEncode why:@(raw::fetchRefusalWhy(refusal))];
    hit(e->_cb, PredEncGeometry, type, uint32_t(count) | uint64_t(uint32_t(instances)) << 32);
}

@implementation RMRenderEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderEncoder");
}

// Core Animation's texture-and-sampler SPI (CA::OGL::MetalContext::draw, every textured draw), as the frontend answers it
- (void)setFragmentTexture:(id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    [self texture:2 object:t index:ti];
    [self sampler:2 object:s index:si];
}

// A store action the pass left Unknown, decided by the encoder: the pass's descriptor takes it before either driver encodes it
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index
{
    if (index < 8) {
        rmOwnPass(self);
        _pass.colorAttachments[index].storeAction = action;
        _cb->_passes[_passAt].color[index].store = action;
    }
}

- (void)setDepthStoreAction:(MTLStoreAction)action
{
    rmOwnPass(self);
    _pass.depthAttachment.storeAction = action;
    _cb->_passes[_passAt].zeta[0].store = action;
    if (_zeta && action != MTLStoreActionDontCare) {
        [_cb takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
        hit(_cb, PredPassDepthStencil, _zeta, action);
    }
}

- (void)setStencilStoreAction:(MTLStoreAction)action
{
    rmOwnPass(self);
    _pass.stencilAttachment.storeAction = action;
    _cb->_passes[_passAt].zeta[1].store = action;
    if (_zeta && action != MTLStoreActionDontCare) {
        [_cb takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
        hit(_cb, PredPassDepthStencil, _zeta, action);
    }
}

// Apple's encoder SPI as metal2vulkan answers it (metal2vulkan-runtime apple_only.rs, render/restart.rs, from Metal's measured
// answers): at Metal's default each changes nothing a draw does; any other value goes to m2v as the call it is
static void forwardCall(RMEncoder *e, SEL selector, const void *argument)
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
// engine's sample counter (raw_visibility.h)
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    id<MTLBuffer> results = _visibility;
    if (mode != MTLVisibilityResultModeDisabled) {
        raw::VisibilityAsk ask;
        ask.mode = uint32_t(mode);
        ask.offset = offset;
        ask.rawBuffer = [(id)results isKindOfClass:[RMBuffer class]];
        ask.bufferLength = ask.rawBuffer ? ((RMBuffer *)results)->_raw.length : 0;
        ask.accumulate = [_pass respondsToSelector:@selector(visibilityResultType)] &&
                         [[(id)_pass valueForKey:@"visibilityResultType"] integerValue] != 0;
        if (const char *why = raw::visibilityRefusal(ask)) {
            [_cb takeReason:WhyCall why:@(why)];
            hit(_cb, PredCallVisibility, mode);
        }
    }
    Cmd m = cmd(OpVisibility);
    m.a = uint32_t(mode);
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
    if (![(id)buffer isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCall why:@"an index buffer not made by this device"];
        hit(_cb, PredCallBufForeign, 0xffff, 0);
    } else if ((indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) ||
               !((RMBuffer *)buffer)->_raw.cpu) {
        [_cb takeReason:WhyEncode why:@"an indexed draw the raw path does not take"];
        hit(_cb, PredEncIndexedRange, offset, indexType == MTLIndexTypeUInt16 ? 2 : 4);
    }
    [self checkDraw];
    checkFetch(self, type, count, instances, false, true);
    if (buffer)
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
    RMPipeline *p = (RMPipeline *)state;
    if (![(id)p isKindOfClass:[RMPipeline class]]) {
        [_cb takeReason:WhyState why:@"a render state not made by this device"];
        hit(_cb, PredStateForeign);
    } else if (!p->_raw) {
        [_cb takeReason:WhyState why:@"a state not in the pack (its label: the line it was made with)"];
        hit(_cb, !p->_completable ? PredStateNotPackable : PredStateNotInPack);
    }
    _pipe = [(id)p isKindOfClass:[RMPipeline class]] ? p : nil;
    _drawn = _pipe ? _pipe->_raw : nullptr;
    _drawnVariant = nullptr;
    setBinding(_cb, OpPipeline, 0, 0, state, 0);
}

// What raw::RenderEncoder::prepare would refuse of a draw with the encoder's bindings, decided here (the same tests)
- (void)checkDraw
{
    _drew = true;
    // no raw draw to decide (m2v drops a draw without a state too), or this encoder goes to m2v already (its pass refused: its
    // formats are not recorded, and every draw would hit enc.colorfmt for nothing, r7p5's "a=0 b=0")
    if (!_pipe || !_pipe->_raw || _cb->_encRefused)
        return;
    if (_checkedGeneration == _cb->_plan.eligibilityGeneration() && _checkedPipeline == _pipe->_raw)
        return;
    const raw::Pipeline &pl = *_pipe->_raw;
    for (uint32_t j = 0; j < 8; j++) {
        const uint32_t f = j < _count ? _formats[j] : 0;
        if (f != pl.colorFormats[j] && !(f && pl.colorFormats[j] == 0)) {
            [_cb takeReason:WhyEncode why:@"a state whose color formats differ from the pass's"];
            return hit(_cb, PredEncColorFormats, j, f);
        }
    }
    if (pl.depthFormat != _zeta || (!_zeta && _ds.active())) {  // (raw::RenderEncoder::prepare's test)
        [_cb takeReason:WhyEncode why:@"a depth/stencil state or format the pass's attachment does not take"];
        return hit(_cb, PredEncDepthStencil, pl.depthFormat, _zeta | uint64_t(_ds.active()) << 32);
    }
    if (_turn && pl.turnRefused) {
        [_cb takeReason:WhyEncode why:@(raw::rotatedRefusalText(raw::RotatedRefusal::Variant))];
        return hit(_cb, PredPassRotated, uint64_t(raw::RotatedRefusal::Variant), _turn);
    }
    uint32_t sampleMasks[4];
    if (pl.samples != _samples || !raw::sampleShadingMasks(_samples, pl.samplePasses, sampleMasks)) {
        [_cb takeReason:WhyEncode why:@"a state whose samples a pixel or per-sample shading the pass does not take"];
        return hit(_cb, PredEncSamples, pl.samples, _samples | uint64_t(pl.samplePasses) << 32);
    }
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (e.kind == raw::EntryUbo) {
            // an offset off m2v's descriptor base alignment needs the push word of its SlotBindOffset residual, which a
            // record made without the probe's offset run lacks, or a storage slot whose code another shares
            // (raw::bufferDescriptor)
            const bool storage = raw::bufferStorage(e);
            const uint64_t residual =
                e.stage < 3 && e.slot < 31 ? _bufVa[e.stage][e.slot] & (raw::bufferBaseAlignment(storage) - 1) : 0;
            if (raw::bufferResidualWord(e) < 0 && residual) {
                [_cb takeReason:WhyEncode
                            why:storage ? @"a storage buffer bound at an offset that is not a multiple of 16"
                                        : @"a uniform buffer bound at an offset that is not a multiple of 256"];
                return hit(_cb, PredEncUboOffset, uint64_t(e.stage) << 8 | e.slot, residual);
            }
        } else if (e.kind == raw::EntryTexture && e.samplerKind == raw::SamplerConstexpr &&
                   e.slot < raw::kStageTextures && _tex[raw::handleStage(e)][e.slot] &&
                   _tex[raw::handleStage(e)][e.slot]->_raw.addressMap) {
            [_cb takeReason:WhyEncode why:@"a planar texture sampled through a constexpr sampler"];
            return hit(_cb, PredEncPlanarClamp, e.slot, _tex[raw::handleStage(e)][e.slot]->_raw.addressMap);
        }
    }
    [self drawnRecord];
    _checkedGeneration = _cb->_plan.eligibilityGeneration();
    _checkedPipeline = _pipe->_raw;
}

- (void)setScissorRect:(MTLScissorRect)r
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpScissor);
    m.a = uint32_t(r.x);
    m.b = uint32_t(r.y);
    m.c = uint64_t(r.width) << 32 | uint32_t(r.height);
    [_cb record:m];
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
    if (s && ![(id)s isKindOfClass:[RMDepthStencil class]]) {
        [_cb takeReason:WhyCall why:@"a depth/stencil state not made by this device"];
        hit(_cb, PredStateForeign);
    }
    _ds = [(id)s isKindOfClass:[RMDepthStencil class]] ? ((RMDepthStencil *)s)->_raw.desc() : raw::DepthStencilDesc();
    if (raw::testsHiddenZeta(_ds))
        _cb->_passes[_passAt].testsHiddenZeta = true;
    setBinding(_cb, OpDepthStencil, 0, 0, s, 0);
}

// SPI: lines one pixel wide are what the raw path draws (its default); any other width goes to m2v with the call
- (void)setLineWidth:(float)width
{
    if (width == 1.0f)
        return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[self methodSignatureForSelector:_cmd]];
    inv.selector = _cmd;
    [inv setArgument:&width atIndex:2];
    [self forwardInvocation:inv];
}

// No tile shaders (as m2v: the device says so); their calls stay m2v's, which refuses them
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
    // an infinite or NaN factor biases every fragment to a depth m2v sets through the viewport (render_state.rs
    // depth_bias_effect), which the raw path does not
    if (!std::isfinite(bias) || !std::isfinite(slope)) {
        uint32_t b, k;
        memcpy(&b, &bias, 4);
        memcpy(&k, &slope, 4);
        [_cb takeReason:WhyCall why:@"a depth bias of an infinite or NaN factor"];
        hit(_cb, PredCallDepthBias, b, k);
    }
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

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)base
{
    SWITCH_API(ApiDraw);
    [self checkDraw];
    checkFetch(self, type, count, instances, false, false);
    Cmd m = cmd(OpDraw, uint8_t(type));
    m.a = uint32_t(start);
    m.b = uint32_t(count);
    m.c = uint64_t(instances) << 32 | uint32_t(base);
    [_cb record:m];
}

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
{
    SWITCH_API(ApiDraw);
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
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
    RMBuffer *buffer = [(id)arguments isKindOfClass:[RMBuffer class]] ? (RMBuffer *)arguments : nil;
    RMBuffer *index = [(id)indices isKindOfClass:[RMBuffer class]] ? (RMBuffer *)indices : nil;
    if (!buffer || (offset & 3) || offset > buffer->_raw.length || bytes > buffer->_raw.length - offset ||
        (indexed && (!index || (indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) ||
                     indexOffset > index->_raw.length || indexOffset % (indexType == MTLIndexTypeUInt16 ? 2 : 4)))) {
        [_cb takeReason:WhyEncode why:@"indirect draw arguments or indices outside the buffer"];
        hit(_cb, PredEncIndexedRange, offset, bytes);
    }
    [self checkDraw];
    checkFetch(self, type, 0, 1, true, indexed);
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
// they are submitted (raw_heap_members.h), whatever it holds
- (void)useHeap:(id<MTLHeap>)heap
{
    [self useHeap:heap stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    if ([(id)heap isKindOfClass:[RMHeap class]])
        [self useResource:(id<MTLResource>)heap usage:MTLResourceUsageRead stages:stages];
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
    return std::min<NSUInteger>(count, raw::kMaxViewports);
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
