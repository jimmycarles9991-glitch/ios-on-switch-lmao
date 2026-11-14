// SPDX-License-Identifier: MIT
#import "RawMetalRangeEncode.h"
#import "RawMetalPassFusion.h"
#import "RawMetalPassUses.h"
#import "RawMetalReach.h"
#import "RawMetalResolveAsk.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalTargetViews.h"
#include "raw_clear.h"
#include "raw_samples.h"

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

// The pass's backings of its YCbCr targets' stand-ins, read before the resolve passes take the encoder over, then the
// stand-ins stored into the planes
void RMRangeEncode::standInBackings()
{
    backings_ = {};
    for (const auto &[t, j] : standInStores_)
        if (t->_raw.ycbcrStore && re_)
            backings_[j] = re_->target(j);
}

bool RMRangeEncode::storeStandIns()
{
    for (const auto &[t, j] : standInStores_) {
        if (!t->_raw.ycbcrStore) {
            if (standInPass(t, true))
                return false;
            continue;
        }
        if (!backings_[j])
            return fail(PredEncPlanarRT, "a YCbCr texture's pass left no stand-in backing", j);
        if (const int failed = storeYcbcrPlanes(rc_, t->_raw, *backings_[j], why_))
            return failed == 1 ? fail(PredEncPlanarRT, *why_, j) : refused();
        planesWritten_.push_back(t);
    }
    standInStores_.clear();
    return true;
}

static const raw::DepthStencilState &depthWrites()
{
    static const raw::DepthStencilState state = [] {
        raw::DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return raw::DepthStencilState(desc);
    }();
    return state;
}

// Multisampled attachments' resolves (MTLStoreActionMultisampleResolve): after their pass, a pass of the pack's resolve state
// of the format each, into the resolve texture over the pass's render area (an 8-bit sRGB one as its UNORM twin, whose codes
// the state writes), sampling what the pass rendered (the resolve pass's prologue barrier orders its reads after those
// writes)
bool RMRangeEncode::resolvePasses()
{
    raw::Device &d = *d_;
    raw::CommandBuffer *rc = rc_;
    const raw::Texture *from[8] = {};
    for (const Resolve &r : resolves_)
        from[r.attachment] = re_->target(r.attachment);
    const raw::Texture *depthFrom = depthResolve_.into ? re_->depthTarget() : nullptr;
    const raw::Texture *stencilFrom = stencilResolve_.into ? re_->depthTarget() : nullptr;
    // a memoryless zeta's compressed backing is made without a texture header (Device::compBacking)
    if (const raw::Texture *z = depthFrom ? depthFrom : stencilFrom; z && !d.sampleable(*const_cast<raw::Texture *>(z)))
        return fail(PredEncDepthStencil, "no texture header for a multisampled zeta's resolve", z->format);
    const uint32_t width = re_->areaWidth(), height = re_->areaHeight();
    for (const Resolve &r : resolves_) {
        raw::Texture part;
        const raw::Texture *into = rawAttachmentTarget(r.into, r.level, r.slice, false, part);
        if (!into)
            return fail(PredEncPassRender, "a resolve level, slice or plane the raw path cannot render", r.attachment);
        const raw::ResolveAsk ask = resolveAsk(r.into, *into, *from[r.attachment], width, height);
        raw::ResolveRoute route = raw::resolveRoute(ask);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (d.probeResolveDraw.load(std::memory_order_relaxed))
            route = raw::ResolveByPass;
        else if (raw::twodResolveComplementShape(ask) && d.probeResolveExact.load(std::memory_order_relaxed))
            route = raw::ResolveByComplement;
#endif
        // a turned target's resolve is its state's turned variant, which reads the frame's position
        const bool turned = r.into->_raw.rotation;
        if (turned)
            route = raw::ResolveByPass;
        const bool twod = route != raw::ResolveByPass, exact = route == raw::ResolveByComplement;
        if (twod) {
            useTexture(rc, r.into, true);
            if (rc->resolveTwod(*from[r.attachment], *into, exact, r.keepSamples))
                continue;
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (!turned && rc->probeForms().resolveTwoD) {
            useTexture(rc, r.into, true);
            if (rc->probeResolve(*from[r.attachment], *into))
                continue;
        }
#endif
        resolveTargets_.push_back(*into);
        resolveTargets_.back().format = raw::resolveTargetFormat(into->format);
        raw::PassDesc p;
        p.color[0].texture = &resolveTargets_.back();
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        p.width = width;
        p.height = height;
        useTexture(rc, r.into, true);
        const uint32_t samples = from[r.attachment]->samples;
        raw::Pipeline *resolve = d.resolvePipeline(r.into->_raw.format, samples);
        if (!resolve)
            return fail(PredEncPassRender, "a resolve format with no resolve state", r.attachment);
        raw::RenderEncoder *e = rc->render(p);
        if (!e)
            return fail(PredEncPassRender, "a resolve texture the raw path cannot render", r.attachment);
        e->setPipeline(resolve);
        e->setTexture(0, from[r.attachment]);
        e->setBytes(2, 0, &samples, sizeof samples);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        e->end();
        RAW_DIAGNOSTIC(d.stats.resolveRoute[raw::ResolveByPass][raw::resolveSampleSlot(samples)]++);
    }
    resolves_.clear();
    if (__unsafe_unretained RMTexture *into = depthResolve_.into) {
        raw::PassDesc p;
        p.depthStencil.texture = &into->_raw;
        p.width = width;
        p.height = height;
        useTexture(rc, into, true);
        raw::RenderEncoder *e = depthFrom ? rc->render(p) : nullptr;
        if (!e)
            return fail(PredEncDepthStencil, "a depth resolve texture the raw path cannot render", into->_raw.format);
        const uint32_t samples = depthFrom->samples;
        e->setPipeline(d.depthResolvePipeline(into->_raw.format, depthResolve_.filter));
        e->setDepthStencil(&depthWrites());
        e->setTexture(0, depthFrom);
        e->setBytes(2, 0, &samples, sizeof samples);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        e->end();
        depthResolve_ = {};
    }
    if (__unsafe_unretained RMTexture *into = stencilResolve_.into) {
        useTexture(rc, into, true);
        if (!stencilFrom)
            return fail(PredEncDepthStencil, "a stencil resolve without its source", into->_raw.format);
        if (!rc->stencilResolve(into->_raw, *stencilFrom, stencilResolve_.pick, width, height, why_))
            return fail(PredEncDepthStencil, *why_, into->_raw.format);
        stencilResolve_ = {};
    }
    return true;
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
            if (a.load == MTLLoadActionLoad || (a.load == MTLLoadActionDontCare && stores && !t->_raw.ycbcr))
                standInLoads_.push_back(t);
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
        p.count = j + 1;
    }
    if (__unsafe_unretained RMTexture *r = (RMTexture *)pass.depthResolve)
        depthResolve_ = {r, pass.depthFilter};
    if (__unsafe_unretained RMTexture *r = (RMTexture *)pass.stencilResolve)
        stencilResolve_ = {r, pass.stencilPick};
    if (__unsafe_unretained RMTexture *z = (RMTexture *)pass.depth) {  // (rawDepthStencil: decided at record)
        p.depthStencil.texture = &z->_raw;
        if (pass.depthSlice && !z->_raw.memoryless) {  // a layer of a 2D array or cube: its layers from there
            zetaPart_ = z->_raw;
            zetaPart_.va += pass.depthSlice * z->_raw.layerBytes;
            zetaPart_.layers -= uint32_t(pass.depthSlice);
            zetaPart_.tic = 0;
            p.depthStencil.texture = &zetaPart_;
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
        p.depthStencil.depth = float(std::clamp(pass.clearDepth, 0.0, 1.0));
        p.depthStencil.stencil = pass.clearStencil;
        if (__unsafe_unretained RMTexture *s = (RMTexture *)pass.zeta[1].texture; s && s != z) {
            p.separate = {&z->_raw,
                          &s->_raw,
                          pass.depthLoad == MTLLoadActionLoad,
                          pass.stencilLoad == MTLLoadActionLoad,
                          pass.zeta[0].store == MTLStoreActionStore,
                          pass.zeta[1].store == MTLStoreActionStore};
            useTexture(rc, z, true);
            useTexture(rc, s, true);
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
    PlanDraws draws(cb, at, to_, *rc);
    if (const char *refusal = raw::expandXrPlanes(p, *d_))
        return fail(PredEncPassRender, refusal);
    if (!turned_.begin(*rc, p, draws, why_))
        return fail(PredPassRotated, *why_);
    if (!(re_ = rc->render(p)))
        return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
    counters_->started(at, *re_);
    planSplitPrograms(cb, at, *re_);
    planned_ = raw::PassUses{};
    planned_.known = !sampledShadows_ && !standIns_;
    planPassUses(cb, at, planned_);
    return true;
}

bool RMRangeEncode::endEncoder(size_t at)
{
    RMCommandBuffer *cb = cb_;
    if (re_)
        re_->finishVisibility();
    // encoded ahead, the fresh textures the next encoder uses first are written between the two, so it fuses with none
    bool fresh = false;
    if (!whole_ && at + 1 < to_ && RMCommandPlan::isEncoderBegin(cb->_plan.operation(at + 1).op)) {
        freshAt_ = at + 1;
        fresh = firstContents_->claim(cb, at + 1, cb->_plan.encoderEnd(at + 1));
    }
    if (re_ && !fresh && at + 1 < to_ && cb->_plan.operation(at + 1).op == OpRender) {
        if (rmContinuesPass(cb, at + 1)) {
            continuing_ = true;
            return true;
        }
        if (!re_->turned() && !rmGroupedPass(cb, renderBegin_) && !rmGroupedPass(cb, at + 1) && !counters_->at(at) &&
            !counters_->at(at + 1)) {
            // the next pass's facts are the next boundary's first pass's
            const bool shadows = sampledShadows_ || standIns_;
            const raw::FusionPass first = fusedAt_ == renderBegin_ ? fused_ : fusionPass(cb, renderBegin_, shadows);
            fused_ = fusionPass(cb, at + 1, shadows);
            fusedAt_ = at + 1;
            if (raw::canFusePasses(first, fused_)) {
                continuing_ = true;
                return true;
            }
        }
    }
    if (!endEncoders())
        return false;
    if (fresh)
        firstContents_->written(*rc_);
    counters_->ended(at);
    re_ = nullptr;
    be_ = nullptr;
    return true;
}

// The open encoders ended, their resolves and stand-ins after them
bool RMRangeEncode::endEncoders()
{
    if (re_) {
        re_->plan(planned_);
        re_->end();
    }
    standInBackings();
    if (re_ && !resolvePasses())
        return false;
    if (be_)
        be_->end();
    if (ce_) {
        ce_->end();
        ce_.reset();
        endKernelShadows();
    }
    if (!turned_.end(*rc_, why_))
        return fail(PredPassRotated, *why_);
    return storeStandIns();
}

bool RMRangeEncode::seal(size_t to, bool &stale)
{
    const Encoding encoding;
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    raw::Device &d = *d_;
    to_ = to;
    sealed_ = true;
    RAW_ENCODE_SPAN(Tail);
    if (re_)
        re_->end();
    standInBackings();
    if (re_ && !resolvePasses())
        return false;
    if (be_)
        be_->end();
    if (ce_) {
        ce_->end();
        endKernelShadows();
    }
    if (!turned_.end(*rc, why_))
        return fail(PredPassRotated, *why_);
    if (!storeStandIns())
        return false;
    encoder_ = SIZE_MAX;
    uint32_t seq = 0;
    prepareSharedEvents(cb, rc);
    rc->quietEnd = cb->_rangeQuiet;
    const bool lastRange = cb->_segNext + 1 == cb->_segs.size() && cb->_promised.load(std::memory_order_acquire);
    if (lastRange) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promisesTaken = true;
        rc->promises = cb->_promises;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    splitCaptureCommitted(*rc);
#endif
    if (!rc->commit(seq)) {
        cb->_partial = rc->partial;
        if (rc->partial)
            firstContents_->submitted(cb);
        if (rc->admissionBlocked) {
            RMQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            cb->_resourceReady = rc->resourceReadiness(
                [queue] {
                    wakeSubmit((__bridge void *)queue);
                },
                armed);
            if (!cb->_resourceReady) {
                std::lock_guard<raw::Mutex> lock(cb->_lock);
                cb->_segError = [NSError
                    errorWithDomain:MTLCommandBufferErrorDomain
                               code:MTLCommandBufferErrorInternal
                           userInfo:@{
                               NSLocalizedDescriptionKey : @"Resource readiness notification could not be armed"
                           }];
            }
            rc->abandon();
            return false;
        }
        if (!rc->setRefused.empty() && !rc->partial) {
            failCommandBuffer(cb, [NSString stringWithUTF8String:rc->setRefused.c_str()]);
            rc->abandon();
            return false;
        }
        stale = rc->nativeError == kIOReturnNotReady && !rc->partial;
        if (stale)
            rc->invalidateNativeReads();
        else if (rc->nativeError && !rc->partial)
            failCommandBuffer(cb, @"Native ownership or event dependency failed");
        if (stale) {
            rc->abandon();
            return false;
        }
        return fail(PredEncKernel, "the kernel refused the submission");
    }
    firstContents_->submitted(cb);
    publishSharedEvents(cb);
    if (rc->promisesKept) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promises.clear();
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb->_probeTaken.add(rc->probeTaken());
    noteReplayShape(cb, from_, to);
    d.stats.slotsCompared += stateCursor_.slotsCompared;
    d.stats.slotsInTables += stateCursor_.slotsInTables;
#endif
    for (const Conversion &c : converted_) {  // (submitted: their images hold the surfaces at these seeds)
        c.t->_imageSeed = c.seed;
        c.t->_imageWrites = c.writes;
        c.t->_imageNativeGeneration = c.nativeGeneration;
        c.t->_imageCurrent = true;
    }
    noteSurfaceWrites(cb, from_, to);
    for (__unsafe_unretained RMTexture *t :
         planesWritten_)  // a write the surface's seed does not show: converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    cb->_channelGeneration = generation_;
    cb->_stamp = cb->_segs.size() == 1 && cb->_segs.front().raw ? rc->stamp : 0;
    cb->_rangeLoopStamp = rc->loopsLaid ? rc->stamp : 0;
    RAW_DIAGNOSTIC(d.stats.levelPasses += levelPasses_);
    RAW_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    RAW_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    RAW_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}
