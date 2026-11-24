// SPDX-License-Identifier: MIT
// The range encode's encoders as they end, the stand-in stores after them (the resolves: TegraGM20BMetalRangeResolve.mm), and the
// range's commit
#import "TegraGM20BMetalRangeEncode.h"
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalPassFusion.h"
#import "TegraGM20BMetalSplitPlan.h"
#import "TegraGM20BMetalTargetViews.h"
#include "plain_rows.h"
#include "ycbcr_load.h"
#include "xr_pools.h"

#include <algorithm>

// The pass's backings of its YCbCr targets' stand-ins, read before the resolve passes take the encoder over, then the
// stand-ins stored into the planes
void SwitchMetalRangeEncode::standInBackings()
{
    backings_ = {};
    for (const auto &[t, j] : standInStores_)
        if (t->_raw.ycbcrStore && re_)
            backings_[j] = re_->target(j);
}

bool SwitchMetalRangeEncode::storeStandIns()
{
    for (const auto &[t, j] : standInStores_) {
        if (!t->_raw.ycbcrStore) {
            if (standInPass(t, j, true))
                return false;
            continue;
        }
        if (!backings_[j]) {
            constexpr const char *kNoBacking = "a YCbCr texture's pass left no stand-in backing";
            gm20bPassWorkFails(cb_, ahead_, kNoBacking, MTLCommandBufferErrorInternal);
            return fail(PredEncPlanarRT, kNoBacking, j);
        }
        const gm20b::PassReach *reach = loadedTargets_ >> j & 1 ? &loadedReach_ : nullptr;
        if (const int failed = storeYcbcrPlanes(rc_, t->_raw, *backings_[j], why_, reach)) {
            if (failed == 2) {
                gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPlanarRT, *why_, j);
        }
        planesWritten_.push_back(t);
    }
    standInStores_.clear();
    loadedTargets_ = 0;
    return true;
}

bool SwitchMetalRangeEncode::storeOverlappingPlanes()
{
    // a pass that loads its planes and draws nothing stores nothing (an M4 Max over planes it reads and stores at other
    // rows: m2v-textures12 probes/f2/targets-full.txt #168, #169)
    for (SwitchMetalTexture *t : overlapStores_) {
        const bool loaded = std::find(overlapLoaded_.begin(), overlapLoaded_.end(), t) != overlapLoaded_.end();
        if (!passDrew_ && loaded)
            continue;
        int failed = 0;
        if (t->_raw.xrPlanes) {
            failed = gm20b::storeXrPlaneRows(*d_, *rc_, t->_raw, !passDrew_, why_);
        } else {
            const gm20b::TurnMask mask =
                loaded && loadedReach_.mask ? gm20b::ycbcrStoreMask(t->_raw, loadedReach_) : gm20b::TurnMask{};
            failed = gm20b::storePlainRows(*d_, *rc_, t->_raw, mask.mask ? &mask : nullptr, why_);
        }
        if (failed) {
            if (failed == 2) {
                gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredEncPlanarRT, *why_);
        }
    }
    overlapStores_.clear();
    overlapLoaded_.clear();
    return true;
}

// A pass fused into the open one (beginPass's continuing_) draws on the targets that one's begin laid out: the colours
// its draws bind as storage are taken where that layout holds them (gm20b::RenderEncoder::takeStorage)
static bool storageLaidOut(SwitchMetalCommandBuffer *cb, gm20b::RenderEncoder &open, size_t next)
{
    gm20b::PassDesc b;
    passStorageMasks(cb, next, b);
    return !b.storageMask || open.takeStorage(b.storageMask, b.arrayedStorageMask);
}

// A Depth16Unorm a draw of the pass after `open` tests in its shader is the converted zeta only where `open` began it so
// (TegraGM20BMetalRangeForms.mm depth16Tested); a zeta of any other format is the same however its pass began
static bool testedLaidOut(SwitchMetalCommandBuffer *cb, size_t open, size_t next)
{
    const SwitchMetalPass &pass = cb->_passes[cb->_plan.legacy(next).x];
    __unsafe_unretained SwitchMetalTexture *depth = (SwitchMetalTexture *)pass.zeta[0].texture;
    return !pass.shaderTestsDepth || !depth || depth->_raw.format != MTLPixelFormatDepth16Unorm ||
           cb->_passes[cb->_plan.legacy(open).x].shaderTestsDepth;
}

bool SwitchMetalRangeEncode::endEncoder(size_t at)
{
    SwitchMetalCommandBuffer *cb = cb_;
    if (re_)
        re_->finishVisibility();
    // encoded ahead, the fresh textures the next encoder uses first are written between the two, so it fuses with none;
    // what follows an end there and begins no encoder was recorded on the ended one, which its commit moves inside it
    bool fresh = false;
    if (!whole_ && at + 1 < to_) {
        if (!SwitchMetalCommandPlan::isEncoderBegin(cb->_plan.operation(at + 1).op)) {
            rc_->abandon();
            abandoned_ = true;
            return stop(VetoOps);
        }
        freshAt_ = at + 1;
        fresh = firstContents_->claim(cb, at + 1, cb->_plan.encoderEnd(at + 1));
    }
    if (re_ && !fresh && at + 1 < to_ && cb->_plan.operation(at + 1).op == OpRender) {
        if (gm20bContinuesPass(cb, at + 1)) {
            continuing_ = true;
            return true;
        }
        if (!re_->turned() && !gm20bGroupedPass(cb, renderBegin_) && !gm20bGroupedPass(cb, at + 1) &&
            !counters_->at(at) && !counters_->at(at + 1)) {
            // the next pass's facts are the next boundary's first pass's
            const bool shadows = sampledShadows_ || standIns_;
            const gm20b::FusionPass first = fusedAt_ == renderBegin_ ? fused_ : fusionPass(cb, renderBegin_, shadows);
            fused_ = fusionPass(cb, at + 1, shadows);
            fusedAt_ = at + 1;
            if (gm20b::canFusePasses(first, fused_) && testedLaidOut(cb, renderBegin_, at + 1) &&
                storageLaidOut(cb, *re_, at + 1)) {
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

// The open encoders ended at an encoder's end, the render encoder told what its recording read first
bool SwitchMetalRangeEncode::endEncoders()
{
    if (re_)
        re_->plan(planned_);
    return closeEncoders();
}

// The open encoders ended, their resolves and stand-ins after them
bool SwitchMetalRangeEncode::closeEncoders()
{
    passDrew_ = rc_->draws != passDraws_;
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
    if (!turned_.end(*rc_, why_)) {
        gm20bPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
        return fail(PredPassRotated, *why_);
    }
    if (!storeStandIns() || !storeOverlappingPlanes() || !storeFormatTargets())
        return false;
    planesBack();
    return depth16Back() && writtenBack();
}

bool SwitchMetalRangeEncode::seal(size_t to)
{
    SwitchMetalCommandBuffer *cb = cb_;
    gm20b::CommandBuffer *rc = rc_;
    to_ = to;
    sealed_ = true;
    SWITCH_METAL_ENCODE_SPAN(Tail);
    if (!closeEncoders())
        return false;
    encoder_ = SIZE_MAX;
    uint32_t seq = 0;
    prepareSharedEvents(cb, rc);
    rc->quietEnd = cb->_rangeQuiet;
    const bool lastRange = cb->_segNext + 1 == cb->_segs.size() && cb->_promised.load(std::memory_order_acquire);
    if (lastRange) {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        cb->_promisesTaken = true;
        rc->promises = cb->_promises;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    splitCaptureCommitted(*rc);
#endif
    const bool committed = rc->commit(seq);
    cb->_rangeQuiet = rc->quietEnd;
    if (!committed) {
        if (rc->admissionBlocked) {
            SwitchMetalQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            cb->_resourceReady = rc->resourceReadiness(
                [queue] {
                    wakeSubmit((__bridge void *)queue);
                },
                armed);
            if (!cb->_resourceReady) {
                std::lock_guard<gm20b::Mutex> lock(cb->_lock);
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
        // its promises were given up at the holder's bound before this keep: it fails as one held past it
        if (rc->kernelResult == kIOReturnTimeout && !rc->promises.empty()) {
            heldTooLong(cb, 0, 0);
            rc->abandon();
            return false;
        }
        if (!rc->setRefused.empty() && !rc->partial) {
            failCommandBuffer(cb, [NSString stringWithUTF8String:rc->setRefused.c_str()]);
            rc->abandon();
            return false;
        }
        // A view the kernel found stale stays stale for every later command buffer, and one whose seed did not move (another
        // process's GPU write of its surface) would be refused again
        if (rc->nativeError == kIOReturnNotReady)
            rc->invalidateNativeReads();
        if (rc->nativeError && !rc->partial)
            failCommandBuffer(cb,
                              rc->nativeError == kIOReturnNotReady
                                  ? @"A surface it reads changed while it was submitted"
                                  : @"Native ownership or event dependency failed",
                              MTLCommandBufferErrorInternal, PredEncKernel);
        char reason[192];
        rc->describeRefusal(reason, sizeof reason);
        // as Metal fails a command buffer its driver could not submit: nothing of it runs elsewhere
        if (!ahead_ && !rc->partial && !rc->nativeError)
            failCommandBuffer(
                cb, [NSString stringWithFormat:@"The GPU driver did not submit the command buffer: %s", reason],
                MTLCommandBufferErrorInternal);
        // a refusal after part of it ran: nothing of it may run again elsewhere
        if (!ahead_ && rc->partial)
            failCommandBuffer(cb,
                              [NSString stringWithFormat:@"The GPU driver ran part of the command buffer and the "
                                                         @"kernel refused the rest: %s",
                                                         reason],
                              MTLCommandBufferErrorInternal);
        return fail(PredEncKernel, "the kernel refused the submission", uint32_t(rc->kernelResult), 0, reason);
    }
    firstContents_->submitted(cb);
    if (rc->skips)
        skip(cb, refusalPred(rc->skipped.id), rc->skipped.a, uint64_t(rc->skipped.b) | uint64_t(rc->skipped.c) << 32,
             "a draw Apple's driver draws nothing defined for: skipped", rc->skips);
    publishSharedEvents(cb);
    if (rc->promisesKept) {
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
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
    for (__unsafe_unretained SwitchMetalTexture *t :
         planesWritten_)  // a write the surface's seed does not show: converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    cb->_channelGeneration = generation_;
    cb->_stamp = cb->_segs.size() == 1 && !cb->_segs.front().event ? rc->stamp : 0;
#if SWITCH_METAL_DIAGNOSTICS
    if (!cb->_firstStamp)
        cb->_firstStamp = rc->stamp;
#endif
    cb->_rangeLoopStamp = rc->loopsLaid ? rc->stamp : 0;
    SWITCH_METAL_DIAGNOSTIC(d_->stats.levelPasses += levelPasses_);
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    SWITCH_METAL_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}
