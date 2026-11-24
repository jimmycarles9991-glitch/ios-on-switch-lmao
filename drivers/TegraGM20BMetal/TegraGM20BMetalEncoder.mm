// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalCounters.h"
#import "TegraGM20BMetalMultisample.h"
#import "TegraGM20BMetalPassAttachments.h"
#import "TegraGM20BMetalPassCache.h"
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalTargetViews.h"
#include "format_standin.h"
#include "object_pool.h"
#include "planar_target.h"
#include "xr.h"
#include "zeta_colour.h"

// A pass of the contents of one opened before (TegraGM20BMetalPassCache.h): its copy and facts, and what each pass of a command
// buffer does again: the objects it names retained, its targets' GPU writes declared, their compression as it is now. A kept
// shape's key names only this device's textures and buffers (gm20bPassKey), so its objects' classes are known; the cache
// and the command buffer hold every object it names.
static id<MTLRenderCommandEncoder> openKeptPass(SwitchMetalCommandBuffer *cb, SwitchMetalRenderEncoder *e,
                                                const SwitchMetalPassEntry &kept)
{
    __unsafe_unretained MTLRenderPassDescriptor *pass = kept.pass;
    [cb retain:pass];
    e->_pass = pass;
    e->_passShared = true;
    e->_passAt = uint32_t(cb->_passes.size());
    SwitchMetalPass &facts = cb->_passes.emplace_back(kept.facts);
    facts.compressed = 0;
    for (unsigned c = 0; c < 8; c++) {
        __unsafe_unretained SwitchMetalTexture *x = (SwitchMetalTexture *)facts.color[c].texture;
        if (!x)
            continue;
        [cb retain:x];
        declareGpuWrites(x);
        if (x->_gm20bOk && gm20b::xrForm(x->_raw.format) != gm20b::XrForm::None && x->_raw.comptags)
            facts.compressed |= uint8_t(1u << c);
        if (__unsafe_unretained SwitchMetalTexture *r = (SwitchMetalTexture *)facts.color[c].resolve) {
            [cb retain:r];
            declareGpuWrites(r);
        }
    }
    for (const SwitchMetalPass::Zeta &z : facts.zeta)
        if (z.texture)
            [cb retain:z.texture];
    for (unsigned i = 0; i < 2; i++)
        if (__unsafe_unretained SwitchMetalTexture *r =
                (SwitchMetalTexture *)(i ? facts.stencilResolve : facts.depthResolve)) {
            [cb retain:r];
            declareGpuWrites(r);
        }
    e->_visibility = kept.visibility;
    if (kept.visibility)
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

// The encoder's own copy where it is about to change a pass shared with the recording's cache bank
void gm20bOwnPass(SwitchMetalRenderEncoder *e)
{
    if (!e->_passShared)
        return;
    SWITCH_METAL_DIAGNOSTIC(gm20b::frameCpuCounts().passOwned.fetch_add(1, std::memory_order_relaxed));
    gm20bPassBind(e->_cb, e->_pass);
    MTLRenderPassDescriptor *own = [e->_pass copy];
    [e->_cb retain:own];
    e->_pass = own;
    e->_passShared = false;
    e->_cb->_plan.setDescriptor(e->_renderAt, (__bridge void *)own);
}

@implementation SwitchMetalCommandBuffer (Encoders)

- (void)begin:(SwitchMetalEncoder *)e
{
    if (_active)
        [(SwitchMetalEncoder *)_active endEncoding];  // Metal: one encoder at a time
    e->_device = _device;
    e->_cb = self;
    _active = e;
    [self closeEncoder];
    _encAt = _plan.size();  // its first command is the next one recorded
    _encOpen = true;
    _encFailed = false;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    SwitchMetalRenderEncoder *e =
        (__bridge_transfer SwitchMetalRenderEncoder *)gm20bPoolNew([SwitchMetalRenderEncoder class]);
    [self begin:e];
    thread_local std::vector<uint8_t> key;
    uint64_t hash = 0;
    const bool counters = gm20bCounterBuffersMade();
    const bool keyed = !counters && gm20bPassKey(d, key, hash);
    if (keyed)
        if (const SwitchMetalPassEntry *kept = gm20bPassFind(self, key, hash)) {
            SWITCH_METAL_DIAGNOSTIC(gm20b::countPass(gm20b::PassOpen::Hit));
            return openKeptPass(self, e, *kept);
        }
    MTLRenderPassDescriptor *pass = [d copy];
    bool skipped = gm20bPassAsApple(self, pass);
    [self retain:pass];
    e->_pass = pass;
    e->_passAt = uint32_t(_passes.size());
    SwitchMetalPass &facts = _passes.emplace_back();
    e->_samples = 1;
    if (const char *rule = gm20bPassShapeInvalid(pass))
        gm20bPassInvalid(self, PredPassAttLevel, pass.renderTargetArrayLength, 0, rule);
    for (unsigned c = 0; c < 8; c++) {
        MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[c];
        id<MTLTexture> t = a.texture;
        if (!t)
            continue;
        id<MTLTexture> resolve = a.resolveTexture;
        const bool volume = t.textureType == MTLTextureType3D;
        const NSUInteger slice = volume ? a.depthPlane : a.slice;
        facts.color[c] = {t, a.level, slice, a.loadAction, a.storeAction, a.clearColor, resolve, 0, 0};
        // an extended-range texture's alone: no texture of another form may be moving on the submit worker, whose
        // memory and comptags this thread may not read (private_comp.h)
        if (SwitchMetalTexture *x = (SwitchMetalTexture *)t;
            [(id)x isKindOfClass:[SwitchMetalTexture class]] && x->_gm20bOk &&
            gm20b::xrForm(x->_raw.format) != gm20b::XrForm::None && x->_raw.comptags)
            facts.compressed |= uint8_t(1u << c);
        [self retain:t];
        [self retain:resolve];
        SwitchMetalTexture *x = (SwitchMetalTexture *)t;
        if ([(id)x isKindOfClass:[SwitchMetalTexture class]])
            declareGpuWrites(x);
        if (SwitchMetalTexture *r = (SwitchMetalTexture *)resolve; [(id)r isKindOfClass:[SwitchMetalTexture class]])
            declareGpuWrites(r);
        const bool standsIn = [(id)x isKindOfClass:[SwitchMetalTexture class]] && gm20bFormatTarget(x);
        const bool resolving = resolve && (gm20bResolvesStore(a.storeAction) || a.storeAction == MTLStoreActionUnknown);
        // a whole one-sample Depth32Float or Depth16Unorm draws as its colour twin (zeta_colour.h)
        const uint32_t depthColour = [(id)x isKindOfClass:[SwitchMetalTexture class]] && x->_gm20bOk && x->_raw.zeta &&
                                             x->_raw.samples <= 1 && !a.level && !slice
                                         ? gm20b::depthAsColourFormat(x->_raw.format)
                                         : 0;
        if (const char *rule = gm20bColorTargetInvalid(a)) {
            gm20bPassInvalid(self, PredPassAttFormat, t.pixelFormat, c, rule);
        } else if (const char *part = gm20bColorPartInvalid(a)) {
            gm20bPassInvalid(self, PredPassAttLevel, a.level | slice << 8, resolve != nil, part);
        } else if (!x->_gm20bOk) {
            // no creator hands out a texture the driver holds no texels of (each answers nil)
            gm20bPassFailed(self, PredPassAttFormat, t.pixelFormat, c,
                            @"a colour attachment of a texture the driver holds no texels of");
        } else if (x->_raw.zeta && !depthColour) {
            gm20bPassFailed(self, PredPassAttFormat, t.pixelFormat, c,
                            @"a level or slice of a depth texture as a colour attachment: no form built");
        } else if (const char *missing = depthColour ? nullptr : gm20bColorTargetMissing(x, a.level, slice)) {
            gm20bPassFailed(self, PredPassAttFormat, t.pixelFormat, c, @(missing));
        } else if (const char *why = gm20bPlanarTargetRefusal(x, a)) {
            gm20bPassFailed(self, PredEncPlanarRT, t.pixelFormat, c | uint64_t(a.loadAction) << 8, @(why));
        } else if (resolving && !gm20bRawResolve(self, a, x, facts.color[c])) {
            gm20bPassFailed(self, PredPassAttLevel, a.level | slice << 8, 1,
                            @"a resolve with no form built: no resolve state for its format and samples, a one-sample "
                            @"one of a turned or stand-in target, or one into a turned texture");
        } else if (const char *unbound = gm20bColorPartMissing(x, a.level, slice, resolving)) {
            gm20bPassFailed(self, PredPassAttLevel, a.level | slice << 8, resolve != nil, @(unbound));
        } else {
            e->_formats[c] = depthColour             ? depthColour
                             : x->_raw.planarStandIn ? x->_raw.planarStandIn->format
                             : x->_raw.planar        ? gm20b::kPlanarStandInFormat
                             : standsIn              ? gm20b::formatStandIn(x->_raw.format)
                                                     : x->_raw.format;
            e->_count = c + 1;
            e->_samples = x->_raw.samples;
        }
    }
    facts.depth = gm20bZetaAttachment(pass);
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
    const char *zetaInvalid = gm20bZetaAttachmentInvalid(pass);
    // every attachment of a format Metal renders, or depth/stencil of one Metal takes, moves through a turn copy state
    // (turn_copy.h, stencil_bits.h turnMovesZeta): a refusal is a state the driver's pack lacks
    if (const gm20b::RotatedRefusal turned = zetaInvalid ? gm20b::RotatedRefusal::None : rotatedPassRefusal(facts);
        turned != gm20b::RotatedRefusal::None)
        gm20bPassFailed(self, PredPassRotated, uint64_t(turned), 0, @(gm20b::rotatedRefusalText(turned)));
    if (depth.texture && gm20bResolvesStore(depth.storeAction) && depth.resolveTexture) {
        facts.depthResolve = depth.resolveTexture;
        facts.depthFilter = uint32_t(depth.depthResolveFilter);
        facts.depthResolveLevel = depth.resolveLevel;
        facts.depthResolveSlice = depth.resolveSlice;
        facts.zeta[0].store = gm20bKeptStore(depth.storeAction);
        [self retain:facts.depthResolve];
        if (SwitchMetalTexture *r = (SwitchMetalTexture *)facts.depthResolve;
            [(id)r isKindOfClass:[SwitchMetalTexture class]])
            declareGpuWrites(r);
    }
    if (stencil.texture && gm20bResolvesStore(stencil.storeAction) && stencil.resolveTexture) {
        facts.stencilResolve = stencil.resolveTexture;
        facts.stencilPick = gm20bStencilPick(pass);
        facts.stencilResolveLevel = stencil.resolveLevel;
        facts.stencilResolveSlice = stencil.resolveSlice;
        facts.zeta[1].store = gm20bKeptStore(stencil.storeAction);
        [self retain:facts.stencilResolve];
        if (SwitchMetalTexture *r = (SwitchMetalTexture *)facts.stencilResolve;
            [(id)r isKindOfClass:[SwitchMetalTexture class]])
            declareGpuWrites(r);
    }
    facts.clearDepth = pass.depthAttachment.clearDepth;
    facts.clearStencil = pass.stencilAttachment.clearStencil;
    facts.width = pass.renderTargetWidth;
    facts.height = pass.renderTargetHeight;
    if (pass.depthAttachment.texture || pass.stencilAttachment.texture) {
        [self retain:pass.depthAttachment.texture];
        [self retain:pass.stencilAttachment.texture];
        // a resolved aspect is multisampled, so its pass is never a turned plane's (one sample, gm20bPassShapeInvalid): no
        // turned stand-in of a separate pair is in place when a resolve reads it
        const bool separate = gm20bSeparateZeta(pass);
        const char *unbuilt = nullptr;
        const SwitchMetalZetaForm form =
            zetaInvalid || _encFailed ? SwitchMetalZetaForm::Raw : gm20bRawDepthStencil(pass, separate, &unbuilt);
        if (zetaInvalid) {
            gm20bPassInvalid(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                             pass.stencilAttachment.texture.pixelFormat, zetaInvalid);
        } else if (_encFailed) {
            // failed already: nothing of the pass is encoded
        } else if (gm20bZetaResolvesIntoMemoryless(pass)) {
            gm20bPassFailed(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                            pass.stencilAttachment.texture.pixelFormat,
                            @"a depth or stencil resolve into a memoryless texture, which Apple's GPU fails",
                            MTLCommandBufferErrorInvalidResource);
        } else if (form == SwitchMetalZetaForm::Unbuilt) {
            gm20bPassFailed(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                            pass.stencilAttachment.texture.pixelFormat, @(unbuilt));
        } else if (form == SwitchMetalZetaForm::Raw) {
            e->_zeta = separate ? gm20b::kZetaFormat
                       : gm20b::colourAsDepth(((SwitchMetalTexture *)facts.depth)->_raw)
                           ? gm20b::kColourAsDepthFormat
                           : ((SwitchMetalTexture *)facts.depth)->_raw.format;
            if (!e->_count)
                e->_samples = ((SwitchMetalTexture *)facts.depth)->_raw.samples;
            if (separate &&
                (e->_turn || ((SwitchMetalTexture *)depth.texture)->_raw.format == MTLPixelFormatDepth16Unorm)) {
                for (unsigned i = 0; i < (e->_turn ? 2u : 1u); ++i) {
                    SwitchMetalPass::Zeta &aspect = facts.zeta[i];
                    if (aspect.load != MTLLoadActionLoad)
                        continue;
                    __unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)aspect.texture;
                    bool prepared;
                    if (!aspect.level && !aspect.slice) {
                        prepared = gRM->raw->sampleable(t->_raw);
                    } else {
                        gm20b::Texture selected;
                        if (aspect.level) {
                            __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
                            gm20b::chainLevelImage(owner->_raw, t->_levelsBase + uint32_t(aspect.level), selected);
                            selected = gm20b::layersFrom(selected, t->_slicesBase + uint32_t(aspect.slice));
                        } else {
                            selected = gm20b::layersFrom(t->_raw, uint32_t(aspect.slice));
                        }
                        aspect.sourceTic =
                            gRM->raw->attachmentTic(t->_raw, selected, uint32_t(aspect.level), uint32_t(aspect.slice));
                        prepared = aspect.sourceTic != 0;
                    }
                    if (!prepared) {
                        _encFailed = true;
                        failCommandBuffer(self, @"no texture header for a depth/stencil load",
                                          MTLCommandBufferErrorOutOfMemory);
                        break;
                    }
                }
            }
        } else {
            // no creator hands out a texture of a depth/stencil format without its raw zeta
            gm20bPassFailed(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                            pass.stencilAttachment.texture.pixelFormat,
                            @"a depth/stencil attachment of a texture the driver holds no zeta of");
        }
    }
    // renderTargetWidth / renderTargetHeight: the render area's bounds (gm20b::PassDesc width, height)
    // A visibility result buffer is written only by draws under a counting mode (setVisibilityResultMode:offset:, refused there),
    // as m2v copies results only where a query was taken (visibility.rs plan)
    e->_visibility = pass.visibilityResultBuffer;
    [self retain:e->_visibility];
    const NSUInteger positions = [pass getSamplePositions:nullptr count:0];
    facts.layers = uint32_t(std::clamp<NSUInteger>(pass.renderTargetArrayLength, 1, 2048));
    // A device without rate maps draws such a pass at full rate, the map ignored, its command buffer completed (Metal's
    // iOS simulator GPU, which answers supportsRasterizationRateMapWithLayerCount: NO, given a pass carrying one)
    if (pass.rasterizationRateMap)
        SWITCH_REFUSE("gm20b", "pass.ratemap",
                      "a pass with a rasterization rate map: drawn at full rate, as Metal draws it on "
                      "a device without rate maps");
    if (positions && !passSamplePositions(pass, positions, e->_samples, facts.positions)) {
        skip(self, PredPassPositions, positions, e->_samples,
             "sample positions past the pass's samples: drawn at the standard ones");
        skipped = true;
    }
    e->_renderAt = _plan.size();
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    m.x = e->_passAt;
    [self record:m];
    gm20bCounterMarks(self, e, pass);
    gm20bPlaceCounterMarks(self, e, _encAt, true);
    // A resolve's acceptance reads the command buffer's probe forms, so a pass that resolves is read again each time
    const bool resolves =
        std::any_of(std::begin(facts.color), std::end(facts.color), [](const SwitchMetalPass::Color &c) {
            return c.resolve != nil;
        });
    // a pass whose making counted a skip is made again at each call, which counts it again
    const bool keeps = keyed && !_encFailed && !resolves && !skipped;
    SWITCH_METAL_DIAGNOSTIC(gm20b::countPass(keeps      ? gm20b::PassOpen::Kept
                                             : keyed    ? gm20b::PassOpen::Unkept
                                             : counters ? gm20b::PassOpen::Counters
                                                        : gm20b::PassOpen::Unkeyed));
    if (keeps) {
        // what the raw encode reads of it made now, so the copy is read, never changed, after this
        for (unsigned c = 0; c < 8; c++)
            (void)pass.colorAttachments[c];
        (void)pass.sampleBufferAttachments;
        gm20bPassKeep(self, key, hash, pass, facts, e);
        e->_passShared = true;
    }
    return (id<MTLRenderCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    SwitchMetalBlitEncoder *e = [SwitchMetalBlitEncoder new];
    [self begin:e];
    [self record:cmd(OpBlit)];
    return (id<MTLBlitCommandEncoder>)e;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d
{
    SwitchMetalBlitEncoder *e = (SwitchMetalBlitEncoder *)[self blitCommandEncoder];
    gm20bCounterMarks(self, e, d);
    gm20bPlaceCounterMarks(self, e, _encAt, true);
    return (id<MTLBlitCommandEncoder>)e;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    SwitchMetalComputeEncoder *e = [SwitchMetalComputeEncoder new];
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
    SwitchMetalComputeEncoder *e =
        (SwitchMetalComputeEncoder *)[self computeCommandEncoderWithDispatchType:d.dispatchType];
    gm20bCounterMarks(self, e, d);
    gm20bPlaceCounterMarks(self, e, _encAt, true);
    return (id<MTLComputeCommandEncoder>)e;
}

@end
