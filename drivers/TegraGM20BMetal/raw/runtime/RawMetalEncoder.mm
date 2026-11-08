// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

@implementation RMEncoder

- (id)rmTwin
{
    return nil;
}

// What the m2v encoder of the same kind answers (respondsToSelector: as Core Animation's optional paths ask it today)
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = [[self class] twinClass];
    return c && [c instancesRespondToSelector:selector];
}

// A selector this encoder does not record itself (one the m2v encoder implements, or some MTL protocol declares): recorded as an
// invocation (objects mapped to twins at replay) and the command buffer goes to m2v.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = [[self class] twinClass];
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    [inv retainArguments];
    if (!raw::keepPointedArguments(inv))
        SWITCH_REFUSE_DETAIL("raw", "call.pointer", uintptr_t(inv.selector),
                             "-[%s %s] recorded with a pointer argument of no known extent",
                             class_getName(object_getClass(self)), sel_getName(inv.selector));
    [_cb retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    [_cb record:c];
    [_cb takeReason:WhyCall why:@"an encoder call recorded as an invocation"];
    hit(_cb, PredCallInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.invocation", uintptr_t(inv.selector), "-[%s %s] recorded as an invocation",
                         class_getName(object_getClass(self)), sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

+ (Class)twinClass
{
    return nil;
}

// One its caller dropped without ending it is ended here, so the tape's encoders stay closed
- (void)dealloc
{
    if (!_ended && _cb)
        [self endEncoding];
}

- (void)endEncoding
{
    SWITCH_API(ApiEncoder);
    if (_ended)
        return;
    _ended = true;
    [_cb record:cmd(OpEnd)];
    _cb->_active = nil;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}

// (RawResidency.h: everything is resident; m2v's encoders take them as no-ops too)
- (void)useResidencySet:(id)set
{
    (void)set;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    (void)sets;
    (void)count;
}

- (void)updateFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpUpdateFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

- (void)waitForFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpWaitFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}

@end

// A depth/stencil attachment the raw path renders (raw::DepthStencilAttachment): one memoryless Depth32Float_Stencil8 texture as
// both aspects, whole (level, slice and depth plane 0, no resolve), cleared or not loaded and not stored (memoryless), a clear
// depth that is a number (metal2vulkan refuses a NaN)
static bool rawDepthStencil(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    RMTexture *t = (RMTexture *)d.texture;
    if (!t || t != (RMTexture *)s.texture || ![(id)t isKindOfClass:[RMTexture class]] || !t->_rawOk || !t->_raw.zeta ||
        !t->_raw.memoryless || std::isnan(d.clearDepth))
        return false;
    for (MTLRenderPassAttachmentDescriptor *a : @[ d, s ])
        if (a.level || a.slice || a.depthPlane || a.resolveTexture || a.storeAction != MTLStoreActionDontCare ||
            (a.loadAction != MTLLoadActionClear && a.loadAction != MTLLoadActionDontCare))
            return false;
    return true;
}

@implementation RMCommandBuffer (Encoders)

- (void)begin:(RMEncoder *)e
{
    if (_active)
        [(RMEncoder *)_active endEncoding];  // Metal: one encoder at a time
    e->_device = _device;
    e->_cb = self;
    _active = e;
    [self closeEncoder];
    _encAt = _tape.size();  // its first command is the next one recorded
    _encOpen = true;
    _encRefused = false;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    RMRenderEncoder *e = [RMRenderEncoder new];
    [self begin:e];
    MTLRenderPassDescriptor *pass = [d copy];
    [self retain:pass];
    e->_pass = pass;
    e->_passAt = uint32_t(_passes.size());
    RMPass &facts = _passes.emplace_back();
    for (unsigned c = 0; c < 8; c++) {
        MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[c];
        id<MTLTexture> t = a.texture;
        if (!t)
            continue;
        facts.color[c] = {t, a.level, a.loadAction, a.storeAction, a.clearColor};
        [self retain:t];
        [self retain:pass.colorAttachments[c].resolveTexture];
        RMTexture *x = (RMTexture *)t;
        if ([(id)x isKindOfClass:[RMTexture class]])
            declareGpuWrites(x);
        if (![(id)x isKindOfClass:[RMTexture class]] || !x->_rawOk || !(x->_raw.renderable || x->_raw.shadowTarget)) {
            [self takeReason:WhyPass why:@"a color attachment the raw path does not render to"];
            const bool own = [(id)x isKindOfClass:[RMTexture class]];
            // a = format; b = attachment | kind << 8 | the texture's own predicate << 16 (0: kept, not renderable)
            const uint64_t b = c | (own ? uint64_t(x->_kind) : 0xff) << 8 | (own ? uint64_t(x->_pred) : 0) << 16;
            if (own && x->_rawOk &&
                !predLogged(PredPassAttFormat)) {  // kept, not a target: which term (raw_gpu.cpp linearTarget)
                const raw::FormatInfo *f = raw::formatInfo(x->_raw.format);
                logPred(PredPassAttFormat, t.pixelFormat, b,
                        f && f->rt
                            ? [NSString stringWithFormat:@"(kept, not a target: %ux%u pitch %u va&127 %llu, a pitch "
                                                         @"target needs both multiples of 128)",
                                                         x->_raw.width, x->_raw.height, x->_raw.pitch,
                                                         (unsigned long long)(x->_raw.va & 127)]
                            : @"(kept, of a format m2v's NVK renders none of)");
            }
            hit(self, PredPassAttFormat, t.pixelFormat, b);
        } else if ((x->_raw.planar && !x->_raw.planarStandIn) || x->_raw.ycbcr) {
            [self takeReason:WhyPass why:@"a planar texture as a render target"];
            hit(self, PredEncPlanarRT, t.pixelFormat, c);
        } else if (pass.colorAttachments[c].slice || pass.colorAttachments[c].depthPlane ||
                   pass.colorAttachments[c].resolveTexture ||
                   (pass.colorAttachments[c].level &&
                    (!x->_raw.chain || pass.colorAttachments[c].level >= x->_raw.chain->layout.levels))) {
            // A slice or depth plane needs an array or 3D texture and a resolve a multisampled one, none of which the raw
            // path keeps (rawShape)
            [self takeReason:WhyPass why:@"a color attachment slice, depth plane or resolve"];
            hit(self, PredPassAttLevel, pass.colorAttachments[c].level | pass.colorAttachments[c].slice << 8,
                pass.colorAttachments[c].resolveTexture != nil);
        } else {
            e->_formats[c] = x->_raw.planarStandIn ? x->_raw.planarStandIn->format : x->_raw.format;
            e->_count = c + 1;
        }
    }
    facts.depth = pass.depthAttachment.texture;
    facts.depthLoad = pass.depthAttachment.loadAction;
    facts.stencilLoad = pass.stencilAttachment.loadAction;
    facts.clearDepth = pass.depthAttachment.clearDepth;
    facts.clearStencil = pass.stencilAttachment.clearStencil;
    facts.width = pass.renderTargetWidth;
    facts.height = pass.renderTargetHeight;
    if (pass.depthAttachment.texture || pass.stencilAttachment.texture) {
        [self retain:pass.depthAttachment.texture];
        [self retain:pass.stencilAttachment.texture];
        if (rawDepthStencil(pass))
            e->_zeta = raw::kZetaFormat;
        else {
            [self takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
            hit(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                pass.stencilAttachment.texture.pixelFormat);
        }
    }
    // renderTargetWidth / renderTargetHeight: the render area's bounds (raw::PassDesc width, height)
    // A visibility result buffer is written only by draws under a counting mode (setVisibilityResultMode:offset:, refused there),
    // as m2v copies results only where a query was taken (visibility.rs plan)
    [self retain:pass.visibilityResultBuffer];
    if (pass.renderTargetArrayLength > 1 || pass.rasterizationRateMap) {
        [self takeReason:WhyPass why:@"layers or a rate map"];
        hit(self, PredPassOther, pass.renderTargetArrayLength, pass.rasterizationRateMap != nil);
    }
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    m.x = e->_passAt;
    [self record:m];
    return (id<MTLRenderCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    RMBlitEncoder *e = [RMBlitEncoder new];
    [self begin:e];
    [self record:cmd(OpBlit)];
    return (id<MTLBlitCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d
{
    (void)d;
    return [self blitCommandEncoder];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    RMComputeEncoder *e = [RMComputeEncoder new];
    [self begin:e];
    Cmd m = cmd(OpCompute);
    m.a = uint32_t(type);
    [self record:m];
    [self takeReason:WhyCompute why:@"a compute encoder"];
    hit(self, PredCompute, type);
    return (id<MTLComputeCommandEncoder>)e;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    return [self computeCommandEncoderWithDispatchType:d.dispatchType];
}

@end
