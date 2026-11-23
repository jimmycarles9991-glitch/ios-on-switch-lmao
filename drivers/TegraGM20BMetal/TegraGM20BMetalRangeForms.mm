// SPDX-License-Identifier: MIT
// The range encode's passes through attachments of their own: what a pass renders into in place of its targets, and what
// goes back into them after it
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalRangeEncode.h"
#import "TegraGM20BMetalReach.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalXrPlanes.h"
#include "attachment_target.h"
#include "format_standin.h"
#include "fresh.h"
#include "turn_copy.h"
#include "xr_pools.h"

// Loaded two-plane YCbCr targets (ycbcr_load.h): each pass renders into a stand-in the pack's load state fills from the
// planes first
bool SwitchMetalRangeEncode::loadPlanes(gm20b::PassDesc &p)
{
    for (const auto &[t, j] : planeLoads_) {
        gm20b::Texture *loaded = nullptr;
        if (const int failed = loadYcbcrStandIn(rc_, t->_raw, j, loaded, why_)) {
            if (failed == 2) {
                gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPlanarRT, *why_, j);
        }
        p.color[j].texture = loaded;
        if (t->_raw.rotation) {  // laid out as its plane, drawn turned (beginPass)
            levelTargets_[j] = *loaded;
            levelTargets_[j].rotation = t->_raw.rotation;
            p.color[j].texture = &levelTargets_[j];
        }
    }
    planeLoads_.clear();
    return true;
}

gm20b::Texture *SwitchMetalRangeEncode::planeImage(SwitchMetalTexture *t, uint32_t j, uint32_t plane, uint32_t layers,
                                                   bool filled, bool stored)
{
    const gm20b::Texture &x = t->_raw;
    layers = std::min(std::max(layers, 1u), x.depth - plane);
    gm20b::Texture *image = rc_->turnScratch(x.format, x.width, x.height, gm20b::kPlaneImageTag + j, 1, layers);
    // a stand-in format's image is read by its stand-in's fill through a header made once with the scratch
    if (!image || (gm20b::formatStandIn(x.format) && filled && !d_->sampleable(*image))) {
        *why_ = "no memory or texture header for the image of a 3D texture's planes";
        return nullptr;
    }
    if (filled) {
        gm20b::BlitEncoder *b = rc_->blit();
        for (uint32_t l = 0; l < layers; l++)
            b->copyTexels(x, 0, 0, plane + l, *image, 0, 0, l, x.width, x.height);
        b->end();
    }
    if (stored)
        planeImages_.push_back({t, image, plane, layers});
    levelTargets_[j] = *image;
    return &levelTargets_[j];
}

void SwitchMetalRangeEncode::planesBack()
{
    if (planeImages_.empty())
        return;
    gm20b::BlitEncoder *b = rc_->blit();
    for (const PlaneImage &i : planeImages_)
        for (uint32_t l = 0; l < i.layers; l++)
            b->copyTexels(*i.image, 0, 0, l, i.t->_raw, 0, 0, i.plane + l, i.t->_raw.width, i.t->_raw.height);
    b->end();
    planeImages_.clear();
}

// Targets of a format the GM20B renders none of (format_standin.h): the pass renders into a stand-in of the attachment's
// image (its level's or slice's part), filled from it unless the pass clears it, its clear colour as the stand-in holds it.
// A turned one's stand-in, laid out as its plane, is filled and stored straight and drawn turned (TurnedPass)
// A pass that draws nothing (in no sub-encoder that continues it either) and neither clears nor resolves one keeps its
// codes: an M4 Max keeps a BGRA10_XR_sRGB alpha code past 894 there, which the tiles its draws touch clamp (m2v-textures4
// probes/xr553.mm, more.mm corner). One that draws nothing and clears a BGR10_XR or BGRA10_XR one renders no stand-in:
// the clear registers write its codes (xr.h xrClearTexel), Metal's straight route on memory it compresses
bool SwitchMetalRangeEncode::formatStandIns(const SwitchMetalPass &pass, gm20b::PassDesc &p, const PlanDraws &draws,
                                            bool continued)
{
    std::optional<bool> drawn;
    for (const auto &[t, j] : formatTargets_) {
        const gm20b::ColorAttachment bound = p.color[j];
        const SwitchMetalPass::Color &a = pass.color[j];
        const bool drawless = !continued && !a.resolve && !(drawn ? *drawn : *(drawn = draws.any()));
        const uint32_t format = t->_raw.format;
        if (drawless && a.load == MTLLoadActionClear && (format == 552 || format == 554))
            continue;
        const bool kept = drawless && a.load != MTLLoadActionClear;
        const bool fill = !kept && !t->_raw.memoryless && a.load != MTLLoadActionClear;
        gm20b::Texture *standIn = gm20b::formatStandInFor(*d_, *rc_, bound, p.layers, j, fill, why_);
        if (!standIn) {
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredPassAttFormat, *why_, t->_raw.format, j);
        }
        // a turned texture is a surface plane, always bound whole: its slot's level target is free for the turned stand-in
        if (t->_raw.rotation) {
            levelTargets_[j] = *standIn;
            levelTargets_[j].rotation = t->_raw.rotation;
        }
        p.color[j].texture = t->_raw.rotation ? &levelTargets_[j] : standIn;
        p.color[j].image = nullptr;  // the stand-in is an image of its own, at its level 0
        p.color[j].level = p.color[j].slice = 0;
        MTLTextureDescriptor *made = t->_descriptor;
        const bool compressed =
            !t->_buffer && !t->_surface &&
            gm20b::metalCompresses(uint64_t(made.usage), made.allowGPUOptimizedContents, t->_raw.width, t->_raw.height);
        gm20b::formatStandInClear(t->_raw.format, p.color[j].clear, p.color[j].clear, compressed);
        if (p.color[j].store && !t->_raw.memoryless && !kept)
            formatStores_.push_back({bound, p.layers, standIn, fill});
    }
    formatTargets_.clear();
    return true;
}

bool SwitchMetalRangeEncode::storeFormatTargets()
{
    for (const FormatStore &s : formatStores_) {
        // planes stored through their pools: none from a loaded pass that drew nothing, as storeOverlappingPlanes
        const gm20b::Texture *t = s.attachment.texture;
        if (!passDrew_ && s.attachment.load == gm20b::ColorAttachment::Load && t && t->xrPlanes &&
            gm20b::xrPlanesOverlap(*t))
            continue;
        if (!gm20b::storeFormatStandIn(*d_, *rc_, s.attachment, s.layers, *s.standIn, !passDrew_, why_, s.filled)) {
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredPassAttFormat, *why_, s.attachment.texture->format);
        }
    }
    formatStores_.clear();
    return true;
}

// A separate depth and stencil texture, each at its own level and slice: the pass renders a scratch zeta of the pair's,
// which loads and stores each aspect, and stores a depth a resolve reads back into its texture for the resolve to read
// there. A stencil resolve reads the scratch's stencil (stencil_bits.cpp), so a memoryless Stencil8 resolves too
void SwitchMetalRangeEncode::separateZeta(const SwitchMetalPass &pass, SwitchMetalTexture *depth,
                                          SwitchMetalTexture *stencil, gm20b::PassDesc &p)
{
    const auto from = [](SwitchMetalTexture *t, NSUInteger level, NSUInteger slice, gm20b::Texture &part,
                         uint32_t tic) {
        if (!level && !slice) {
            if (t->_raw.format != MTLPixelFormatDepth16Unorm)
                return &t->_raw;
            part = t->_raw;
            return &part;
        }
        if (level) {
            __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
            gm20b::chainLevelImage(owner->_raw, t->_levelsBase + uint32_t(level), part);
            part = gm20b::layersFrom(part, t->_slicesBase + uint32_t(slice));
            part.layers = t->_raw.layers - uint32_t(slice);
        } else {
            part = gm20b::layersFrom(t->_raw, uint32_t(slice));
        }
        part.tic = tic;
        return &part;
    };
    const bool resolvedDepth = depthResolve_.into && !depth->_raw.memoryless;
    gm20b::Texture *depthFrom =
        from(depth, pass.zeta[0].level, pass.zeta[0].slice, separatePart_[0], pass.zeta[0].sourceTic);
    p.separate = {depthFrom,
                  from(stencil, pass.zeta[1].level, pass.zeta[1].slice, separatePart_[1], pass.zeta[1].sourceTic),
                  pass.depthLoad == MTLLoadActionLoad,
                  pass.stencilLoad == MTLLoadActionLoad,
                  !depth->_raw.memoryless && (pass.zeta[0].store == MTLStoreActionStore || resolvedDepth),
                  !stencil->_raw.memoryless && pass.zeta[1].store == MTLStoreActionStore};
    p.depthStencil.texture = depthFrom;
    separateResolved_[0] = resolvedDepth ? p.separate.depth : nullptr;
    separateResolved_[1] = nullptr;
    useTexture(rc_, depth, true);
    useTexture(rc_, stencil, true);
}

// A Depth16Unorm's depth beside a Stencil8 through a Depth32Float_Stencil8 of the command buffer's, converted in by draws
// where the pass loads it and back at its end where it stores it; a depth bias counts in its 16-bit codes. Its resolve
// reads that zeta, the pass's depth target, since the conversion back runs after the resolve and a discarded depth has
// none: the pack's Depth16Unorm resolve state reads any depth and rounds the sample it picks once, as picking among
// rounded samples would
bool SwitchMetalRangeEncode::depth16Zeta(const SwitchMetalPass &pass, gm20b::PassDesc &p)
{
    const gm20b::Texture &z = *p.separate.depth;
    if (z.format != MTLPixelFormatDepth16Unorm)
        return true;
    gm20b::Texture *scratch = rc_->turnScratch(gm20b::kZetaFormat, z.width, z.height, gm20b::kDepthConvertTag,
                                               z.samples, std::max<uint32_t>(pass.layers, 1));
    if (!scratch) {
        constexpr const char *kNoScratch = "no Depth32Float_Stencil8 scratch for a Depth16Unorm";
        gm20bPassWorkFails(cb_, ahead_, kNoScratch, MTLCommandBufferErrorOutOfMemory);
        return fail(PredEncPassRender, kNoScratch, z.width);
    }
    if (pass.depthLoad == MTLLoadActionLoad && !rc_->depthConvert(*scratch, z, why_)) {
        gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
        return fail(PredEncPassRender, *why_, z.width);
    }
    p.depthStencil.texture = scratch;
    p.biasDepthBits = 16;
    p.separate.depth = nullptr;
    p.separate.loadDepth = p.separate.storeDepth = false;
    separateResolved_[0] = nullptr;
    depth16Scratch_ = pass.zeta[0].store == MTLStoreActionStore ? scratch : nullptr;
    return true;
}

// A Depth16Unorm a draw's shader tests (shader_tests.h copies no 16-bit depth) renders as one beside a Stencil8
// does, through depth16Zeta's Depth32Float_Stencil8: every layer of a layered pass converted (CommandBuffer::depthConvert),
// a turned pass's scratch moved into the plane by TurnedPass as any Depth32Float_Stencil8 attachment is
bool SwitchMetalRangeEncode::depth16Tested(const SwitchMetalPass &pass, gm20b::PassDesc &p)
{
    __unsafe_unretained SwitchMetalTexture *z = (SwitchMetalTexture *)pass.depth;
    if (pass.depthLoad == MTLLoadActionLoad) {
        const bool part = p.depthStencil.texture == &zetaPart_;
        const NSUInteger level = pass.zeta[0].level;
        if (!(part ? zetaPart_.tic || (zetaPart_.tic = d_->attachmentTic(z->_raw, zetaPart_, uint32_t(level),
                                                                         uint32_t(pass.depthSlice)))
                   : d_->sampleable(z->_raw))) {
            constexpr const char *kNoHeader = "no texture header for a Depth16Unorm's conversion";
            gm20bPassWorkFails(cb_, ahead_, kNoHeader, MTLCommandBufferErrorOutOfMemory);
            return fail(PredPassDepthStencil, kNoHeader, z->_raw.format);
        }
    }
    separatePart_[0] = *p.depthStencil.texture;
    p.separate.depth = &separatePart_[0];
    p.depthStencil.clearStencil = true;
    p.depthStencil.stencil = 0;
    return depth16Zeta(pass, p);
}

bool SwitchMetalRangeEncode::depth16Back()
{
    if (!depth16Scratch_)
        return true;
    const gm20b::Texture *scratch = depth16Scratch_;
    depth16Scratch_ = nullptr;
    if (!rc_->depthConvert(separatePart_[0], *scratch, why_)) {
        gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
        return fail(PredEncPassRender, *why_, separatePart_[0].width);
    }
    return true;
}

// Two-plane RGB10A8_2P_XR10 textures the pass's draws store into: each through an image of its texels, merged from the planes
// before the pass (TegraGM20BMetalXrPlanes.mm). Different draws write different textures, so a pass holds as many as they name: past
// gm20b::PassDesc's inline ones in morePlanes_, whose room stays from pass to pass
bool SwitchMetalRangeEncode::imagePlanes(size_t pass, gm20b::PassDesc &p)
{
    std::vector<__unsafe_unretained SwitchMetalTexture *> written;
    cb_->_plan.forEachResource(
        pass, cb_->_plan.encoderEnd(pass), SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
            __unsafe_unretained SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)use.object;
            if ((use.access & gm20b::ResourceUse::Write) && t->_raw.xrPlanes && !t->_raw.memoryless &&
                std::find(written.begin(), written.end(), t) == written.end())
                written.push_back(t);
        });
    for (__unsafe_unretained SwitchMetalTexture *t : written) {
        if (std::any_of(planesImaged_.begin(), planesImaged_.end(), [&](const auto &w) {
                return w.first == t;
            }))
            continue;
        const uint32_t k = uint32_t(planesImaged_.size());
        gm20b::Texture *image = nullptr;
        if (const int failed = mergeXrPlanes(rc_, t->_raw, k, image, why_)) {
            if (failed == 2) {
                gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, *why_, k);
        }
        planesImaged_.push_back({t, image});
        if (k < gm20b::PassDesc::kWrittenPlanes)
            p.writtenPlanes[k] = {&t->_raw, image};
        else
            morePlanes_.push_back({&t->_raw, image});
    }
    p.morePlanes = morePlanes_.data();
    p.morePlaneCount = uint32_t(morePlanes_.size());
    return true;
}

// What the pass stored goes back: sampled shadows and texel buffers' pools of a surface's rows copied into their texels,
// stand-ins drawn again before their next sample, two-plane images split into their planes
bool SwitchMetalRangeEncode::writtenBack()
{
    if (!shadowsWritten_.empty()) {
        gm20b::BlitEncoder *b = rc_->blit();
        for (__unsafe_unretained SwitchMetalTexture *t : shadowsWritten_)
            gm20bMoveShadowImage(*b, t->_raw, false);
        b->end();
    }
    for (__unsafe_unretained SwitchMetalTexture *t : standInsWritten_)
        standInFresh_.erase(t);
    shadowsWritten_.clear();
    standInsWritten_.clear();
    for (const auto &[t, image] : planesImaged_) {
        standInFresh_.erase(t);
        fresh_.erase(t);
        if (const int failed = splitXrPlanes(rc_, t->_raw, *image, !passDrew_, why_)) {
            if (failed == 2) {
                gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, *why_);
        }
    }
    planesImaged_.clear();
    morePlanes_.clear();
    return true;
}
