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

// textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
// copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
// its shadow at each encoder that uses it once written since)
void RMRangeEncode::refresh(size_t pass)
{
    cb_->_plan.forEachResource(pass, cb_->_plan.encoderEnd(pass), RMResourceKind::Texture,
                               [&](const RMRangeResource &use) {
                                   if (!(use.access & raw::ResourceUse::Sample))
                                       return;
                                   __unsafe_unretained RMTexture *t = (__bridge RMTexture *)use.object;
                                   if (rc_->readTexture(&t->_raw) != &t->_raw || !t->_raw.sampled ||
                                       (!t->_raw.surfaceView && !fresh_.insert(t).second))
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
                                   if (texture->_raw.standIn && !standInFresh_.count(texture) &&
                                       (!texture->_raw.xrPlanes || cb_->_xrStandIns.count(texture)))
                                       stale.push_back(texture);
                               });
    for (__unsafe_unretained RMTexture *t : stale) {
        raw::Texture &x = t->_raw;
        raw::PassDesc p;
        p.color[0].texture = x.standIn;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *conv = rc_->render(p);
        if (!conv)
            return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
        codes_.push_back(x);
        codes_.back().standIn = nullptr;
        useTexture(rc_, t, false);
        conv->setPipeline(d_->xrPipeline(raw::xrForm(x.format), x.xrPlanes));
        conv->setTexture(0, x.xrPlanes ? x.luma : &codes_.back());
        if (x.xrPlanes)
            conv->setTexture(1, x.chroma);
        conv->setViewport(0, 0, x.width, x.height, 0, 1);
        conv->setScissor(0, 0, x.width, x.height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        conv->end();
        standInFresh_.insert(t);
    }
    return true;
}

// textures a kernel reaches through their images (raw_kernel_shadow.h): filled as their compute encoder begins,
// copied back as it ends, sooner where another resource of a dispatch reaches the same bytes. A load the
// command buffer's native view of a decoder's plane answers (readTexture) reads in place.
void RMRangeEncode::localShadows(std::vector<raw::KernelUse> &list)
{
    for (raw::KernelUse &u : list)
        if (u.shadowed && !u.writes) {
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
        const raw::Texture &x = ((__bridge RMTexture *)o)->_raw;
        copy->copyTexels(*x.sampled, 0, 0, 0, x, 0, 0, 0, x.width, x.height);
    }
    for (const void *o : fill) {
        const raw::Texture &x = ((__bridge RMTexture *)o)->_raw;
        copy->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
    }
    copy->end();
}

void RMRangeEncode::beginKernelShadows(size_t encoderAt)
{
    encoderUses_.clear();
    for (size_t at = encoderAt; at < cb_->_plan.encoderEnd(encoderAt); ++at) {
        if (cb_->_plan.operation(at).op != OpDispatch || !cb_->_plan.dispatch(at).surfaceSlots)
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
        const auto &textures = cb->_plan.textures[2][state.textures].objects;
        dispatchUses_.clear();
        if (dispatch.surfaceSlots && !encoderShadows_) {  // a range that begins inside its encoder
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
        const auto &regions = dispatch.regions;
        ce_->setStageInRegion(dispatch.stageIn);
        ce_->setStageInRegionIndirect(dispatch.stageInBuffer ? &((__bridge RMBuffer *)dispatch.stageInBuffer)->_raw
                                                             : nullptr,
                                      dispatch.stageInOffset);
        if (!kernels || kernels->count != regions.count)
            return fail(PredCompute, "a dispatch with an incomplete region plan", c.op);
        if (dispatch.arguments) {
            ce_->setState(kernels->jobs[0]->value());
            if (const char *w = nullptr;
                !ce_->dispatchIndirect(&((__bridge RMBuffer *)dispatch.arguments)->_raw, dispatch.argumentsOffset, &w))
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
                c.c))
            return fail(PredCallVisibility, "a visibility offset the raw path cannot take", c.a, c.c);
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
        encodeCommandsBlit(be_, c);
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
            return fail(PredEvent, "a signal inside a raw range whose event the kernel no longer holds");
        }
        break;
    default:  // states, fences, events, resources the raw path needs nothing for
        break;
    }
    return true;
}
