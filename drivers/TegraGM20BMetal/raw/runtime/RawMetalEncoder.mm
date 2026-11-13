// SPDX-License-Identifier: MIT
#import "RawMetalCounters.h"
#include "raw_object_pool.h"
#import "RawMetalMultisample.h"
#import "RawMetalPassCache.h"
#import "RawMetalRotated.h"
#import "RawMetalTargetViews.h"
#include "raw_device_answers.h"

// One count for every kind, as IOAccelDeviceGetNextGlobalTraceID is the device's
uint64_t nextTraceId()
{
    static std::atomic<uint64_t> ids{0};
    return ids.fetch_add(1, std::memory_order_relaxed) + 1;
}

@implementation RMEncoder

- (uint64_t)globalTraceObjectID
{
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

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
// invocation (objects mapped to twins at replay) and the command buffer goes to m2v; answered zero here when the m2v encoder
// has no method for it either (its answer is zero too).
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
    if (!twinAnswers([[self class] twinClass], inv.selector)) {
        answerZero(self, inv);
        return;
    }
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

// One its caller dropped without ending it is ended here, so the tape's encoders stay closed: a check with no message
// (an ended one, every pass's, sends none)
static void endDropped(void *object)
{
    RMEncoder *__unsafe_unretained e = (__bridge RMEncoder *)object;
    if (!e->_ended && e->_cb)
        [e endEncoding];
}

- (void)dealloc
{
    endDropped((__bridge void *)self);
}

- (void)endEncoding
{
    SWITCH_API(ApiEncoder);
    if (_ended)
        return;
    _ended = true;
    if (!_counterPending.empty())
        rmPlaceCounterMarks(_cb, self, _cb->_plan.size(), false);
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

// The pass's depth/stencil texture: its depth attachment's, else its stencil attachment's
static id<MTLTexture> zetaAttachment(MTLRenderPassDescriptor *pass)
{
    return pass.depthAttachment.texture ?: pass.stencilAttachment.texture;
}

// A depth/stencil attachment the raw path renders (raw::DepthStencilAttachment): one zeta texture (raw_zeta.h) as each aspect its
// format has and no other (Stencil8 a stencil attachment alone), whole (level, slice and depth plane 0), a clear depth that is
// a number (metal2vulkan refuses a NaN); a memoryless one cleared or not loaded and not stored, a stored one loaded and stored
// as asked, each aspect's resolve one the raw path draws
static bool resolves(MTLStoreAction a)
{
    return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
}

// What a store action keeps of the attachment itself: a resolve's as DontCare (MultisampleResolve) or Store
static MTLStoreAction keptStore(MTLStoreAction a)
{
    return !resolves(a) ? a : a == MTLStoreActionMultisampleResolve ? MTLStoreActionDontCare : MTLStoreActionStore;
}

// An aspect's resolve into level, slice and depth plane 0 of a one-sample texture of the attachment's format
static bool zetaResolveTarget(MTLRenderPassAttachmentDescriptor *a, RMTexture *from)
{
    RMTexture *r = (RMTexture *)a.resolveTexture;
    return from->_raw.samples > 1 && [(id)r isKindOfClass:[RMTexture class]] && r->_rawOk && r->_raw.zeta &&
           r->_raw.samples == 1 && !r->_raw.memoryless && r->_raw.format == from->_raw.format && !a.resolveLevel &&
           !a.resolveSlice && !a.resolveDepthPlane;
}

static raw::StencilSample stencilPick(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    return raw::stencilResolveSample(uint32_t(pass.stencilAttachment.stencilResolveFilter),
                                     uint32_t(d.depthResolveFilter), d.texture != nil, resolves(d.storeAction));
}

// A depth resolve the raw path draws after the pass (the pack's depth resolve state of its format and filter), and a stencil
// resolve (raw_stencil_bits.h: a draw a bit of the pack's stencil bits state)
static bool rawDepthResolve(MTLRenderPassDepthAttachmentDescriptor *d, RMTexture *from)
{
    return zetaResolveTarget(d, from) &&
           gRM->raw->depthResolvePipeline(from->_raw.format, uint32_t(d.depthResolveFilter));
}

static bool rawStencilResolve(MTLRenderPassDescriptor *pass, RMTexture *from)
{
    return zetaResolveTarget(pass.stencilAttachment, from) &&
           gRM->raw->stencilBitsPipeline(from->_raw.format, from->_raw.samples, stencilPick(pass));
}

static bool rawDepthStencil(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    RMTexture *t = (RMTexture *)zetaAttachment(pass);
    if (!t || ![(id)t isKindOfClass:[RMTexture class]] || !t->_rawOk || !t->_raw.zeta)
        return false;
    const bool depth = raw::zetaHasDepth(t->_raw.format), stencil = raw::zetaHasStencil(t->_raw.format);
    if ((RMTexture *)d.texture != (depth ? t : nil) || (RMTexture *)s.texture != (stencil ? t : nil) ||
        (depth && std::isnan(d.clearDepth)))
        return false;
    if (depth && (d.resolveTexture || resolves(d.storeAction)) && !rawDepthResolve(d, t))
        return false;
    if (stencil && (s.resolveTexture || resolves(s.storeAction)) && !rawStencilResolve(pass, t))
        return false;
    MTLRenderPassAttachmentDescriptor *aspects[2] = {depth ? d : nil, stencil ? s : nil};
    const NSUInteger slice = depth ? d.slice : s.slice;
    for (MTLRenderPassAttachmentDescriptor *a : aspects) {
        const MTLStoreAction kept = keptStore(a.storeAction);
        if (a && (a.level || a.slice != slice || slice >= std::max(t->_raw.layers, 1u) || a.depthPlane ||
                  (t->_raw.memoryless && (kept != MTLStoreActionDontCare || (a.loadAction != MTLLoadActionClear &&
                                                                             a.loadAction != MTLLoadActionDontCare))) ||
                  (kept != MTLStoreActionDontCare && kept != MTLStoreActionStore)))
            return false;
    }
    return true;
}

// A resolve the raw path draws after the pass (the pack's resolve state of its format, RawMetalEncode.mm): of a multisampled
// attachment into a level, slice or depth plane of a one-sample texture of its format the raw path renders to (the
// attachment's own target, raw_attachment_target.h)
// A probe's command buffer that resolves by the 2D engine (hw_test_app gpu.x1.twod_resolve) needs no resolve state
static bool resolvesByTwoD(RMCommandBuffer *cb, uint32_t format)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return cb->_probeForms.resolveTwoD && raw::probeTwodFormat(format);
#else
    (void)cb, (void)format;
    return false;
#endif
}

static bool rawResolve(RMCommandBuffer *cb, MTLRenderPassColorAttachmentDescriptor *a, RMTexture *from,
                       RMPass::Color &facts)
{
    RMTexture *r = (RMTexture *)a.resolveTexture;
    if (from->_raw.samples <= 1 || ![(id)r isKindOfClass:[RMTexture class]] || !r->_rawOk ||
        !(r->_raw.renderable || r->_raw.shadowTarget) || r->_raw.planar || r->_raw.ycbcr || r->_raw.samples != 1 ||
        r->_raw.format != from->_raw.format ||
        !(gRM->raw->resolvePipeline(from->_raw.format, from->_raw.samples) || resolvesByTwoD(cb, from->_raw.format)))
        return false;
    const bool volume = r->_raw.type == raw::fx::kTexThreeD;
    facts.resolveLevel = a.resolveLevel;
    facts.resolveSlice = volume ? a.resolveDepthPlane : a.resolveSlice;
    raw::Texture part;
    return !(volume ? a.resolveSlice : a.resolveDepthPlane) && facts.resolveLevel < raw::fx::kMaxLevels &&
           facts.resolveSlice <= UINT32_MAX &&
           (raw::wholeAttachment(r->_raw, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice)) ||
            rawAttachmentPart(r, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice), part));
}

// Whether the pass draws RGB10A8_2P_XR10 in its two planes (raw_xr_pass.h); a pass of a 550 beside one it cannot draw so is
// refused (pass.fmt, b 0x550 << 8 | attachment)
static void xrPlanesPassOf(RMCommandBuffer *cb, RMRenderEncoder *e, const raw::Texture *const colors[8])
{
    bool planes = false;
    if (const char *refused = raw::xrPlanesPass(colors, planes)) {
        [cb takeReason:WhyPass why:@(refused)];
        SWITCH_REFUSE_DETAIL("raw", "xr.planes_pass", 0, "%s: m2v's", refused);
        hit(cb, PredPassAttFormat, 550, uint64_t(0x550) << 8);
    }
    e->_xrPlanes = planes;
}

// A pass of the contents of one opened before (RawMetalPassCache.h): its copy and facts, and what each pass of a command
// buffer does again: the objects it names retained, its targets' GPU writes declared, their compression as it is now
static id<MTLRenderCommandEncoder> openKeptPass(RMCommandBuffer *cb, RMRenderEncoder *e, const RMPassEntry &kept)
{
    MTLRenderPassDescriptor *pass = kept.pass;
    [cb retain:pass];
    e->_pass = pass;
    e->_passShared = true;
    e->_passAt = uint32_t(cb->_passes.size());
    RMPass &facts = cb->_passes.emplace_back(kept.facts);
    facts.compressed = 0;
    const raw::Texture *colors[8] = {};
    for (unsigned c = 0; c < 8; c++) {
        id t = facts.color[c].texture;
        if (!t)
            continue;
        [cb retain:t];
        [cb retain:facts.color[c].resolve];
        if (RMTexture *x = (RMTexture *)t; [(id)x isKindOfClass:[RMTexture class]]) {
            declareGpuWrites(x);
            if (x->_rawOk)
                colors[c] = &x->_raw;
            if (x->_rawOk && x->_raw.comptags)
                facts.compressed |= uint8_t(1u << c);
        }
        if (RMTexture *r = (RMTexture *)facts.color[c].resolve; [(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
    }
    [cb retain:facts.zeta[0].texture];
    [cb retain:facts.zeta[1].texture];
    if (RMTexture *r = (RMTexture *)facts.depthResolve) {
        [cb retain:r];
        if ([(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
    }
    if (RMTexture *r = (RMTexture *)facts.stencilResolve) {
        [cb retain:r];
        if ([(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
    }
    xrPlanesPassOf(cb, e, colors);
    e->_visibility = kept.visibility;
    [cb retain:kept.visibility];
    std::copy(kept.formats.begin(), kept.formats.end(), e->_formats);
    e->_count = kept.count;
    e->_zeta = kept.zeta;
    e->_samples = kept.samples;
    e->_turn = kept.turn;
    e->_renderAt = cb->_plan.size();
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    m.x = e->_passAt;
    [cb record:m];
    return (id<MTLRenderCommandEncoder>)e;
}

// The encoder's own copy of its pass where it shares the thread's (it is about to change it)
void rmOwnPass(RMRenderEncoder *e)
{
    if (!e->_passShared)
        return;
    MTLRenderPassDescriptor *own = [e->_pass copy];
    [e->_cb retain:own];
    e->_pass = own;
    e->_passShared = false;
    e->_cb->_plan.setDescriptor(e->_renderAt, (__bridge void *)own);
}

void rmPoolEncoders()
{
    for (Class cls : {[RMRenderEncoder class], [RMBlitEncoder class], [RMComputeEncoder class]})
        if (!rawPoolInstances(cls, endDropped))
            os_log_error(rawLog(), "raw: %{public}s instances are not pooled", class_getName(cls));
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
    _encAt = _plan.size();  // its first command is the next one recorded
    _encOpen = true;
    _encRefused = false;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    RMRenderEncoder *e = [RMRenderEncoder new];
    [self begin:e];
    thread_local std::vector<uint8_t> key;
    uint64_t hash = 0;
    const bool keyed = !rmCounterBuffersMade() && rmPassKey(d, key, hash);
    if (keyed)
        if (const RMPassEntry *kept = rmPassFind(key, hash))
            return openKeptPass(self, e, *kept);
    MTLRenderPassDescriptor *pass = [d copy];
    [self retain:pass];
    e->_pass = pass;
    e->_passAt = uint32_t(_passes.size());
    RMPass &facts = _passes.emplace_back();
    e->_samples = 1;
    const raw::Texture *colors[8] = {};
    for (unsigned c = 0; c < 8; c++) {
        MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[c];
        id<MTLTexture> t = a.texture;
        if (!t)
            continue;
        id<MTLTexture> resolve = a.resolveTexture;
        const bool volume = t.textureType == MTLTextureType3D;
        const NSUInteger slice = volume ? a.depthPlane : a.slice;
        facts.color[c] = {t, a.level, slice, a.loadAction, a.storeAction, a.clearColor, resolve, 0, 0};
        if ([(id)t isKindOfClass:[RMTexture class]] && ((RMTexture *)t)->_rawOk && ((RMTexture *)t)->_raw.comptags)
            facts.compressed |= uint8_t(1u << c);
        [self retain:t];
        [self retain:resolve];
        RMTexture *x = (RMTexture *)t;
        if ([(id)x isKindOfClass:[RMTexture class]]) {
            declareGpuWrites(x);
            if (x->_rawOk)
                colors[c] = &x->_raw;
        }
        if (RMTexture *r = (RMTexture *)resolve; [(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
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
        } else if ((x->_raw.planar && !x->_raw.planarStandIn) ||
                   (x->_raw.ycbcr && ycbcrTargetPassRefusal(x->_raw, a))) {
            [self takeReason:WhyPass why:@"a planar texture as a render target"];
            hit(self, PredEncPlanarRT, t.pixelFormat, c | uint64_t(a.loadAction) << 8);
        } else if (raw::Texture part; (volume ? a.slice : a.depthPlane) || a.level >= raw::fx::kMaxLevels ||
                                      slice > UINT32_MAX ||
                                      (resolve && (!raw::wholeAttachment(x->_raw, uint32_t(a.level), uint32_t(slice)) ||
                                                   !rawResolve(self, a, x, facts.color[c]))) ||
                                      (!raw::wholeAttachment(x->_raw, uint32_t(a.level), uint32_t(slice)) &&
                                       !rawAttachmentPart(x, uint32_t(a.level), uint32_t(slice), part))) {
            // a level, slice or depth plane is a block-linear image's (raw_attachment_target.h), resolved from no layer
            [self takeReason:WhyPass why:@"a color attachment level, slice, depth plane or resolve"];
            hit(self, PredPassAttLevel, a.level | slice << 8, resolve != nil);
        } else {
            e->_formats[c] = x->_raw.planarStandIn ? x->_raw.planarStandIn->format : x->_raw.format;
            e->_count = c + 1;
            e->_samples = x->_raw.samples;
        }
    }
    xrPlanesPassOf(self, e, colors);
    facts.depth = zetaAttachment(pass);
    facts.depthSlice = pass.depthAttachment.texture ? pass.depthAttachment.slice : pass.stencilAttachment.slice;
    facts.depthLoad = pass.depthAttachment.loadAction;
    facts.stencilLoad = pass.stencilAttachment.loadAction;
    MTLRenderPassDepthAttachmentDescriptor *depth = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *stencil = pass.stencilAttachment;
    facts.zeta[0] = {depth.texture,    depth.level,       depth.slice,
                     depth.loadAction, depth.storeAction, depth.resolveTexture != nil};
    facts.zeta[1] = {stencil.texture,    stencil.level,       stencil.slice,
                     stencil.loadAction, stencil.storeAction, stencil.resolveTexture != nil};
    e->_turn = passTurn(facts);
    if (const raw::RotatedRefusal turned = rotatedPassRefusal(facts); turned != raw::RotatedRefusal::None) {
        [self takeReason:WhyPass why:@(raw::rotatedRefusalText(turned))];
        hit(self, PredPassRotated, uint64_t(turned));
    }
    if (depth.texture && resolves(depth.storeAction) && depth.resolveTexture) {
        facts.depthResolve = depth.resolveTexture;
        facts.depthFilter = uint32_t(depth.depthResolveFilter);
        facts.zeta[0].store = keptStore(depth.storeAction);
        [self retain:facts.depthResolve];
        if (RMTexture *r = (RMTexture *)facts.depthResolve; [(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
    }
    if (stencil.texture && resolves(stencil.storeAction) && stencil.resolveTexture) {
        facts.stencilResolve = stencil.resolveTexture;
        facts.stencilPick = stencilPick(pass);
        facts.zeta[1].store = keptStore(stencil.storeAction);
        [self retain:facts.stencilResolve];
        if (RMTexture *r = (RMTexture *)facts.stencilResolve; [(id)r isKindOfClass:[RMTexture class]])
            declareGpuWrites(r);
    }
    facts.clearDepth = pass.depthAttachment.clearDepth;
    facts.clearStencil = pass.stencilAttachment.clearStencil;
    facts.width = pass.renderTargetWidth;
    facts.height = pass.renderTargetHeight;
    if (pass.depthAttachment.texture || pass.stencilAttachment.texture) {
        [self retain:pass.depthAttachment.texture];
        [self retain:pass.stencilAttachment.texture];
        if (rawDepthStencil(pass)) {
            e->_zeta = ((RMTexture *)facts.depth)->_raw.format;
            if (!e->_count)
                e->_samples = ((RMTexture *)facts.depth)->_raw.samples;
        } else {
            [self takeReason:WhyPass why:@"a depth/stencil attachment the raw path does not take"];
            hit(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                pass.stencilAttachment.texture.pixelFormat);
        }
    }
    // renderTargetWidth / renderTargetHeight: the render area's bounds (raw::PassDesc width, height)
    // A visibility result buffer is written only by draws under a counting mode (setVisibilityResultMode:offset:, refused there),
    // as m2v copies results only where a query was taken (visibility.rs plan)
    e->_visibility = pass.visibilityResultBuffer;
    [self retain:e->_visibility];
    const NSUInteger positions = [pass getSamplePositions:nullptr count:0];
    facts.layers = uint32_t(std::clamp<NSUInteger>(pass.renderTargetArrayLength, 1, 2048));
    if ((facts.layers > 1 && !raw::answers::kThisKind.layeredRendering) || pass.rasterizationRateMap ||
        (positions && !passSamplePositions(pass, positions, e->_samples, facts.positions))) {
        [self takeReason:WhyPass why:@"layers, a rate map or sample positions of its own"];
        hit(self, PredPassOther, pass.renderTargetArrayLength | positions << 32, pass.rasterizationRateMap != nil);
    }
    e->_renderAt = _plan.size();
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    m.x = e->_passAt;
    [self record:m];
    rmCounterMarks(self, e, pass);
    rmPlaceCounterMarks(self, e, _encAt, true);
    // A resolve's acceptance reads the command buffer's probe forms, so a pass that resolves is read again each time
    const bool resolves = std::any_of(std::begin(facts.color), std::end(facts.color), [](const RMPass::Color &c) {
        return c.resolve != nil;
    });
    if (keyed && !_encRefused && !resolves) {
        // what the raw encode and m2v's replay read of it made now, so the copy is read, never changed, after this
        for (unsigned c = 0; c < 8; c++)
            (void)pass.colorAttachments[c];
        (void)pass.sampleBufferAttachments;
        rmPassKeep(key, hash, pass, facts, e);
        e->_passShared = true;
    }
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
    RMBlitEncoder *e = (RMBlitEncoder *)[self blitCommandEncoder];
    rmCounterMarks(self, e, d);
    rmPlaceCounterMarks(self, e, _encAt, true);
    return (id<MTLBlitCommandEncoder>)e;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    RMComputeEncoder *e = [RMComputeEncoder new];
    e->_type = type;
    [self begin:e];
    Cmd m = cmd(OpCompute);
    m.a = uint32_t(type);
    [self record:m];
    return (id<MTLComputeCommandEncoder>)e;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    RMComputeEncoder *e = (RMComputeEncoder *)[self computeCommandEncoderWithDispatchType:d.dispatchType];
    rmCounterMarks(self, e, d);
    rmPlaceCounterMarks(self, e, _encAt, true);
    return (id<MTLComputeCommandEncoder>)e;
}

@end
