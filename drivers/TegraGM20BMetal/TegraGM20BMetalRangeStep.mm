// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalRangeEncode.h"
#import "TegraGM20BMetalPlanAdapters.h"
#import "TegraGM20BMetalPassErrors.h"
#import "TegraGM20BMetalPassFusion.h"
#import "TegraGM20BMetalReach.h"
#import "TegraGM20BMetalSplitPlan.h"
#import "TegraGM20BMetalIndirect.h"
#import "TegraGM20BMetalIndirectCompute.h"
#import "TegraGM20BMetalTargetViews.h"
#include "compute_lookup.h"
#include "clear.h"
#include "srgb_pair.h"
#include "xr_parts.h"
#include "xr_pools.h"

static void noteWritten(std::vector<__unsafe_unretained SwitchMetalTexture *> &list, SwitchMetalTexture *t)
{
    if (std::find(list.begin(), list.end(), t) == list.end())
        list.push_back(t);
}

// textures sampled through a pool image (rows 16 but not 32 apart, gm20b::Texture::sampled): refreshed from their texels by the
// copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
// its shadow at each encoder that uses it once written since); a texture over a buffer, and a texel buffer held in a pool
// of a block-linear surface's rows (texel_rows.h), before every pass that samples it, as any work may write the
// buffer's bytes or the surface's rows. One the pass's draws store into is copied back after it.
void SwitchMetalRangeEncode::refresh(size_t pass)
{
    cb_->_plan.forEachResource(
        pass, cb_->_plan.encoderEnd(pass), SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
            if (!(use.access & gm20b::ResourceUse::Sample))
                return;
            __unsafe_unretained SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)use.object;
            if (gm20b::xrPlanesPooled(t->_raw)) {
                // what the pass writes of it lands in its planes' rows: their pools go stale
                const bool held =
                    (use.access & gm20b::ResourceUse::Write) ? fresh_.erase(t) != 0 : !fresh_.insert(t).second;
                if (!held)
                    gm20b::fillXrPools(*rc_, t->_raw);
                return;
            }
            const gm20b::Texture &x = t->_raw;
            if (rc_->readTexture(&x) != &x || !(x.sampled || x.texelRows))
                return;
            if (use.access & gm20b::ResourceUse::Write)
                noteWritten(shadowsWritten_, t);
            if (!x.surfaceView && !x.texelRows && t->_kind != SwitchMetalTextureBuffer && !fresh_.insert(t).second)
                return;
            if (x.texelRows)
                standInFresh_.erase(t);
            gm20b::BlitEncoder *b = rc_->blit();
            gm20bMoveShadowImage(*b, x, true);
            b->end();
        });
}

// A stand-in's conversion that failed: the stand-in and its pass are the driver's own, so the command buffer fails with
// the driver's error or for want of memory, its line naming the step
static void standInFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, MTLCommandBufferError code)
{
    if (ahead)
        return;
    SWITCH_REFUSE_DETAIL("gm20b", "standin.conversion", uintptr_t(why), "%s: %s (MTLCommandBuffer.h)", why,
                         code == MTLCommandBufferErrorOutOfMemory ? "MTLCommandBufferErrorOutOfMemory"
                                                                  : "MTLCommandBufferErrorInternal");
    failCommandBuffer(cb, @(why), code);
}

// extended-range textures (xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
// before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
// orders the conversion between the codes' writes and the samples.
bool SwitchMetalRangeEncode::standIns(size_t pass)
{
    std::vector<__unsafe_unretained SwitchMetalTexture *> stale;
    cb_->_plan.forEachResource(
        pass, cb_->_plan.encoderEnd(pass), SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
            if (!(use.access & gm20b::ResourceUse::Sample))
                return;
            __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
            if ((use.access & gm20b::ResourceUse::Write) && gm20bStandInBacked(texture))
                noteWritten(standInsWritten_, texture);
            __unsafe_unretained SwitchMetalTexture *holder = gm20bStandInHolder(texture);
            if (texture->_raw.standIn && !standInFresh_.count(holder) &&
                (!texture->_raw.xrPlanes || cb_->_xrStandIns.count(texture)) &&
                std::find(stale.begin(), stale.end(), holder) == stale.end())
                stale.push_back(holder);
        });
    return drawStandIns(stale);
}

bool SwitchMetalRangeEncode::drawStandIns(const std::vector<__unsafe_unretained SwitchMetalTexture *> &stale)
{
    // one pass of the conversion state from `from` into `into`, `width` x `height` of each; false: refused
    auto convert = [&](gm20b::Texture *into, const gm20b::Texture *from, const gm20b::Texture &x, uint32_t width,
                       uint32_t height) -> bool {
        gm20b::PassDesc p;
        p.color[0].texture = into;
        p.color[0].load = gm20b::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        p.width = width;
        p.height = height;
        // a pass into one stand-in the device made as a target: render() refuses it only for want of a backing
        gm20b::RenderEncoder *conv = rc_->render(p, why_);
        if (!conv) {
            standInFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPassRender, *why_);
        }
        conv->setPipeline(d_->ownPipeline(gm20b::xrConversionOf(x.format, x.xrPlanes)));
        conv->setTexture(0, from);
        if (x.xrPlanes)
            conv->setTexture(1, x.chroma);
        conv->setViewport(0, 0, width, height, 0, 1);
        conv->setScissor(0, 0, width, height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_)) {
            gm20bOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
            return refused();
        }
        conv->end();
        return true;
    };
    for (__unsafe_unretained SwitchMetalTexture *t : stale) {
        codes_.push_back(gm20bStandInCodes(t));
        gm20b::Texture x = codes_.back();
        gm20b::Texture &own = codes_.back();
        own.standIn = nullptr;
        useTexture(rc_, t, false);
        if (gm20b::srgbPair(x.format)) {  // RG8Unorm_sRGB: its bytes widened by the copy engine (srgb_pair.h)
            gm20b::BlitEncoder *b = rc_->blit();
            const bool widened = b->widenSrgbPair(x);
            b->end();
            if (!widened) {
                constexpr const char *kUnlike = "an RG8Unorm_sRGB texture whose stand-in is not of its texels";
                standInFails(cb_, ahead_, kUnlike, MTLCommandBufferErrorInternal);
                return fail(PredEncPassRender, kUnlike);
            }
            standInFresh_.insert(t);
            continue;
        }
        if (x.type == gm20b::fx::kTexOneDBuffer) {
            // a texel buffer's codes copied in order into images of its stand-in's rows (Device::createTexelStandIn),
            // a two-plane one's alpha bytes beside its words, then converted as a 2D image
            gm20b::Texture &words = *x.standIn->luma;
            gm20b::BlitEncoder *b = rc_->blit();
            rc_->use(words, true);
            b->copyBytes(x.va, words.va, uint64_t(x.width) * x.bpp);
            if (gm20b::twoPlaneXr(x.format)) {
                rc_->use(*x.standIn->chroma, true);
                b->copyBytes(x.va + gm20b::texelBufferAlphaOffset(x.width), x.standIn->chroma->va, x.width);
                x.xrPlanes = true;
                x.chroma = x.standIn->chroma;
            }
            b->end();
            if (!convert(x.standIn, &words, x, x.standIn->width, x.standIn->height))
                return false;
            standInFresh_.insert(t);
            continue;
        }
        const bool one = x.xrPlanes || (!x.chain && x.layers <= 1 && x.samples <= 1 && x.type == gm20b::fx::kTexTwoD);
        if (one) {
            if (!convert(x.standIn, x.xrPlanes ? x.luma : &own, x, x.width, x.height))
                return false;
            standInFresh_.insert(t);
            continue;
        }
        // a layered, mipmapped, 3D or multisampled one: a pass a pair of 2D images over the same texels (xr_parts.h)
        std::vector<gm20b::XrPart> parts;
        if (!gm20b::xrParts(own, *x.standIn, parts)) {
            constexpr const char *kUnlike = "an extended-range texture whose stand-in is not of its texels";
            standInFails(cb_, ahead_, kUnlike, MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, kUnlike);
        }
        for (gm20b::XrPart &part : parts) {
            codeParts_.push_back(part);
            gm20b::XrPart &held = codeParts_.back();
            held.codes.tic = d_->attachmentTic(own, held.codes, held.level, held.key);
            held.standIn.tic = d_->attachmentTic(*x.standIn, held.standIn, held.level, held.key);
            if (!held.codes.tic || !held.standIn.tic) {
                constexpr const char *kNoHeader = "no header left for an extended-range texture's conversion";
                standInFails(cb_, ahead_, kNoHeader, MTLCommandBufferErrorOutOfMemory);
                return fail(PredEncPassRender, kNoHeader);
            }
            if (!convert(&held.standIn, &held.codes, x, held.codes.width, held.codes.height))
                return false;
        }
        standInFresh_.insert(t);
    }
    return true;
}

// setVisibility refuses a mode set at a result word its Metal encoder set before (Metal's validation: "Using the same
// offset in the visibility buffer twice in the same command encoder is not allowed"; an offset past
// maxVisibilityQueryOffset lands on one) and any mode once the device could not make its report page
// (visibility_pass.cpp, made once): a mode the encoder set before took a slot, so the page was there
static bool modeSetBefore(SwitchMetalCommandBuffer *cb, size_t at)
{
    for (size_t k = at; k-- > 0 && cb->_plan.operation(k).op != OpRender;)
        if (cb->_plan.operation(k).op == OpVisibility && cb->_plan.legacy(k).a != MTLVisibilityResultModeDisabled)
            return true;
    return false;
}

static void visibilityFails(SwitchMetalCommandBuffer *cb, const Cmd &c, const char *why, bool twice)
{
    if (twice) {
        visibilityError(cb, c.a, c.c, why);
        return failCommandBuffer(cb, [NSString stringWithFormat:@"setVisibilityResultMode:offset: %s: Metal's "
                                                                @"validation error (MTLRenderCommandEncoder.h)",
                                                                why]);
    }
    if (!predLogged(PredCallVisibility))
        logPred(PredCallVisibility, c.a, c.c,
                [NSString stringWithFormat:@"%s: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)", why]);
    failCommandBuffer(cb, @(why), MTLCommandBufferErrorOutOfMemory);
}

bool SwitchMetalRangeEncode::command(size_t at)
{
    SwitchMetalCommandBuffer *cb = cb_;
    gm20b::CommandBuffer *rc = rc_;
    const Cmd c = cb->_plan.legacy(at);
    // a compute command the encode refused fails its command buffer as its encoder names it (gm20b::ComputeFailure);
    // predication records the recording found misused are the client's
    const auto computeRefused = [&](SwitchMetalPred p, const char *w, uint64_t a) {
        if (!ahead_)
            gm20bComputeFails(cb,
                              p == PredComputePredicate && cb->_predicates.misused() ? gm20b::ComputeFailure::Invalid
                                                                                     : ce_->failure(),
                              p, a, w);
        return fail(p, w, a);
    };
    if (at == from_)  // a range begun inside a compute encoder (open) is inside that encoder's unit
        unitWithheld_ = computeUnit_ && cb->_predicates.recorded() && gm20bComputeUnitWithheld(cb, at, serialCompute_);
    if ((c.op == OpRender && !gm20bContinuesPass(cb, at)) || c.op == OpCompute || c.op == OpBlit) {
        encoder_ = at;
#if SWITCH_METAL_DIAGNOSTICS
        traceFreshUses(cb, at);
#endif
    }
    if (c.op == OpDispatch && unitWithheld_)
        return true;
    if (c.op == OpDispatch) {
        SWITCH_METAL_ENCODE_SPAN(Compute);
        const auto &dispatch = cb->_plan.dispatch(at);
        const auto *kernels = static_cast<const gm20b::ComputeKernelSet<gm20b::ComputeState> *>(dispatch.kernel);
        const auto &state = cb->_plan.kernelStates[dispatch.state];
        const auto &textures = cb->_plan.kernelTextures[state.textures].objects;
        dispatchUses_.clear();
        if (dispatch.surfaceSlots && !encoderShadows_) {  // a range begun inside its encoder
            ce_->end();
            ce_.reset();
            beginKernelShadows(dispatch.encoderBegin);
            ce_.emplace(*rc, *d_);
            ce_->setSerial(serialCompute_);
            stateCursor_.reset();
        }
        if (encoderTracked_) {
            kernelUses(cb, at, dispatchUses_);
            localShadows(dispatchUses_);
        }
        if (const auto &steps = kernelShadows_.before(dispatchUses_.data(), dispatchUses_.size()); !steps.empty()) {
            ce_->end();
            ce_.reset();
            shadowCopies(steps.copyBack, steps.fill);
            if (!steps.redraw.empty()) {
                std::vector<__unsafe_unretained SwitchMetalTexture *> redrawn;
                for (const void *o : steps.redraw) {
                    __unsafe_unretained SwitchMetalTexture *holder =
                        gm20bStandInHolder((__bridge SwitchMetalTexture *)o);
                    standInFresh_.erase(holder);
                    if (std::find(redrawn.begin(), redrawn.end(), holder) == redrawn.end())
                        redrawn.push_back(holder);
                }
                if (!drawStandIns(redrawn))
                    return false;
            }
            ce_.emplace(*rc, *d_);
            ce_->setSerial(serialCompute_);
            stateCursor_.reset();
        }
        encodePlannedState(cb, at, *rc, re_, &*ce_, stateCursor_);
        const auto &regions = dispatch.regions;
        ce_->setStageInRegion(dispatch.stageIn);
        ce_->setStageInRegionIndirect(
            dispatch.stageInBuffer ? &((__bridge SwitchMetalBuffer *)dispatch.stageInBuffer)->_raw : nullptr,
            dispatch.stageInOffset);
        // the recording fails a dispatch of no region plan (TegraGM20BMetalCompute.mm), and readiness resolves its kernels
        if (!kernels || kernels->count != regions.count) {
            constexpr const char *incomplete =
                "a dispatch whose kernels are not its region plan's: the driver's error, MTLCommandBufferErrorInternal";
            if (!ahead_)
                failCommandBuffer(cb, @(incomplete));
            return fail(PredCompute, incomplete, c.op, 0, incomplete);
        }
        if (dispatch.arguments) {
            ce_->setState(kernels->jobs[0]->value());
            if (const char *w = nullptr;
                !ce_->dispatchIndirect(&((__bridge SwitchMetalBuffer *)dispatch.arguments)->_raw,
                                       dispatch.argumentsOffset, regions.nominal, &w))
                return computeRefused(PredCompute, w, c.op);
        }
        for (uint8_t region = 0; region < regions.count && !dispatch.arguments; ++region) {
            ce_->setState(kernels->jobs[region]->value());
            if (const char *w = nullptr; !ce_->dispatchRegion(regions.regions[region], regions.grid, regions.nominal,
                                                              !region, region + 1 == regions.count, &w))
                return computeRefused(PredCompute, w, c.op);
        }
        kernelShadows_.after(dispatchUses_.data(), dispatchUses_.size());
        if (standIns_ && !standInFresh_.empty())
            for (uint8_t region = 0; region < kernels->count; ++region) {
                const auto &record = *kernels->jobs[region]->value()->rec;
                for (uint32_t i = 0; i < record.entries; ++i)
                    if (record.entry[i].kind == gm20b::KernelTexture && record.entry[i].slot < 31 &&
                        gm20b::kernelEntryWrites(record, i))
                        standInFresh_.erase((__bridge SwitchMetalTexture *)textures[record.entry[i].slot]);
            }
        return true;
    }
    {
        SWITCH_METAL_ENCODE_SPAN(State);
        encodePlannedState(cb, at, *rc, re_, ce_ ? &*ce_ : nullptr, stateCursor_);
    }
    if (isKernelCommand(c.op)) {
        SWITCH_METAL_ENCODE_SPAN(Compute);
        if (unitWithheld_ && (c.op == OpExecuteKernels || c.op == OpPredicate))
            return true;
        if (const char *w = nullptr; !encodeKernel(*ce_, *rc, cb, c, at, &w))
            return computeRefused(c.op == OpPredicate        ? PredComputePredicate
                                  : c.op == OpExecuteKernels ? PredComputeKernel
                                                             : PredCompute,
                                  w, c.op);
        return true;
    }
    switch (c.op) {
    case OpCompute:
        if (!computeUnit_ || serialCompute_ != (c.a == MTLDispatchTypeSerial)) {
            rc->endPredicationUnit();
            unitWithheld_ =
                cb->_predicates.recorded() && gm20bComputeUnitWithheld(cb, at, c.a == MTLDispatchTypeSerial);
        }
        computeUnit_ = true;
        beginKernelShadows(at);
        if (standIns_ && !standIns(at))
            return false;
        ce_.emplace(*rc, *d_);
        serialCompute_ = c.a == MTLDispatchTypeSerial;
        ce_->setSerial(serialCompute_);
        counters_->started(at, *ce_);
        break;
    case OpRender:
        return beginPass(at);
    case OpBlit:
        computeUnit_ = false;
        rc->endPredicationUnit();
        be_ = rc->blit();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        for (const auto &o : cb->_probeOps)
            if (o.at == at)
                be_->probe(o.op, o.read ? &o.read->_raw : nullptr, o.written->_raw);
#endif
        counters_->started(at, *be_);
        break;
    case OpEnd:
        return endEncoder(at);
    case OpDraw:
        if (!re_->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why_))
            return refused();
        break;
    case OpBarrier:
        re_->barrier();
        break;
    case OpVisibility:
        if (!re_->setVisibility(c.a,
                                [(__bridge id)c.obj isKindOfClass:[SwitchMetalBuffer class]]
                                    ? &((__bridge SwitchMetalBuffer *)c.obj)->_raw
                                    : nullptr,
                                c.c, c.b != 0)) {
            const bool twice = modeSetBefore(cb, at);
            const char *why = twice ? "an offset its encoder set before, or one past maxVisibilityQueryOffset onto it"
                                    : "no memory for the device's visibility report page";
            if (!ahead_)
                visibilityFails(cb, c, why, twice);
            return fail(PredCallVisibility, why, c.a, c.c);
        }
        break;
    case OpDrawIndexed: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained SwitchMetalBuffer *indices = (__bridge SwitchMetalBuffer *)draw.indices;
        useBuffer(rc, indices, false);
        if (!re_->drawIndexed(draw.primitive, draw.count, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4, &indices->_raw,
                              draw.indexOffset, draw.instances, draw.baseVertex, draw.baseInstance, why_))
            return refused();
        break;
    }
    case OpDrawIndirect:
    case OpDrawIndexedIndirect: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained SwitchMetalBuffer *arguments = (__bridge SwitchMetalBuffer *)draw.arguments;
        __unsafe_unretained SwitchMetalBuffer *indices = (__bridge SwitchMetalBuffer *)draw.indices;
        useBuffer(rc, arguments, false);
        useBuffer(rc, indices, false);
        const bool accepted =
            c.op == OpDrawIndirect
                ? re_->drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why_)
                : re_->drawIndexedIndirect(draw.primitive, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4, &indices->_raw,
                                           draw.indexOffset, &arguments->_raw, draw.argumentOffset, why_);
        if (!accepted)
            return refused();
        break;
    }
    case OpExecuteCommands:
        if (!encodeCommands(re_, rc, cb->_plan.draw(at), why_))
            return refused();
        break;
    case OpDrawPatches:
    case OpDrawPatchesIndirect:
        if (!encodePatches(re_, rc, cb->_plan.draw(at), cb->_plan.patches(at), c.op == OpDrawPatchesIndirect, why_))
            return refused();
        break;
    case OpResetCommands:
    case OpCopyCommands:
        encodeCommandsBlit(rc, be_, c);
        break;
    case OpMipmaps: {
        SWITCH_METAL_ENCODE_SPAN(Blit);
        __unsafe_unretained SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)c.obj;
        if (!encodeMipmaps(rc, t, uint32_t(c.c), be_, why_)) {
            // every step was routed at record: what fails here is a header or a backing the device had no room for
            constexpr const char *memory = "no texture header or memory for a generateMipmapsForTexture: step";
            if (!ahead_) {
                SWITCH_REFUSE("gm20b", "mipmaps.memory",
                              "%s (%s): MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)", memory,
                              *why_ ? *why_ : "");
                failCommandBuffer(cb, @(memory), MTLCommandBufferErrorOutOfMemory);
            }
            return fail(PredEncPassRender, memory, t->_descriptor.pixelFormat);
        }
        if (!fresh_.empty())
            fresh_.erase(t);  // its levels written: its pool image and stand-in are refreshed again before a sample
        standInFresh_.erase(t);
        break;
    }
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpFillBuffer:
    case OpFillTexture:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer: {
        SWITCH_METAL_ENCODE_SPAN(Blit);
        blitTexelRows(c, true);
        if (!encodeBlit(cb, c, rc, be_, fresh_, standInFresh_, why_)) {
            constexpr const char *clamp =
                "no memory, header or pack state for a copy's alpha clamp (TegraGM20BMetalCopyClamp.h) "
                "or a decoded format's upload (decode_pass.h)";
            if (!ahead_) {
                SWITCH_REFUSE("gm20b", "copy.clamp", "%s (%s): MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)",
                              clamp, *why_ ? *why_ : "");
                failCommandBuffer(cb, @(clamp), MTLCommandBufferErrorOutOfMemory);
            }
            return fail(PredEncPassRender, clamp, uint32_t(c.op));
        }
        blitTexelRows(c, false);
        break;
    }
    case OpSignalEvent:
        if (!isInlineSignal(cb, at))
            break;
        computeUnit_ = false;
        rc->endPredicationUnit();
        if (!releaseInlineSignal(cb, at, rc)) {
            encoder_ = SIZE_MAX;
            const char *lost = "an inlined signal whose shared event holds no kernel lease, which its commit took "
                               "(inlineSignals): the driver's error, MTLCommandBufferErrorInternal";
            if (!ahead_) {
                eventError(cb, true, lost);
                failCommandBuffer(cb, @(lost));
            }
            return fail(PredEvent, lost);
        }
        break;
    default:  // states, fences, events, resources the driver needs nothing for
        break;
    }
    return true;
}
