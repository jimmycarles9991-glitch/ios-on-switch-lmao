// SPDX-License-Identifier: MIT
// The range encode's render passes as they begin: their attachments and the stand-ins and zeta parts they render into
#import "TegraGM20BMetalRangeEncode.h"
#import "TegraGM20BMetalPassAttachments.h"
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalPassUses.h"
#import "TegraGM20BMetalReach.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalSplitPlan.h"
#import "TegraGM20BMetalTargetViews.h"
#include "clear.h"
#include "format_standin.h"
#include "plain_rows.h"
#include "planar_target.h"
#include "xr_pools.h"

#include <algorithm>
#include <iterator>

// A planar render target's stand-in passes (planar_target.h), the frontend's: the planes' codes into the stand-in before a
// pass that loads it (its code image from the planes first, the copy engine's remaps), the stand-in into the planes after one
// that stores it. The stand-in and plane targets are the texture's own, or the pass's (the scratch of attachment `slot`, the
// planes as targets of no header). 0: drawn; else fail()'s or refused()'s answer.
int SwitchMetalRangeEncode::standInPass(SwitchMetalTexture *t, uint32_t slot, bool store)
{
    gm20b::Texture &x = t->_raw;
    gm20b::Texture *standIn = x.planarStandIn ?: planarScratch_[slot];
    gm20b::Texture luma, chroma;
    if (store && !x.luma) {
        gm20b::planarPlaneTarget(x, false, luma);
        gm20b::planarPlaneTarget(x, true, chroma);
        if (!d_->sampleable(*standIn)) {
            constexpr const char *kNoHeader = "no texture header for a planar texture's stand-in";
            gm20bPassWorkFails(cb_, ahead_, kNoHeader, MTLCommandBufferErrorOutOfMemory);
            fail(PredEncPlanarRT, kNoHeader, slot);
            return 1;
        }
    }
    if (!store) {
        gm20b::BlitEncoder *b = rc_->blit();
        b->convertPlanar(x);
        b->end();
    }
    gm20b::PassDesc p;
    p.color[0].texture = store ? (x.luma ?: &luma) : standIn;
    p.color[1].texture = store ? (x.chroma ?: &chroma) : nullptr;
    for (gm20b::ColorAttachment &a : p.color) {
        a.load = gm20b::ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = store ? 2 : 1;
    gm20b::RenderEncoder *e = rc_->render(p);
    if (!e) {
        constexpr const char *kNoPass = "no memory for a planar texture's stand-in pass";
        gm20bPassWorkFails(cb_, ahead_, kNoPass, MTLCommandBufferErrorOutOfMemory);
        fail(PredEncPlanarRT, kNoPass, store);
        return 1;
    }
    e->setPipeline(d_->standInPipeline(store));
    e->setTexture(0, store ? standIn : &x);
    if (!e->draw(3 /* MTLPrimitiveTypeTriangle */, 0, 3, 1, 0, why_)) {
        gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
        refused();
        return 2;
    }
    e->end();
    if (store)
        planesWritten_.push_back(t);
    return 0;
}

bool SwitchMetalRangeEncode::beginPass(size_t at)
{
    SwitchMetalCommandBuffer *cb = cb_;
    gm20b::CommandBuffer *rc = rc_;
    const bool own = !ahead_;
    const auto &header = own ? cb->_plan.ownBegin(at) : cb->_plan.begin(at);
    computeUnit_ = false;
    rc->endPredicationUnit();
    SWITCH_METAL_ENCODE_SPAN(Pass);
    const SwitchMetalPass &pass = own ? cb->_passes.own(header.pass) : cb->_passes[header.pass];
    renderBegin_ = at;
    if (continuing_) {
        continuing_ = false;
        uint8_t stores = 0, discards = 0;
        const uint8_t mask = pass.colorMask ? pass.colorMask : 0xffu;
        for (uint8_t m = mask; m; m &= uint8_t(m - 1)) {
            const uint32_t i = uint32_t(__builtin_ctz(m));
            if (pass.color[i].texture && pass.color[i].store == MTLStoreActionStore)
                stores |= uint8_t(1u << i);
            if (pass.color[i].texture && pass.color[i].store == MTLStoreActionDontCare)
                discards |= uint8_t(1u << i);
        }
        re_->continuePass(stores, discards);
        planSplitPrograms(cb, at, *re_, own);
        planPassUses(cb, at, planned_, own);
        return true;
    }
    gm20b::PassDesc p;
    p.mayFetch = false;
    bool shaderTestsDepth = false, testsStencil = false;
    for (size_t sub = at; sub == at || (sub < to_ && gm20bContinuesPass(cb, sub));
         sub = (own ? cb->_plan.ownBegin(sub).end : cb->_plan.begin(sub).end)) {
        const SwitchMetalPass &subPass =
            sub == at ? pass : (own ? cb->_passes.own(cb->_plan.ownBegin(sub).pass) : cb->_passes[cb->_plan.begin(sub).pass]);
        if (sampledShadows_)
            refresh(sub);
        if (standIns_ && !standIns(sub))
            return false;
        if (subPass.xrPlaneBinds && !imagePlanes(sub, p))
            return false;
        passStorageMasks(cb, sub, p, own);
        p.hiddenZeta |= subPass.testsHiddenZeta;
        shaderTestsDepth |= subPass.shaderTestsDepth;
        testsStencil |= subPass.testsStencil;
    }
    const uint8_t colorMask = pass.colorMask ? pass.colorMask : 0xffu;
    for (uint8_t cm = colorMask; cm; cm &= uint8_t(cm - 1)) {
        const unsigned j = unsigned(__builtin_ctz(cm));
        const SwitchMetalPass::Color &a = pass.color[j];
        __unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)a.texture;
        if (!t)
            continue;
        gm20b::Texture *standIn = t->_raw.planarStandIn;
        if (t->_raw.planar && !standIn &&
            !(standIn = planarScratch_[j] = rc->turnScratch(gm20b::kPlanarStandInFormat, t->_raw.width, t->_raw.height,
                                                            gm20b::kPlanarScratchTag + j))) {
            constexpr const char *kNoStandIn = "no memory for a planar texture's stand-in";
            gm20bPassWorkFails(cb, ahead_, kNoStandIn, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPlanarRT, kNoStandIn, j);
        }
        useTexture(rc, t, true);
        // an M4 Max stores only the 32x32 frame tiles a loaded pass's draws reach of a plain target whose rows overlap
        // (the plain-rows probe: a scissored draw's texels win bytes later rows outside it would by the order)
        if (gm20b::plainRowsPooled(t->_raw) && a.load == MTLLoadActionLoad && a.store != MTLStoreActionDontCare)
            loadedTargets_ |= uint8_t(1u << j);
        if (standIn) {
            // Metal's DontCare load keeps the planes' codes where no draw writes, and the store writes every texel
            // (harness/standinoracle.mm): such a pass loads them too. A YCbCr code target's undrawn texels are
            // undefined there (ycbcr_store.h)
            const bool stores = a.store != MTLStoreActionDontCare;
            if (a.load == MTLLoadActionLoad && t->_raw.ycbcrStore) {
                planeLoads_.push_back({t, j});
                if (stores && !t->_raw.ycbcrStore->plane)
                    loadedTargets_ |= uint8_t(1u << j);
            } else if (a.load == MTLLoadActionLoad || (a.load == MTLLoadActionDontCare && stores && !t->_raw.ycbcr)) {
                standInLoads_.push_back({t, j});
            }
            if (stores)
                standInStores_.push_back({t, j});
        }
        if (!fresh_.empty())
            fresh_.erase(t);  // this pass writes it
        if (!standInFresh_.empty())
            standInFresh_.erase(t);
        // a one-layer pass's part has a header of its own (a fetch of the attachment reads it); a texture buffer's row and a
        // plane image are images of their own
        const uint32_t level = uint32_t(a.level), slice = uint32_t(a.slice);
        const bool own =
            gm20b::texelBufferTarget(t->_raw, levelTargets_[j]) || gm20b::planeInBlocks(t->_raw, level, slice);
        const bool stored = a.store != MTLStoreActionDontCare;
        gm20b::Texture *target =
            !own ? gm20bAttachmentTarget(t, level, slice, pass.layers <= 1, levelTargets_[j])
            : t->_raw.type == gm20b::fx::kTexOneDBuffer
                ? &levelTargets_[j]
                : planeImage(t, j, slice, pass.layers, a.load == MTLLoadActionLoad || stored, stored);
        if (!target) {
            const char *why = own ? *why_ : "a level, slice or plane the recording took and cannot be rendered to";
            gm20bPassWorkFails(cb, ahead_, why, own ? MTLCommandBufferErrorOutOfMemory : MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, why, a.level | a.slice << 8);
        }
        if (target == &t->_raw && gm20bWholeViewTarget(t)) {
            levelTargets_[j] = t->_raw;
            levelTargets_[j].renderable = true;
            target = &levelTargets_[j];
        }
        const bool whole = target == &t->_raw || (target == &levelTargets_[j] && gm20bWholeViewTarget(t));
        if (!whole && !own)
            SWITCH_METAL_DIAGNOSTIC(levelPasses_++);
        p.color[j].texture = target != &t->_raw ? target : standIn ? standIn : &t->_raw;
        if (p.color[j].texture == &t->_raw && t->_raw.nativePlane != ~0u) {
            if (const gm20b::Texture *native = rc->writeTexture(&t->_raw); native != &t->_raw)
                p.color[j].texture = const_cast<gm20b::Texture *>(native);
        }
        if (standIn && t->_raw.rotation) {  // laid out as its planes (TegraGM20BMetalRotated.mm): drawn turned
            levelTargets_[j] = *standIn;
            levelTargets_[j].rotation = t->_raw.rotation;
            p.color[j].texture = &levelTargets_[j];
        }
        p.color[j].image = whole || own ? nullptr : &t->_raw;
        p.color[j].level = own ? 0 : level;
        p.color[j].slice = own ? 0 : slice;
        p.color[j].load = a.load == MTLLoadActionClear  ? gm20b::ColorAttachment::Clear
                          : a.load == MTLLoadActionLoad ? gm20b::ColorAttachment::Load
                                                        : gm20b::ColorAttachment::DontCare;
        // a one-sample attachment's resolve copies it (resolvePasses), so its pass keeps all of it
        const bool copied = a.resolve && t->_raw.samples <= 1 && a.store == MTLStoreActionMultisampleResolve;
        p.color[j].store =
            a.store == MTLStoreActionStore || a.store == MTLStoreActionStoreAndMultisampleResolve || copied;
        p.color[j].discard = !p.color[j].store && !a.resolve;
        if (a.store == MTLStoreActionMultisampleResolve || a.store == MTLStoreActionStoreAndMultisampleResolve)
            if (__unsafe_unretained SwitchMetalTexture *r = (SwitchMetalTexture *)a.resolve)
                resolves_.push_back({j, r, a.store == MTLStoreActionStoreAndMultisampleResolve,
                                     uint32_t(a.resolveLevel), uint32_t(a.resolveSlice)});
        const MTLClearColor k = a.clear;
        const double exact[4] = {k.red, k.green, k.blue, k.alpha};
        if (!gm20b::integerClearSlots(t->_raw.format, exact, p.color[j].clear))
            for (int c = 0; c < 4; c++)
                p.color[j].clear[c] = float(exact[c]);
        if (t->_raw.ycbcrStore)  // Metal holds a clear's colour rounded to the nearest half, a draw's maybe not
            for (float &c : p.color[j].clear)
                c = float(_Float16(c));
        if (gm20b::formatStandIn(t->_raw.format))
            formatTargets_.push_back({t, j});
        p.count = j + 1;
    }
    if (__unsafe_unretained SwitchMetalTexture *r = (SwitchMetalTexture *)pass.depthResolve)
        depthResolve_ = {r, pass.depthFilter, uint32_t(pass.depthResolveLevel), uint32_t(pass.depthResolveSlice)};
    if (__unsafe_unretained SwitchMetalTexture *r = (SwitchMetalTexture *)pass.stencilResolve)
        stencilResolve_ = {r, pass.stencilPick, uint32_t(pass.stencilResolveLevel), uint32_t(pass.stencilResolveSlice)};
    separateResolved_[0] = separateResolved_[1] = nullptr;
    if (__unsafe_unretained SwitchMetalTexture *z =
            (SwitchMetalTexture *)pass.depth) {  // (gm20bDepthStencil: decided at record)
        __unsafe_unretained SwitchMetalTexture *s = (SwitchMetalTexture *)pass.zeta[1].texture;
        // (gm20bSeparateZeta: two textures, or one at two levels or slices)
        const bool separate =
            s && pass.zeta[0].texture &&
            (s != z || pass.zeta[0].level != pass.zeta[1].level || pass.zeta[0].slice != pass.zeta[1].slice);
        p.depthStencil.texture = &z->_raw;
        if (!separate) {
            // a level and a layer of a 2D array or cube: its layers from there, through the chain of a view's texture
            const NSUInteger level = pass.zeta[0].texture ? pass.zeta[0].level : pass.zeta[1].level;
            const gm20b::Texture *part = gm20bZetaPart(z, level, pass.depthSlice, zetaPart_);
            if (!part) {
                constexpr const char *kNoLevel =
                    "a depth/stencil level the recording took that its texture has no chain of";
                gm20bPassWorkFails(cb, ahead_, kNoLevel, MTLCommandBufferErrorInternal);
                return fail(PredPassDepthStencil, kNoLevel, level);
            }
            p.depthStencil.texture = const_cast<gm20b::Texture *>(part);
            // a turned pass loads it through a turn copy (turned_pass.h), which reads the texture's own header of
            // the part: one made for the pass would never be freed
            const bool loads = pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad;
            if (p.depthStencil.texture == &zetaPart_ && loads && passTurn(pass) &&
                !(zetaPart_.tic = gm20bZetaPartTic(z, level, pass.depthSlice, zetaPart_))) {
                constexpr const char *kNoHeader = "no texture header for a turned pass's depth/stencil level or slice";
                gm20bPassWorkFails(cb, ahead_, kNoHeader, MTLCommandBufferErrorOutOfMemory);
                return fail(PredPassRotated, kNoHeader, level | pass.depthSlice << 8);
            }
        }
        p.depthStencil.clearDepth = pass.depthLoad == MTLLoadActionClear;
        p.depthStencil.clearStencil = pass.stencilLoad == MTLLoadActionClear;
        p.depthStencil.loaded = pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad;
        p.depthStencil.stored =
            pass.zeta[0].store != MTLStoreActionDontCare || pass.zeta[1].store != MTLStoreActionDontCare;
        p.depthStencil.depthEnabled = (pass.zetaAspects & 1) != 0;
        p.depthStencil.stencilEnabled = (pass.zetaAspects & 2) != 0;
        p.depthStencil.depth = gm20b::metalClearDepth(pass.clearDepth, z->_raw.format);
        p.depthStencil.stencil = pass.clearStencil;
        if (separate) {
            separateZeta(pass, z, s, p);
            if (!depth16Zeta(pass, p))
                return false;
        } else if (((z->_raw.format == MTLPixelFormatDepth16Unorm && (shaderTestsDepth || testsStencil)) ||
                    (z->_raw.format == MTLPixelFormatDepth32Float && testsStencil) ||
                    (z->_raw.format == MTLPixelFormatDepth32Float_Stencil8 && testsStencil &&
                     !(pass.zetaAspects & 2))) &&
                   !depth16Tested(pass, p)) {
            return false;
        }
    }
    p.width = uint32_t(std::min<NSUInteger>(pass.width, UINT32_MAX));
    p.height = uint32_t(std::min<NSUInteger>(pass.height, UINT32_MAX));
    p.layers = pass.layers;
    p.positions = pass.positions;
    for (const auto &[t, j] : standInLoads_)
        if (standInPass(t, j, false))
            return false;
    standInLoads_.clear();
    PlanDraws draws(cb, at, to_, *rc);
    const size_t next = header.end;
    if (!loadPlanes(p) || !formatStandIns(pass, p, draws, next < to_ && gm20bContinuesPass(cb, next)))
        return false;
    if (loadedTargets_ && !reachOfLoadedTargets(pass, p, draws, *rc, loadedReach_, why_)) {
        gm20bReachFails(cb, ahead_, *why_, loadedReach_.memory, rc->refusal);
        return fail(PredEncPlanarRT, *why_, loadedTargets_, 0, *why_);
    }
    overlappingPlanes(pass, p);
    texelRowsTargets(pass);
    if (!turned_.begin(*rc, p, draws, why_)) {
        gm20bReachFails(cb, ahead_, *why_, turned_.failedForMemory(), rc->refusal);
        return fail(PredPassRotated, *why_, 0, 0, *why_);
    }
    p.metalDraws = true;
    // the recording failed every pass Metal's validation stops (TegraGM20BMetalPassErrors.h): render() refuses the rest for
    // want of memory alone
    if (!(re_ = rc->render(p, why_))) {
        gm20bPassWorkFails(cb, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
        return passRefused(p.count);
    }
    counters_->started(at, *re_);
    passDraws_ = rc->draws;
    planSplitPrograms(cb, at, *re_, own);
    planned_.reset();
    planned_.known = !sampledShadows_ && !standIns_;
    planPassUses(cb, at, planned_, own);
    return true;
}

// The zcull regions the job ending at `to` stored that a pass of the raw ranges straight after it loads (none past an
// event's or a held range) are kept for those ranges (gm20b::CommandBuffer::carryZcull). Read at the seal, from the
// commit on, when every pass of the command buffer is recorded.
void SwitchMetalRangeEncode::carryZcull(size_t to)
{
    SwitchMetalCommandBuffer *cb = cb_;
    const std::vector<gm20b::ZcullStored> &regions = rc_->zcullRegions();
    if (regions.empty())
        return;
    size_t end = to;
    for (size_t s = cb->_segNext + 1; s < cb->_segs.size(); ++s) {
        const SwitchMetalSegment &next = cb->_segs[s];
        if (next.event || next.held || next.from != end)
            break;
        end = next.to;
    }
    const SwitchMetalCommandPlan &plan = cb->_plan;
    uint32_t kept = 0;
    for (size_t k = to; k < end;) {
        const Op op = plan.operation(k).op;
        if (op == OpRender) {
            const SwitchMetalPass &later = cb->_passes[plan.legacy(k).x];
            if (later.depth && later.depthLoad == MTLLoadActionLoad)
                for (size_t i = 0; i < regions.size(); i++)
                    kept |= uint32_t(regions[i].zetaVa == ((SwitchMetalTexture *)later.depth)->_raw.va) << i;
        }
        k = std::max(SwitchMetalCommandPlan::isEncoderBegin(op) ? plan.encoderEnd(k) : k + 1, k + 1);
    }
    if (end > to)
        rc_->carryZcull((__bridge const void *)cb, to, kept);
}

void SwitchMetalRangeEncode::overlappingPlanes(const SwitchMetalPass &pass, gm20b::PassDesc &p)
{
    const uint8_t mask = pass.colorMask ? pass.colorMask : 0xffu;
    for (uint8_t m = mask; m; m &= uint8_t(m - 1)) {
        const unsigned j = unsigned(__builtin_ctz(m));
        const SwitchMetalPass::Color &a = pass.color[j];
        __unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)a.texture;
        if (!t)
            continue;
        const bool plain = gm20b::plainRowsPooled(t->_raw) && p.color[j].texture == &t->_raw;
        if (plain) {
            levelTargets_[j] = *t->_raw.sampled;
            levelTargets_[j].rotation = t->_raw.rotation;  // laid out as the plane: a turned one's drawn turned
            p.color[j].texture = &levelTargets_[j];
        } else if (!gm20b::xrPlanesOverlap(t->_raw) ||
                   // drawn in an image of its planes, or through its format's stand-in, it is stored there
                   std::none_of(std::begin(p.color), std::end(p.color), [&](const gm20b::ColorAttachment &c) {
                       return c.texture == t->_raw.luma->sampled;
                   })) {
            continue;
        }
        if (a.load == MTLLoadActionLoad && plain)
            gm20b::fillPlainRows(*rc_, t->_raw);
        else if (a.load == MTLLoadActionLoad)
            gm20b::fillXrPools(*rc_, t->_raw);
        if (a.store == MTLStoreActionDontCare)
            continue;
        overlapStores_.push_back(t);
        if (a.load == MTLLoadActionLoad)
            overlapLoaded_.push_back(t);
    }
}
