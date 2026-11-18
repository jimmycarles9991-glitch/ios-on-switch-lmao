// SPDX-License-Identifier: MIT
// The range encode's encoders as they end, the resolves and stand-in stores after them, and the range's commit
#import "RawMetalRangeEncode.h"
#import "RawMetalPassFusion.h"
#import "RawMetalResolveAsk.h"
#import "RawMetalTargetViews.h"
#include "raw_samples.h"

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
        if (const int failed =
                storeYcbcrPlanes(rc_, t->_raw, *backings_[j], why_, loadedTargets_ >> j & 1 ? &loadedReach_ : nullptr))
            return failed == 1 ? fail(PredEncPlanarRT, *why_, j) : refused();
        planesWritten_.push_back(t);
    }
    standInStores_.clear();
    loadedTargets_ = 0;
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
    const raw::Texture *depthFrom =
        depthResolve_.into ? (separateResolved_[0] ? separateResolved_[0] : re_->depthTarget()) : nullptr;
    const raw::Texture *stencilFrom =
        stencilResolve_.into ? (separateResolved_[1] ? separateResolved_[1] : re_->depthTarget()) : nullptr;
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
        raw::RenderEncoder *e = rc->render(p, why_);
        if (!e)
            return passRefused(r.attachment);
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
    cb->_xrShadowCurrent = nullptr;
    return true;
}

// The open encoders ended at an encoder's end, the render encoder told what its recording read first
bool RMRangeEncode::endEncoders()
{
    if (re_)
        re_->plan(planned_);
    return closeEncoders();
}

// The open encoders ended, their resolves and stand-ins after them
bool RMRangeEncode::closeEncoders()
{
    if (re_)
        re_->end();
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
    return storeStandIns() && storeFormatTargets() && depth16Back() && writtenBack();
}

bool RMRangeEncode::seal(size_t to, bool &stale)
{
    const Encoding encoding;
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    to_ = to;
    sealed_ = true;
    RAW_ENCODE_SPAN(Tail);
    if (!closeEncoders())
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
    const bool committed = rc->commit(seq);
    cb->_rangeQuiet = rc->quietEnd;
    if (!committed) {
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
        char reason[192];
        rc->describeRefusal(reason, sizeof reason);
        // as Metal fails a command buffer its driver could not submit: nothing of it runs elsewhere
        if (!ahead_ && !rc->partial && !rc->nativeError)
            failCommandBuffer(
                cb, [NSString stringWithFormat:@"The GPU driver did not submit the command buffer: %s", reason],
                MTLCommandBufferErrorInternal);
        return fail(PredEncKernel, "the kernel refused the submission", uint32_t(rc->kernelResult), 0, reason);
    }
    firstContents_->submitted(cb);
    if (rc->skips)
        hit(cb, refusalPred(rc->skipped.id), rc->skipped.a, uint64_t(rc->skipped.b) | uint64_t(rc->skipped.c) << 32,
            "a draw Apple's driver draws nothing defined for: skipped");
    publishSharedEvents(cb);
    if (rc->promisesKept) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promises.clear();
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb->_probeTaken.add(rc->probeTaken());
    noteReplayShape(cb, from_, to);
    d_->stats.slotsCompared += stateCursor_.slotsCompared;
    d_->stats.slotsInTables += stateCursor_.slotsInTables;
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
    RAW_DIAGNOSTIC(d_->stats.levelPasses += levelPasses_);
    RAW_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    RAW_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    RAW_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}
