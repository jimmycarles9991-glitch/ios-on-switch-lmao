// SPDX-License-Identifier: MIT
#import "RawMetalRangeEncode.h"
#import "RawMetalPlanAdapters.h"
#import "RawMetalPassFusion.h"
#import "RawMetalReach.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalIndirect.h"
#import "RawMetalTargetViews.h"
#include "raw_compute_lookup.h"
#include "raw_clear.h"
#include "raw_srgb_pair.h"
#include "raw_xr_parts.h"

static void noteWritten(std::vector<__unsafe_unretained RMTexture *> &list, RMTexture *t)
{
    if (std::find(list.begin(), list.end(), t) == list.end())
        list.push_back(t);
}

// textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
// copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
// its shadow at each encoder that uses it once written since); a texture over a buffer before every pass that samples
// it, as any work may write the buffer's bytes. One the pass's draws store into is copied back after it.
void RMRangeEncode::refresh(size_t pass)
{
    cb_->_plan.forEachResource(pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture,
                               [&](const RMRangeResource &use) {
                                   if (!(use.access & raw::ResourceUse::Sample))
                                       return;
                                   __unsafe_unretained RMTexture *t = (__bridge RMTexture *)use.object;
                                   if (rc_->readTexture(&t->_raw) != &t->_raw || !t->_raw.sampled)
                                       return;
                                   if (use.access & raw::ResourceUse::Write)
                                       noteWritten(shadowsWritten_, t);
                                   if (!t->_raw.surfaceView && t->_kind != RMTextureBuffer && !fresh_.insert(t).second)
                                       return;
                                   const raw::Texture &x = t->_raw;
                                   raw::BlitEncoder *b = rc_->blit();
                                   b->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
                                   b->end();
                               });
}

// extended-range textures (raw_xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
// before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
// orders the conversion between the codes' writes and the samples.
bool RMRangeEncode::standIns(size_t pass)
{
    std::vector<__unsafe_unretained RMTexture *> stale;
    cb_->_plan.forEachResource(pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture,
                               [&](const RMRangeResource &use) {
                                   if (!(use.access & raw::ResourceUse::Sample))
                                       return;
                                   __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
                                   if ((use.access & raw::ResourceUse::Write) && texture->_raw.standIn)
                                       noteWritten(standInsWritten_, texture);
                                   if (texture->_raw.standIn && !standInFresh_.count(texture) &&
                                       (!texture->_raw.xrPlanes || cb_->_xrStandIns.count(texture)))
                                       stale.push_back(texture);
                               });
    // one pass of the conversion state from `from` into `into`, `width` x `height` of each; false: refused
    auto convert = [&](raw::Texture *into, const raw::Texture *from, const raw::Texture &x, uint32_t width,
                       uint32_t height) -> bool {
        raw::PassDesc p;
        p.color[0].texture = into;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        p.width = width;
        p.height = height;
        raw::RenderEncoder *conv = rc_->render(p);
        if (!conv)
            return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
        conv->setPipeline(d_->xrPipeline(raw::xrForm(x.format), x.xrPlanes));
        conv->setTexture(0, from);
        if (x.xrPlanes)
            conv->setTexture(1, x.chroma);
        conv->setViewport(0, 0, width, height, 0, 1);
        conv->setScissor(0, 0, width, height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        conv->end();
        return true;
    };
    for (__unsafe_unretained RMTexture *t : stale) {
        raw::Texture &x = t->_raw;
        codes_.push_back(x);
        raw::Texture &own = codes_.back();
        own.standIn = nullptr;
        useTexture(rc_, t, false);
        if (raw::srgbPair(x.format)) {  // RG8Unorm_sRGB: its bytes widened by the copy engine (raw_srgb_pair.h)
            raw::BlitEncoder *b = rc_->blit();
            const bool widened = b->widenSrgbPair(x);
            b->end();
            if (!widened)
                return fail(PredEncPassRender, "an RG8Unorm_sRGB texture whose stand-in is not of its texels");
            standInFresh_.insert(t);
            continue;
        }
        const bool one = x.xrPlanes || (!x.chain && x.layers <= 1 && x.samples <= 1 && x.type == raw::fx::kTexTwoD);
        if (one) {
            if (!convert(x.standIn, x.xrPlanes ? x.luma : &own, x, x.width, x.height))
                return false;
            standInFresh_.insert(t);
            continue;
        }
        // a layered, mipmapped, 3D or multisampled one: a pass a pair of 2D images over the same texels (raw_xr_parts.h)
        std::vector<raw::XrPart> parts;
        if (!raw::xrParts(own, *x.standIn, parts))
            return fail(PredEncPassRender, "an extended-range texture whose stand-in is not of its texels");
        for (raw::XrPart &part : parts) {
            codeParts_.push_back(part);
            raw::XrPart &held = codeParts_.back();
            held.codes.tic = d_->attachmentTic(own, held.codes, held.level, held.key);
            held.standIn.tic = d_->attachmentTic(*x.standIn, held.standIn, held.level, held.key);
            if (!held.codes.tic || !held.standIn.tic)
                return fail(PredEncPassRender, "no header left for an extended-range texture's conversion");
            if (!convert(&held.standIn, &held.codes, x, held.codes.width, held.codes.height))
                return false;
        }
        standInFresh_.insert(t);
    }
    return true;
}

// textures a kernel reaches through their images (raw_kernel_shadow.h): filled as their compute encoder begins,
// copied back as it ends, sooner where another resource of a dispatch reaches the same bytes. A load the
// command buffer's native view of a decoder's plane answers (readTexture) reads in place.
void RMRangeEncode::localShadows(std::vector<raw::KernelUse> &list)
{
    kernelWindows_.name(list, [&](uint64_t bytes) {
        return rc_->transient(uint32_t(std::min<uint64_t>(bytes, UINT32_MAX)));
    });
    for (raw::KernelUse &u : list)
        if (u.shadowed && !u.writes && u.windowSlot < 0) {
            const raw::Texture &x = ((__bridge RMTexture *)u.object)->_raw;
            if (const raw::Texture *native = rc_->readTexture(&x); native != &x) {
                u.shadowed = false;
                u.object = native;  // another resource over its bytes: an image written before is copied back first
            }
        }
}

void RMRangeEncode::shadowCopies(const std::vector<const void *> &back, const std::vector<const void *> &fill)
{
    raw::BlitEncoder *copy = rc_->blit();
    for (const void *o : back) {
        if (const raw::KernelWindows::Window *w = kernelWindows_.find(o)) {
            rc_->use(((__bridge RMBuffer *)w->buffer)->_raw, true, false);
            rc_->useAddress(w->va, w->span.bytes, false);
            copy->copyBytes(w->va, w->span.va, w->span.bytes);
            continue;
        }
        const raw::Texture &x = ((__bridge RMTexture *)o)->_raw;
        copy->copyTexels(*x.sampled, 0, 0, 0, x, 0, 0, 0, x.width, x.height);
    }
    for (const void *o : fill) {
        if (const raw::KernelWindows::Window *w = kernelWindows_.find(o)) {
            useBuffer(rc_, (__bridge RMBuffer *)w->buffer, false);
            rc_->useAddress(w->va, w->span.bytes, true);
            copy->copyBytes(w->span.va, w->va, w->span.bytes);
            continue;
        }
        const raw::Texture &x = ((__bridge RMTexture *)o)->_raw;
        copy->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
    }
    copy->end();
}

void RMRangeEncode::beginKernelShadows(size_t encoderAt)
{
    encoderUses_.clear();
    for (size_t at = encoderAt; at < cb_->_plan.encoderEnd(encoderAt); ++at) {
        if (cb_->_plan.operation(at).op != OpDispatch ||
            !(cb_->_plan.dispatch(at).surfaceSlots | cb_->_plan.dispatch(at).windowSlots))
            continue;
        kernelUses(cb_, at, dispatchUses_);
        localShadows(dispatchUses_);
        encoderUses_.insert(encoderUses_.end(), dispatchUses_.begin(), dispatchUses_.end());
    }
    kernelShadows_.begin(encoderUses_.data(), encoderUses_.size());
    encoderShadows_ = !encoderUses_.empty();
    std::vector<const void *> fill;
    for (const raw::KernelUse &u : encoderUses_)
        if (u.shadowed && std::find(fill.begin(), fill.end(), u.object) == fill.end())
            fill.push_back(u.object);
    if (!fill.empty())
        shadowCopies({}, fill);
}

// the images written back, and every pool image of a texture a draw samples refreshed again before that draw: a
// kernel may have written its rows through another resource
void RMRangeEncode::endKernelShadows()
{
    const std::vector<const void *> back = kernelShadows_.end();
    if (!back.empty())
        shadowCopies(back, {});
    fresh_.clear();
    encoderShadows_ = false;
}

bool RMRangeEncode::command(size_t at)
{
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    const Cmd c = cb->_plan.legacy(at);
    if ((c.op == OpRender && !rmContinuesPass(cb, at)) || c.op == OpCompute || c.op == OpBlit) {
        encoder_ = at;
#if SWITCH_RAW_DIAGNOSTICS
        traceFreshUses(cb, at);
#endif
    }
    if (c.op == OpDispatch) {
        RAW_ENCODE_SPAN(Compute);
        const auto &dispatch = cb->_plan.dispatch(at);
        const auto *kernels = static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(dispatch.kernel);
        const auto &state = cb->_plan.kernelStates[dispatch.state];
        const auto &textures = cb->_plan.kernelTextures[state.textures].objects;
        dispatchUses_.clear();
        if ((dispatch.surfaceSlots | dispatch.windowSlots) && !encoderShadows_) {  // a range begun inside its encoder
            ce_->end();
            ce_.reset();
            beginKernelShadows(dispatch.encoderBegin);
            ce_.emplace(*rc, *d_);
            ce_->setSerial(serialCompute_);
            stateCursor_.reset();
        }
        if (encoderShadows_) {
            kernelUses(cb, at, dispatchUses_);
            localShadows(dispatchUses_);
        }
        if (const auto &steps = kernelShadows_.before(dispatchUses_.data(), dispatchUses_.size()); !steps.empty()) {
            ce_->end();
            ce_.reset();
            shadowCopies(steps.copyBack, steps.fill);
            ce_.emplace(*rc, *d_);
            ce_->setSerial(serialCompute_);
            stateCursor_.reset();
        }
        encodePlannedState(cb, at, *rc, re_, &*ce_, stateCursor_);
        ce_->clearBufferWindows();
        for (const raw::KernelUse &u : dispatchUses_)
            if (u.windowSlot >= 0)
                ce_->setBufferWindow(uint32_t(u.windowSlot), kernelWindows_.find(u.object)->va);
        const auto &regions = dispatch.regions;
        ce_->setStageInRegion(dispatch.stageIn);
        ce_->setStageInRegionIndirect(dispatch.stageInBuffer ? &((__bridge RMBuffer *)dispatch.stageInBuffer)->_raw
                                                             : nullptr,
                                      dispatch.stageInOffset);
        if (!kernels || kernels->count != regions.count)
            return fail(PredCompute, "a dispatch with an incomplete region plan", c.op);
        if (dispatch.arguments) {
            ce_->setState(kernels->jobs[0]->value());
            if (const char *w = nullptr; !ce_->dispatchIndirect(&((__bridge RMBuffer *)dispatch.arguments)->_raw,
                                                                dispatch.argumentsOffset, regions.nominal, &w))
                return fail(PredCompute, w, c.op);
        }
        for (uint8_t region = 0; region < regions.count && !dispatch.arguments; ++region) {
            ce_->setState(kernels->jobs[region]->value());
            if (const char *w = nullptr; !ce_->dispatchRegion(regions.regions[region], regions.grid, regions.nominal,
                                                              !region, region + 1 == regions.count, &w))
                return fail(PredCompute, w, c.op);
        }
        kernelShadows_.after(dispatchUses_.data(), dispatchUses_.size());
        if (standIns_ && !standInFresh_.empty())
            for (uint8_t region = 0; region < kernels->count; ++region) {
                const auto &record = *kernels->jobs[region]->value()->rec;
                for (uint32_t i = 0; i < record.entries; ++i)
                    if (record.entry[i].kind == raw::KernelTexture && record.entry[i].slot < 31 &&
                        raw::kernelEntryWrites(record, i))
                        standInFresh_.erase((__bridge RMTexture *)textures[record.entry[i].slot]);
            }
        return true;
    }
    {
        RAW_ENCODE_SPAN(State);
        encodePlannedState(cb, at, *rc, re_, ce_ ? &*ce_ : nullptr, stateCursor_);
    }
    if (isKernelCommand(c.op)) {
        RAW_ENCODE_SPAN(Compute);
        if (const char *w = nullptr; !encodeKernel(*ce_, *rc, cb, c, at, &w))
            return fail(PredCompute, w, c.op);
        return true;
    }
    switch (c.op) {
    case OpCompute:
        if (!computeUnit_ || serialCompute_ != (c.a == MTLDispatchTypeSerial))
            rc->endPredicationUnit();
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
        if (!re_->setVisibility(
                c.a,
                [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr,
                c.c, c.b != 0)) {
            const char *taken = "an offset set twice in an encoder or past maxVisibilityQueryOffset, or no report page";
            if (!ahead_)
                visibilityError(cb, c.a, c.c, taken);
            return fail(PredCallVisibility, taken, c.a, c.c);
        }
        break;
    case OpDrawIndexed: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
        useBuffer(rc, indices, false);
        if (!re_->drawIndexed(draw.primitive, draw.count, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4, &indices->_raw,
                              draw.indexOffset, draw.instances, draw.baseVertex, draw.baseInstance, why_))
            return refused();
        break;
    }
    case OpDrawIndirect:
    case OpDrawIndexedIndirect: {
        const auto &draw = cb->_plan.draw(at);
        __unsafe_unretained RMBuffer *arguments = (__bridge RMBuffer *)draw.arguments;
        __unsafe_unretained RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
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
        RAW_ENCODE_SPAN(Blit);
        if (!encodeMipmaps(rc, (__bridge RMTexture *)c.obj, uint32_t(c.c), be_, why_))
            return refused();
        break;
    }
    case OpFillTexture: {
        RAW_ENCODE_SPAN(Blit);
        be_->end();
        if (!fillTexturePass(cb, c, rc))
            return fail(PredBlitTexFill, "a texture fill into a target the raw path cannot render",
                        ((__bridge RMTexture *)c.obj)->_descriptor.pixelFormat);
        be_ = rc->blit();
        break;
    }
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpFillBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer: {
        RAW_ENCODE_SPAN(Blit);
        encodeBlit(cb, c, rc, be_, fresh_, standInFresh_);
        break;
    }
    case OpSignalEvent:
        if (!isInlineSignal(cb, at))
            break;
        computeUnit_ = false;
        rc->endPredicationUnit();
        if (!releaseInlineSignal(cb, at, rc)) {
            encoder_ = SIZE_MAX;
            const char *lost =
                "a signal whose shared event the kernel refused to import (EventImport): the command buffer fails";
            if (!ahead_)
                eventError(cb, true, lost);
            return fail(PredEvent, lost);
        }
        break;
    default:  // states, fences, events, resources the raw path needs nothing for
        break;
    }
    return true;
}
