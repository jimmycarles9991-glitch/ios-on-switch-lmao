// SPDX-License-Identifier: MIT
// The range encode's render passes as they begin: their attachments and the stand-ins and zeta parts they render into
#import "RawMetalRangeEncode.h"
#import "RawMetalPassUses.h"
#import "RawMetalReach.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalTargetViews.h"
#include "raw_clear.h"
#include "raw_format_standin.h"

// A planar render target's stand-in passes (raw::Texture::planarStandIn), the frontend's: the planes' codes into the stand-in
// before a pass that loads it (its code image from the planes first, the copy engine's remaps), the stand-in into the planes
// after one that stores it. 0: drawn; else fail()'s or refused()'s answer.
int RMRangeEncode::standInPass(RMTexture *t, bool store)
{
    raw::Texture &x = t->_raw;
    if (!store) {
        raw::BlitEncoder *b = rc_->blit();
        b->convertPlanar(x);
        b->end();
    }
    raw::PassDesc p;
    p.color[0].texture = store ? x.luma : x.planarStandIn;
    p.color[1].texture = store ? x.chroma : nullptr;
    for (raw::ColorAttachment &a : p.color) {
        a.load = raw::ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = store ? 2 : 1;
    raw::RenderEncoder *e = rc_->render(p);
    if (!e) {
        fail(PredEncPlanarRT, "a planar texture's stand-in pass the raw path cannot render", store);
        return 1;
    }
    e->setPipeline(d_->standInPipeline(store));
    e->setTexture(0, store ? x.planarStandIn : &x);
    if (!e->draw(3 /* MTLPrimitiveTypeTriangle */, 0, 3, 1, 0, why_)) {
        refused();
        return 2;
    }
    e->end();
    if (store)
        planesWritten_.push_back(t);
    return 0;
}

bool RMRangeEncode::beginPass(size_t at)
{
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    const Cmd c = cb->_plan.legacy(at);
    computeUnit_ = false;
    rc->endPredicationUnit();
    RAW_ENCODE_SPAN(Pass);
    const RMPass &pass = cb->_passes[c.x];
    renderBegin_ = at;
    if (continuing_) {
        continuing_ = false;
        uint8_t stores = 0;
        for (uint32_t i = 0; i < 8; ++i)
            if (pass.color[i].texture && pass.color[i].store == MTLStoreActionStore)
                stores |= uint8_t(1u << i);
        re_->continuePass(stores);
        planSplitPrograms(cb, at, *re_);
        planPassUses(cb, at, planned_);
        return true;
    }
    raw::PassDesc p;
    for (size_t sub = at; sub == at || (sub < to_ && rmContinuesPass(cb, sub)); sub = cb->_plan.encoderEnd(sub)) {
        if (sampledShadows_)
            refresh(sub);
        if (standIns_ && !standIns(sub))
            return false;
        if (cb->_passes[cb->_plan.legacy(sub).x].xrPlaneBinds && !imagePlanes(sub, p))
            return false;
        passStorageMasks(cb, sub, p);
        p.hiddenZeta |= cb->_passes[cb->_plan.legacy(sub).x].testsHiddenZeta;
    }
    for (unsigned j = 0; j < 8; j++) {
        const RMPass::Color &a = pass.color[j];
        __unsafe_unretained RMTexture *t = (RMTexture *)a.texture;
        if (!t)
            continue;
        if (t->_raw.planar && !t->_raw.planarStandIn)
            return fail(PredEncPlanarRT, "a planar texture as a render target", j);
        useTexture(rc, t, true);
        if (t->_raw.planarStandIn) {
            // Metal's DontCare load keeps the planes' codes where no draw writes, and the store writes every texel
            // (raw/test/standinoracle.mm): such a pass loads them too. A YCbCr code target's undrawn texels are
            // undefined there (raw_ycbcr_store.h)
            const bool stores = a.store != MTLStoreActionDontCare;
            if (a.load == MTLLoadActionLoad && t->_raw.ycbcrStore) {
                planeLoads_.push_back({t, j});
                if (stores && !t->_raw.ycbcrStore->plane)
                    loadedTargets_ |= uint8_t(1u << j);
            } else if (a.load == MTLLoadActionLoad || (a.load == MTLLoadActionDontCare && stores && !t->_raw.ycbcr)) {
                standInLoads_.push_back(t);
            }
            if (stores)
                standInStores_.push_back({t, j});
        }
        if (!fresh_.empty())
            fresh_.erase(t);  // this pass writes it
        standInFresh_.erase(t);
        // a one-layer pass's part has a header of its own (a fetch of the attachment reads it)
        raw::Texture *target =
            rawAttachmentTarget(t, uint32_t(a.level), uint32_t(a.slice), pass.layers <= 1, levelTargets_[j]);
        if (!target)
            return fail(PredEncPassRender, "a level, slice or plane the raw path cannot render to",
                        a.level | a.slice << 8);
        const bool whole = target == &t->_raw;
        if (!whole) {
#if SWITCH_RAW_DIAGNOSTICS
            static std::atomic<bool> loggedLevel{false};
            levelPasses_++;
            if (!loggedLevel.exchange(true))
                os_log(rawLog(),
                       "raw: renders into levels and slices (first: level %lu slice %lu of %ux%u format %u type %u)",
                       (unsigned long)a.level, (unsigned long)a.slice, t->_raw.width, t->_raw.height, t->_raw.format,
                       unsigned(t->_raw.type));
#endif
        }
        p.color[j].texture = !whole ? &levelTargets_[j] : t->_raw.planarStandIn ? t->_raw.planarStandIn : &t->_raw;
        p.color[j].image = whole ? nullptr : &t->_raw;
        p.color[j].level = uint32_t(a.level);
        p.color[j].slice = uint32_t(a.slice);
        p.color[j].load = a.load == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                          : a.load == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                        : raw::ColorAttachment::DontCare;
        p.color[j].store = a.store == MTLStoreActionStore || a.store == MTLStoreActionStoreAndMultisampleResolve;
        if (a.store == MTLStoreActionMultisampleResolve || a.store == MTLStoreActionStoreAndMultisampleResolve)
            if (__unsafe_unretained RMTexture *r = (RMTexture *)a.resolve)
                resolves_.push_back({j, r, a.store == MTLStoreActionStoreAndMultisampleResolve,
                                     uint32_t(a.resolveLevel), uint32_t(a.resolveSlice)});
        const MTLClearColor k = a.clear;
        const double exact[4] = {k.red, k.green, k.blue, k.alpha};
        if (!raw::integerClearSlots(t->_raw.format, exact, p.color[j].clear))
            for (int c = 0; c < 4; c++)
                p.color[j].clear[c] = float(exact[c]);
        if (t->_raw.ycbcrStore)  // Metal holds a clear's colour rounded to the nearest half, a draw's maybe not
            for (float &c : p.color[j].clear)
                c = float(_Float16(c));
        if (raw::formatStandIn(t->_raw.format))
            formatTargets_.push_back({t, j});
        p.count = j + 1;
    }
    if (__unsafe_unretained RMTexture *r = (RMTexture *)pass.depthResolve)
        depthResolve_ = {r, pass.depthFilter};
    if (__unsafe_unretained RMTexture *r = (RMTexture *)pass.stencilResolve)
        stencilResolve_ = {r, pass.stencilPick};
    separateResolved_[0] = separateResolved_[1] = nullptr;
    if (__unsafe_unretained RMTexture *z = (RMTexture *)pass.depth) {  // (rawDepthStencil: decided at record)
        __unsafe_unretained RMTexture *s = (RMTexture *)pass.zeta[1].texture;
        const bool separate = s && s != z;
        p.depthStencil.texture = &z->_raw;
        if (!separate) {
            const NSUInteger level = pass.zeta[0].texture ? pass.zeta[0].level : pass.zeta[1].level;
            if (level) {
                raw::chainLevelImage(z->_raw, uint32_t(level), zetaPart_);
                p.depthStencil.texture = &zetaPart_;
            }
            if (pass.depthSlice && !z->_raw.memoryless) {  // a layer of a 2D array or cube: its layers from there
                zetaPart_ = raw::layersFrom(*p.depthStencil.texture, uint32_t(pass.depthSlice));
                p.depthStencil.texture = &zetaPart_;
            }
        }
        p.depthStencil.clearDepth = pass.depthLoad == MTLLoadActionClear;
        p.depthStencil.clearStencil = pass.stencilLoad == MTLLoadActionClear;
        p.depthStencil.loaded = pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad;
        p.depthStencil.stored =
            pass.zeta[0].store != MTLStoreActionDontCare || pass.zeta[1].store != MTLStoreActionDontCare;
        // an encode ahead has no later pass that loads it (ineligible())
        p.depthStencil.loadedLater =
            whole_ && std::any_of(cb->_passes.begin() + c.x + 1, cb->_passes.end(), [&](const RMPass &later) {
                return later.depth == pass.depth && later.depthLoad == MTLLoadActionLoad;
            });
        p.depthStencil.depth = raw::metalClearDepth(pass.clearDepth, z->_raw.format);
        p.depthStencil.stencil = pass.clearStencil;
        if (separate) {
            separateZeta(pass, z, s, p);
            if (!depth16Zeta(pass, p))
                return false;
        }
    }
    p.width = uint32_t(std::min<NSUInteger>(pass.width, UINT32_MAX));
    p.height = uint32_t(std::min<NSUInteger>(pass.height, UINT32_MAX));
    p.layers = pass.layers;
    p.positions = pass.positions;
    for (__unsafe_unretained RMTexture *t : standInLoads_)
        if (standInPass(t, false))
            return false;
    standInLoads_.clear();
    if (!loadPlanes(p) || !formatStandIns(pass, p))
        return false;
    PlanDraws draws(cb, at, to_, *rc);
    if (loadedTargets_ && !reachOfLoadedTargets(pass, p, draws, *rc, loadedReach_, why_))
        return fail(PredEncPlanarRT, *why_, loadedTargets_);
    if (const char *refusal = raw::expandXrPlanes(p, *d_))
        return fail(PredEncPassRender, refusal);
    if (!turned_.begin(*rc, p, draws, why_))
        return fail(PredPassRotated, *why_);
    if (pass.xrShadows && !rmXrShadowFill(rc, pass, p, why_))
        return fail(PredEncPassRender, *why_, pass.xrShadows);
    cb->_xrShadowCurrent = rmXrShadowTwinsOf(cb, uint32_t(c.x));
    p.metalDepth = true;
    if (!(re_ = rc->render(p, why_)))
        return passRefused(p.count);
    counters_->started(at, *re_);
    planSplitPrograms(cb, at, *re_);
    planned_ = raw::PassUses{};
    planned_.known = !sampledShadows_ && !standIns_;
    planPassUses(cb, at, planned_);
    return true;
}
