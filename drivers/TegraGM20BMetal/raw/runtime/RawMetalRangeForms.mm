// SPDX-License-Identifier: MIT
// The range encode's passes through attachments of their own: what a pass renders into in place of its targets, and what
// goes back into them after it
#import "RawMetalRangeEncode.h"
#import "RawMetalXrPlanes.h"
#include "raw_format_standin.h"
#include "raw_fresh.h"
#include "raw_turn_copy.h"

// Loaded two-plane YCbCr targets (raw_ycbcr_load.h): each pass renders into a stand-in the pack's load state fills from the
// planes first
bool RMRangeEncode::loadPlanes(raw::PassDesc &p)
{
    for (const auto &[t, j] : planeLoads_) {
        raw::Texture *loaded = nullptr;
        if (const int failed = loadYcbcrStandIn(rc_, t->_raw, j, loaded, why_))
            return failed == 1 ? fail(PredEncPlanarRT, *why_, j) : refused();
        p.color[j].texture = loaded;
    }
    planeLoads_.clear();
    return true;
}

// Targets of a format the GM20B renders none of (raw_format_standin.h): the pass renders into a stand-in of the attachment's
// image (its level's or slice's part), filled from it unless the pass clears it, its clear colour as the stand-in holds it
bool RMRangeEncode::formatStandIns(const RMPass &pass, raw::PassDesc &p)
{
    for (const auto &[t, j] : formatTargets_) {
        if (p.layers > 1)
            return fail(PredPassAttFormat,
                        "a layered pass into a target of a format the GM20B renders none of: its stand-in is one layer",
                        t->_raw.format, j);
        raw::Texture &image = *p.color[j].texture;
        const bool fill = !t->_raw.memoryless && pass.color[j].load != MTLLoadActionClear;
        raw::Texture *standIn = raw::formatStandInFor(*d_, *rc_, image, j, fill, why_);
        if (!standIn)
            return fail(PredPassAttFormat, *why_, t->_raw.format, j);
        p.color[j].texture = standIn;
        p.color[j].image = nullptr;  // the stand-in is an image of its own, at its level 0
        p.color[j].level = p.color[j].slice = 0;
        MTLTextureDescriptor *made = t->_descriptor;
        const bool compressed =
            !t->_buffer && !t->_surface &&
            raw::metalCompresses(uint64_t(made.usage), made.allowGPUOptimizedContents, t->_raw.width, t->_raw.height);
        raw::formatStandInClear(t->_raw.format, p.color[j].clear, p.color[j].clear, compressed);
        if (p.color[j].store && !t->_raw.memoryless)
            formatStores_.push_back({&image, standIn});
    }
    formatTargets_.clear();
    return true;
}

bool RMRangeEncode::storeFormatTargets()
{
    for (const auto &[image, standIn] : formatStores_)
        if (!raw::storeFormatStandIn(*d_, *rc_, *image, *standIn, why_))
            return fail(PredPassAttFormat, *why_, image->format);
    formatStores_.clear();
    return true;
}

// A separate depth and stencil texture, each at its own level and slice: the pass renders a scratch zeta of the pair's,
// which loads and stores each aspect, and stores an aspect a resolve reads back into its texture for the resolve to read there
void RMRangeEncode::separateZeta(const RMPass &pass, RMTexture *depth, RMTexture *stencil, raw::PassDesc &p)
{
    const auto from = [](RMTexture *t, NSUInteger level, NSUInteger slice, raw::Texture &part, uint32_t tic) {
        if (!level && !slice) {
            if (t->_raw.format != MTLPixelFormatDepth16Unorm)
                return &t->_raw;
            part = t->_raw;
            return &part;
        }
        if (level) {
            __unsafe_unretained RMTexture *owner = t->_levelsOwner ?: t;
            raw::chainLevelImage(owner->_raw, t->_levelsBase + uint32_t(level), part);
            part = raw::layersFrom(part, t->_slicesBase + uint32_t(slice));
            part.layers = t->_raw.layers - uint32_t(slice);
        } else {
            part = raw::layersFrom(t->_raw, uint32_t(slice));
        }
        part.tic = tic;
        return &part;
    };
    const bool resolvedDepth = depthResolve_.into && !depth->_raw.memoryless;
    raw::Texture *depthFrom =
        from(depth, pass.zeta[0].level, pass.zeta[0].slice, separatePart_[0], pass.zeta[0].sourceTic);
    p.separate = {depthFrom,
                  from(stencil, pass.zeta[1].level, pass.zeta[1].slice, separatePart_[1], pass.zeta[1].sourceTic),
                  pass.depthLoad == MTLLoadActionLoad,
                  pass.stencilLoad == MTLLoadActionLoad,
                  pass.zeta[0].store == MTLStoreActionStore || resolvedDepth,
                  pass.zeta[1].store == MTLStoreActionStore || stencilResolve_.into};
    p.depthStencil.texture = depthFrom;
    separateResolved_[0] = resolvedDepth ? p.separate.depth : nullptr;
    separateResolved_[1] = stencilResolve_.into ? p.separate.stencil : nullptr;
    useTexture(rc_, depth, true);
    useTexture(rc_, stencil, true);
}

// A one-sample Depth16Unorm's depth beside a Stencil8 through a Depth32Float_Stencil8 of the command buffer's, converted in
// by draws where the pass loads it and back at its end where it stores it; a depth bias counts in its 16-bit codes
bool RMRangeEncode::depth16Zeta(const RMPass &pass, raw::PassDesc &p)
{
    const raw::Texture &z = *p.separate.depth;
    if (z.format != MTLPixelFormatDepth16Unorm)
        return true;
    raw::Texture *scratch = rc_->turnScratch(raw::kZetaFormat, z.width, z.height, raw::kDepthConvertTag, z.samples);
    if (!scratch)
        return fail(PredEncPassRender, "no Depth32Float_Stencil8 scratch for a Depth16Unorm", z.width);
    if (pass.depthLoad == MTLLoadActionLoad && !rc_->depthConvert(*scratch, z, why_))
        return fail(PredEncPassRender, *why_, z.width);
    p.depthStencil.texture = scratch;
    p.biasDepthBits = 16;
    p.separate.depth = nullptr;
    p.separate.loadDepth = p.separate.storeDepth = false;
    depth16Scratch_ = pass.zeta[0].store == MTLStoreActionStore ? scratch : nullptr;
    return true;
}

bool RMRangeEncode::depth16Back()
{
    if (!depth16Scratch_)
        return true;
    const raw::Texture *scratch = depth16Scratch_;
    depth16Scratch_ = nullptr;
    if (!rc_->depthConvert(separatePart_[0], *scratch, why_))
        return fail(PredEncPassRender, *why_, separatePart_[0].width);
    return true;
}

// Two-plane RGB10A8_2P_XR10 textures the pass's draws store into: each through an image of its texels, merged from the planes
// before the pass (RawMetalXrPlanes.mm)
bool RMRangeEncode::imagePlanes(size_t pass, raw::PassDesc &p)
{
    std::vector<__unsafe_unretained RMTexture *> written;
    cb_->_plan.forEachResource(
        pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture, [&](const RMRangeResource &use) {
            __unsafe_unretained RMTexture *t = (__bridge RMTexture *)use.object;
            if ((use.access & raw::ResourceUse::Write) && t->_raw.xrPlanes && !t->_raw.memoryless &&
                std::find(written.begin(), written.end(), t) == written.end())
                written.push_back(t);
        });
    for (__unsafe_unretained RMTexture *t : written) {
        if (std::any_of(planesImaged_.begin(), planesImaged_.end(), [&](const auto &w) {
                return w.first == t;
            }))
            continue;
        const uint32_t k = uint32_t(planesImaged_.size());
        if (k == raw::PassDesc::kWrittenPlanes)
            return fail(PredEncPassRender, "more written two-plane RGB10A8_2P_XR10 textures than a pass holds", k);
        raw::Texture *image = nullptr;
        if (const int failed = mergeXrPlanes(rc_, t->_raw, k, image, why_))
            return failed == 1 ? fail(PredEncPassRender, *why_, k) : refused();
        planesImaged_.push_back({t, image});
        p.writtenPlanes[k] = {&t->_raw, image};
    }
    return true;
}

// What the pass's draws stored goes back: sampled shadows copied into their textures, stand-ins drawn again before their
// next sample, two-plane images split into their planes
bool RMRangeEncode::writtenBack()
{
    if (!shadowsWritten_.empty()) {
        raw::BlitEncoder *b = rc_->blit();
        for (__unsafe_unretained RMTexture *t : shadowsWritten_) {
            const raw::Texture &x = t->_raw;
            b->copyTexels(*x.sampled, 0, 0, 0, x, 0, 0, 0, x.width, x.height);
        }
        b->end();
    }
    for (__unsafe_unretained RMTexture *t : standInsWritten_)
        standInFresh_.erase(t);
    shadowsWritten_.clear();
    standInsWritten_.clear();
    for (const auto &[t, image] : planesImaged_) {
        standInFresh_.erase(t);
        if (const int failed = splitXrPlanes(rc_, t->_raw, *image, why_))
            return failed == 1 ? fail(PredEncPassRender, *why_) : refused();
    }
    planesImaged_.clear();
    return true;
}
