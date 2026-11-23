// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPassAttachments.h"
#import "TegraGM20BMetalMultisample.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalTargetViews.h"
#include "attachment_target.h"
#include "format_standin.h"
#include "probe_ops.h"
#include "stencil_bits.h"
#include "zeta_colour.h"

id<MTLTexture> gm20bZetaAttachment(MTLRenderPassDescriptor *pass)
{
    return pass.depthAttachment.texture ?: pass.stencilAttachment.texture;
}

bool gm20bResolvesStore(MTLStoreAction a)
{
    return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
}

MTLStoreAction gm20bKeptStore(MTLStoreAction a)
{
    if (!gm20bResolvesStore(a))
        return a;
    return a == MTLStoreActionMultisampleResolve ? MTLStoreActionDontCare : MTLStoreActionStore;
}

const gm20b::Texture *gm20bZetaPart(SwitchMetalTexture *t, NSUInteger level, NSUInteger slice, gm20b::Texture &part)
{
    if ((!level && !slice) || t->_raw.memoryless)
        return &t->_raw;
    if (!level) {
        part = gm20b::layersFrom(t->_raw, uint32_t(slice));
        return &part;
    }
    SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    if (!gm20b::chainLevelImage(owner->_raw, t->_levelsBase + uint32_t(level), part))
        return nullptr;
    part = gm20b::layersFrom(part, t->_slicesBase + uint32_t(slice));
    part.layers = std::max(t->_raw.layers, 1u) - uint32_t(slice);
    return &part;
}

uint32_t gm20bZetaPartTic(SwitchMetalTexture *t, NSUInteger level, NSUInteger slice, const gm20b::Texture &part)
{
    SwitchMetalTexture *owner = level ? t->_levelsOwner ?: t : t;
    const uint32_t base = level ? t->_levelsBase : 0, layer = level ? t->_slicesBase : 0;
    return gRM->raw->attachmentTic(owner->_raw, part, base + uint32_t(level), layer + uint32_t(slice));
}

// An aspect's resolve into a level and slice of a one-sample texture holding the aspect, of any zeta format: an M4 Max
// resolves a Depth32Float_Stencil8's depth into a Depth32Float, a Depth32Float's into a Depth32Float_Stencil8 (its stencil
// kept) and a Depth32Float_Stencil8's stencil into a Stencil8, into a level or a slice too (harness/passformsoracle.mm
// zeta_resolves). A memoryless one fails the pass first (gm20bZetaResolvesIntoMemoryless).
static bool zetaResolveTarget(MTLRenderPassAttachmentDescriptor *a, SwitchMetalTexture *from)
{
    SwitchMetalTexture *r = (SwitchMetalTexture *)a.resolveTexture;
    gm20b::Texture part;
    return from->_raw.samples > 1 && [(id)r isKindOfClass:[SwitchMetalTexture class]] && r->_gm20bOk && r->_raw.zeta &&
           r->_raw.samples == 1 && !r->_raw.memoryless && !a.resolveDepthPlane &&
           gm20bZetaPart(r, a.resolveLevel, a.resolveSlice, part);
}

gm20b::StencilSample gm20bStencilPick(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    return gm20b::stencilResolveSample(uint32_t(pass.stencilAttachment.stencilResolveFilter),
                                       uint32_t(d.depthResolveFilter), d.texture != nil,
                                       gm20bResolvesStore(d.storeAction));
}

// A depth resolve the driver draws after the pass (the pack's depth resolve state of its format and filter), and a stencil
// resolve (stencil_bits.h: a draw a bit of the pack's stencil bits state)
static bool gm20bDepthResolve(MTLRenderPassDepthAttachmentDescriptor *d, SwitchMetalTexture *from)
{
    return zetaResolveTarget(d, from) &&
           gRM->raw->depthResolvePipeline(((SwitchMetalTexture *)d.resolveTexture)->_raw.format,
                                          uint32_t(d.depthResolveFilter));
}

// A separate pair's stencil resolve reads the pass's Depth32Float_Stencil8 (TegraGM20BMetalRangeForms.mm separateZeta)
static bool gm20bStencilResolve(MTLRenderPassDescriptor *pass, SwitchMetalTexture *from, bool separate)
{
    const uint32_t read = separate ? gm20b::kZetaFormat : from->_raw.format;
    return zetaResolveTarget(pass.stencilAttachment, from) &&
           gRM->raw->stencilBitsPipeline(read, from->_raw.samples, gm20bStencilPick(pass));
}

bool gm20bSeparateZeta(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    return d.texture && s.texture && (d.texture != s.texture || d.level != s.level || d.slice != s.slice);
}

// Why `t`'s level `level`, slice `slice` is no zeta a pass renders whole aspects of, null where it is one
static const char *zetaPartMissing(SwitchMetalTexture *t, NSUInteger level, NSUInteger slice)
{
    gm20b::Texture part;
    if (!gm20bZetaPart(t, level, slice, part))
        return "a depth/stencil level of a texture whose levels the driver holds no chain of";
    return nullptr;
}

SwitchMetalZetaForm gm20bRawDepthStencil(MTLRenderPassDescriptor *pass, bool separate, const char **why)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    // a colour texture as the depth attachment alone, whole (zeta_colour.h colourAsDepth)
    const bool colourDepth = !separate && d.texture && !s.texture && !d.level && !d.slice &&
                             ((SwitchMetalTexture *)d.texture)->_gm20bOk &&
                             gm20b::colourAsDepth(((SwitchMetalTexture *)d.texture)->_raw);
    for (id<MTLTexture> x : {d.texture, s.texture})
        if (x && !colourDepth && (!((SwitchMetalTexture *)x)->_gm20bOk || !((SwitchMetalTexture *)x)->_raw.zeta))
            return SwitchMetalZetaForm::NotRaw;
    SwitchMetalTexture *t = (SwitchMetalTexture *)gm20bZetaAttachment(pass);
    const bool depth = separate || colourDepth || gm20b::zetaHasDepth(t->_raw.format),
               stencil = separate || gm20b::zetaHasStencil(t->_raw.format);
    *why = nullptr;
    if (separate) {
        const gm20b::Texture &x = ((SwitchMetalTexture *)d.texture)->_raw;
        const bool transient = d.loadAction != MTLLoadActionLoad &&
                               gm20bKeptStore(d.storeAction) != MTLStoreActionStore &&
                               !gm20bResolvesStore(d.storeAction);
        if (x.format == MTLPixelFormatDepth16Unorm && x.samples > 1 && !transient &&
            !gRM->raw->depthConvertible(x.samples))
            *why = "a multisampled Depth16Unorm loaded or kept beside a stencil texture, of a sample count the pack "
                   "has no conversion states of";
    } else if ((d.texture && !depth) || (s.texture && !stencil)) {
        *why = "a depth/stencil texture as an aspect its format has none of";
    }
    // a separate pair's resolved depth is stored back into its own texture, which the resolve reads; a memoryless depth and
    // a stencil are resolved from the pass's Depth32Float_Stencil8 itself
    SwitchMetalTexture *depthOwn = separate ? (SwitchMetalTexture *)d.texture : t,
                       *stencilOwn = separate ? (SwitchMetalTexture *)s.texture : t;
    if (!*why && d.texture && d.resolveTexture && gm20bResolvesStore(d.storeAction) && !gm20bDepthResolve(d, depthOwn))
        *why = "a depth resolve into a multisampled texture, or of a format and filter the pack has no state of";
    if (!*why && s.texture && s.resolveTexture && gm20bResolvesStore(s.storeAction) &&
        !gm20bStencilResolve(pass, stencilOwn, separate))
        *why = "a stencil resolve into a multisampled texture, or of a sample count the pack has no state of";
    for (MTLRenderPassAttachmentDescriptor *a :
         {(MTLRenderPassAttachmentDescriptor *)d, (MTLRenderPassAttachmentDescriptor *)s})
        if (!*why && a.texture && !colourDepth)
            *why = zetaPartMissing((SwitchMetalTexture *)a.texture, a.level, a.slice);
    return *why ? SwitchMetalZetaForm::Unbuilt : SwitchMetalZetaForm::Raw;
}

// A probe's command buffer that resolves by the 2D engine (hw_test_app gpu.x1.twod_resolve) needs no resolve state
static bool resolvesByTwoD(SwitchMetalCommandBuffer *cb, uint32_t format)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return cb->_probeForms.resolveTwoD && gm20b::probeTwodFormat(format);
#else
    (void)cb, (void)format;
    return false;
#endif
}

// A stand-in attachment's resolve (attachment_target.h formatResolveState): into a stand-in of the resolve texture's by
// its stand-in format's resolve or the format's own state, or for one sample a copy of its stand-in, then stored
static bool standInResolve(const gm20b::Texture &from, SwitchMetalTexture *r)
{
    const uint32_t standIn = gm20b::formatStandIn(from.format);
    if (!gm20bFormatTarget(r) || r->_raw.samples != 1)
        return false;
    if (from.samples <= 1)
        return true;
    const char *own = gm20b::formatResolveState(from.format);
    return own ? gRM->raw->ownPipeline(own) != nullptr : gRM->raw->resolvePipeline(standIn, from.samples) != nullptr;
}

bool gm20bRawResolve(SwitchMetalCommandBuffer *cb, MTLRenderPassColorAttachmentDescriptor *a, SwitchMetalTexture *from,
                     SwitchMetalPass::Color &facts)
{
    SwitchMetalTexture *r = (SwitchMetalTexture *)a.resolveTexture;
    if (![(id)r isKindOfClass:[SwitchMetalTexture class]] || !r->_gm20bOk || r->_raw.planar || r->_raw.ycbcr ||
        !gm20b::resolvesInto(from->_raw.format, r->_raw.format))
        return false;
    // a multisampled resolve texture takes the resolve in each of its samples (TegraGM20BMetalRangeSeal.mm resolvePasses)
    if (r->_raw.samples != 1 && !(from->_raw.samples > 1 && !gm20b::formatStandIn(from->_raw.format) &&
                                  r->_raw.blockLinear && !r->_raw.rotation))
        return false;
    if (gm20b::formatStandIn(from->_raw.format)) {
        if (!standInResolve(from->_raw, r))
            return false;
    } else if (!(r->_raw.renderable || r->_raw.shadowTarget)) {
        return false;
    } else if (from->_raw.samples <= 1) {
        // the copy reads the pass's own target
        if (from->_raw.rotation || r->_raw.rotation || from->_raw.planarStandIn || from->_raw.planar)
            return false;
    } else if (!(gRM->raw->resolvePipeline(from->_raw.format, from->_raw.samples) ||
                 resolvesByTwoD(cb, from->_raw.format) ||
                 (gm20b::codesResolvedByTwod(from->_raw.format) && from->_raw.format == r->_raw.format &&
                  (from->_raw.samples == 2 || from->_raw.samples == 4) && !r->_raw.rotation))) {
        return false;
    }
    const bool volume = r->_raw.type == gm20b::fx::kTexThreeD;
    facts.resolveLevel = a.resolveLevel;
    facts.resolveSlice = volume ? a.resolveDepthPlane : a.resolveSlice;
    gm20b::Texture part;
    return !(volume ? a.resolveSlice : a.resolveDepthPlane) && facts.resolveLevel < gm20b::fx::kMaxLevels &&
           facts.resolveSlice <= UINT32_MAX &&
           (gm20b::wholeAttachment(r->_raw, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice)) ||
            gm20bAttachmentPart(r, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice), part));
}

bool gm20bFormatTarget(SwitchMetalTexture *x)
{
    const gm20b::Texture &t = x->_raw;
    return x->_gm20bOk && gm20b::formatStandIn(t.format) &&
           gm20b::formatStandInSamples(t.format, t.samples, t.memoryless);
}

// A view of several layers or levels is no target as a whole; the part a pass names of it is, where its format is one the
// GM20B renders (attachment_target.h)
static bool partTarget(SwitchMetalTexture *x, NSUInteger level, NSUInteger slice)
{
    gm20b::Texture part;
    const gm20b::FormatInfo *f = gm20b::formatInfo(x->_raw.format);
    return f && f->rt && !gm20b::wholeAttachment(x->_raw, uint32_t(level), uint32_t(slice)) &&
           gm20bAttachmentPart(x, uint32_t(level), uint32_t(slice), part);
}

bool gm20bWholeViewTarget(SwitchMetalTexture *x)
{
    const gm20b::Texture &t = x->_raw;
    const gm20b::FormatInfo *f = gm20b::formatInfo(t.format);
    return x->_viewPart && f && f->rt && t.blockLinear && !t.renderable && !t.shadowTarget && !t.zeta &&
           (t.type == gm20b::fx::kTexTwoD || t.type == gm20b::fx::kTexOneD) && t.samples <= 1;
}

const char *gm20bColorTargetMissing(SwitchMetalTexture *x, NSUInteger level, NSUInteger slice)
{
    const gm20b::Texture &t = x->_raw;
    gm20b::Texture buffer;
    if (t.renderable || t.shadowTarget || gm20bFormatTarget(x) || partTarget(x, level, slice) ||
        gm20b::texelBufferTarget(t, buffer) || gm20b::planeInBlocks(t, uint32_t(level), uint32_t(slice)) ||
        gm20bWholeViewTarget(x))
        return nullptr;
    if (t.type == gm20b::fx::kTexOneDBuffer)
        return "a texture buffer past 16384 texels as a colour attachment, which an M4 Max fails Internal";
    if (gm20b::formatStandIn(t.format))
        return "a multisampled target of a format the GM20B renders none of, of a sample count the pack has no "
               "per-sample load and store states of";
    return "a colour attachment the driver renders no form of";
}

const char *gm20bColorPartMissing(SwitchMetalTexture *x, NSUInteger level, NSUInteger slice, bool resolving)
{
    const gm20b::Texture &t = x->_raw;
    gm20b::Texture part;
    const bool whole = gm20b::wholeAttachment(t, uint32_t(level), uint32_t(slice));
    if (resolving && t.samples > 1 && !whole)
        return "a resolved part of a multisampled texture, which this device makes no layers or levels of";
    if (whole || gm20b::texelBufferTarget(t, part) || gm20b::planeInBlocks(t, uint32_t(level), uint32_t(slice)) ||
        gm20bAttachmentPart(x, uint32_t(level), uint32_t(slice), part))
        return nullptr;
    if (!t.blockLinear && gm20b::formatStandIn(t.format))
        return "a slice of a linear array of a format the GM20B renders none of: its stand-in reads block-linear "
               "headers";
    return "a level of a view of several levels past its first, which the driver binds through no levels owner";
}

const char *gm20bPlanarTargetRefusal(SwitchMetalTexture *x, MTLRenderPassColorAttachmentDescriptor *a)
{
    if (x->_raw.planar && !x->_raw.planarStandIn &&
        !(gRM->raw->standInPipeline(false) && gRM->raw->standInPipeline(true)))
        return "a planar texture as a render target: the pack has no two-plane load or store state";
    return x->_raw.ycbcr ? ycbcrTargetPassRefusal(x->_raw, a) : nullptr;
}
