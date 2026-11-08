// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

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
        _pass.colorAttachments[index].storeAction = action;
        _cb->_passes[_passAt].color[index].store = action;
    }
}

- (void)setDepthStoreAction:(MTLStoreAction)action
{
    _pass.depthAttachment.storeAction = action;
    if (_zeta && action != MTLStoreActionDontCare) {
        [_cb takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
        hit(_cb, PredPassDepthStencil, _zeta, action);
    }
}

- (void)setStencilStoreAction:(MTLStoreAction)action
{
    _pass.stencilAttachment.storeAction = action;
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

// Core Animation sets the mode Disabled when its encoder state changes (update_encoder_state): nothing to do; any other mode
// counts occlusion, which the raw path does not
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    if (mode != MTLVisibilityResultModeDisabled) {
        [_cb takeReason:WhyCall why:@"a visibility result mode"];
        hit(_cb, PredCallVisibility, mode);
    }
    Cmd m = cmd(OpVisibility);
    m.a = uint32_t(mode);
    m.c = offset;
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
        hit(_cb, !p->_completable ? PredStateNotPackable : p->_asked ? PredStateBeingMade : PredStateNotInPack);
    }
    _pipe = [(id)p isKindOfClass:[RMPipeline class]] ? p : nil;
    setBinding(_cb, OpPipeline, 0, 0, state, 0);
}

// What raw::RenderEncoder::prepare would refuse of a draw with the encoder's bindings, decided here (the same tests)
- (void)checkDraw
{
    // no raw draw to decide (m2v drops a draw without a state too), or this encoder goes to m2v already (its pass refused: its
    // formats are not recorded, and every draw would hit enc.colorfmt for nothing, r7p5's "a=0 b=0")
    if (!_pipe || !_pipe->_raw || _cb->_encRefused)
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
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (e.kind == raw::EntryUbo) {
            // an offset off NVK's 256-byte uniform alignment needs the push word of m2v's SlotBindOffset residual, which a
            // record made without the probe's offset run lacks (raw::bufferResidualWord)
            if (!raw::bufferStorage(e) && raw::bufferResidualWord(e) < 0 && e.stage < 3 && e.slot < 31 &&
                (_bufVa[e.stage][e.slot] & 255)) {
                [_cb takeReason:WhyEncode why:@"a uniform buffer bound at an offset that is not a multiple of 256"];
                return hit(_cb, PredEncUboOffset, uint64_t(e.stage) << 8 | e.slot, _bufVa[e.stage][e.slot] & 255);
            }
        } else if (e.kind == raw::EntryTexture && e.samplerKind == raw::SamplerConstexpr && e.slot < 16 &&
                   _tex[e.slot] && _tex[e.slot]->_raw.addressMap) {
            [_cb takeReason:WhyEncode why:@"a planar texture sampled through a constexpr sampler"];
            return hit(_cb, PredEncPlanarClamp, e.slot, _tex[e.slot]->_raw.addressMap);
        }
    }
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
    _ds = [(id)s isKindOfClass:[RMDepthStencil class]] ? ((RMDepthStencil *)s)->_raw : raw::DepthStencilDesc();
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

- (void)useHeap:(id<MTLHeap>)heap
{
    (void)heap;  // the facade's heaps hold ordinary resources: nothing to make resident
}

- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    (void)heap;
    (void)stages;
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
    (void)heaps;
    (void)count;
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages
{
    (void)heaps;
    (void)count;
    (void)stages;
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

// The count forms at one viewport or scissor are the single ones; more go to m2v with the call
- (void)forwardCall:(SEL)selector pointer:(const void *)pointer count:(NSUInteger)count
{
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[self methodSignatureForSelector:selector]];
    inv.selector = selector;
    [inv setArgument:&pointer atIndex:2];
    [inv setArgument:&count atIndex:3];
    [self forwardInvocation:inv];
}

- (void)setViewports:(const MTLViewport[])viewports count:(NSUInteger)count
{
    if (count == 1)
        [self setViewport:viewports[0]];
    else
        [self forwardCall:_cmd pointer:viewports count:count];
}

- (void)setScissorRects:(const MTLScissorRect[])rects count:(NSUInteger)count
{
    if (count == 1)
        [self setScissorRect:rects[0]];
    else
        [self forwardCall:_cmd pointer:rects count:count];
}

// Metal's default clips; clamping goes to m2v with the call
- (void)setDepthClipMode:(MTLDepthClipMode)mode
{
    if (mode != MTLDepthClipModeClip)
        forwardCall(self, _cmd, &mode);
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
