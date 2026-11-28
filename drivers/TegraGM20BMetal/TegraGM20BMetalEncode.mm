// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalRangeEncode.h"
#import "TegraGM20BMetalPipeline.h"
#include "lock.h"

// ---- the tape onto the driver ----

SwitchMetalRangeEncode &gm20bRangeEncode()
{
    static SwitchMetalRangeEncode *encode = new SwitchMetalRangeEncode;
    return *encode;
}

SwitchMetalPred refusalPred(uint8_t id)
{
    static const SwitchMetalPred map[] = {PredEncPassRender, PredEncIndexedRange, PredEncColorFormats,
                                          PredEncNoTic,      PredEncDepthStencil, PredEncSamples,
                                          PredEncGeometry,   PredEncStorageView,  PredPassRotated,
                                          PredEncPassRender, PredEncPassRender};
    static_assert(sizeof map / sizeof *map == gm20b::RefWrittenTexture + 1, "a predicate for each refusal");
    return id < sizeof map / sizeof *map ? map[id] : PredEncPassRender;
}

// Encoded ahead, a failure is not the command buffer's yet: its encode at the commit meets it again and counts it. A draw
// that refused under another step (a turned pass's reach) still fails as Metal's error.
bool SwitchMetalRangeEncode::fail(SwitchMetalPred p, const char *w, uint64_t a, uint64_t b, const char *detail)
{
    *why_ = w;
    if (ahead_)
        veto_ = VetoFailed;
    else {
        nameFailure(cb_, p);
        if (!rc_->refusal.id || !drawFailed(cb_, rc_->refusal, w))
            hit(cb_, p, a, b, detail);
    }
    rc_->abandon();
    abandoned_ = true;
    return false;
}

bool SwitchMetalRangeEncode::stop(uint32_t veto)
{
    veto_ = veto;
    return false;
}

// A pass gm20b::CommandBuffer::render refused: Metal's validation or out of memory, named in the predicate's first line
bool SwitchMetalRangeEncode::passRefused(uint64_t a)
{
    if (!ahead_ && !predLogged(PredEncPassRender))
        logPred(PredEncPassRender, a, 0, @(*why_));
    return fail(PredEncPassRender, *why_, a);
}

// A draw the encoder refused: Metal's error, or its predicate and values (draw_refusal.h Refusal)
bool SwitchMetalRangeEncode::refused()
{
    const gm20b::Refusal &f = rc_->refusal;
    if (ahead_)
        veto_ = VetoFailed;
    else {
        nameFailure(cb_, refusalPred(f.id));
        if (!drawFailed(cb_, f, *why_))
            hit(cb_, refusalPred(f.id), f.a, uint64_t(f.b) | uint64_t(f.c) << 32, *why_);
    }
    rc_->abandon();
    abandoned_ = true;
    return false;
}

__attribute__((objc_externally_retained)) bool SwitchMetalRangeEncode::open(SwitchMetalCommandBuffer *cb, size_t from,
                                                                            size_t to, bool whole, const char **why)
{
    cb_ = cb;
    d_ = gRM->raw;
    why_ = why;
    from_ = at_ = from;
    to_ = to;
    whole_ = whole;
    ahead_ = !whole;
    abandoned_ = false;
    veto_ = 0;
    namedBytesChunk_ = UINT32_MAX;
    sampledShadows_ = whole && cb->_sampledShadows;
    standIns_ = whole && cb->_standIns;
    gm20b::Device &d = *d_;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (whole)
        d.nextForms = cb->_probeForms;
#endif
    rc_ = d.begin();
    if (!rc_) {
        *why = "the kernel gave the driver no channel";
        if (ahead_)
            return stop(VetoChannel);
        hit(cb, PredEncNoChannel, 0, 0, d.channelError().c_str());
        return false;
    }
    generation_ = d.channelGeneration();
    rc_->internalCompletion();
    beginInlineSignals(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    rc_->forceWholePrograms(cb->_forceWholePrograms);
    rc_->inlineStates(cb->_inlineStates);
#endif
    if (whole && !producersOrdered())
        return false;
    // before any write the encode makes (first contents, conversions), which must mark a carried region stale
    if (whole && from)
        rc_->resumeZcull((__bridge const void *)cb, from);
    if (whole)
        firstContents_.emplace(cb, from, to, *rc_);
    else
        firstContents_.emplace();
    SWITCH_METAL_ENCODE_BEGIN(resourceSpan, Resources);
    if (whole) {
        resources(from, to);
        counters_.emplace(cb, from, to, *rc_);
    } else {
        counters_.emplace(*rc_);
    }
    // A range that starts inside a compute encoder (a loop's continuation, or what follows a block that holds loops:
    // SwitchMetalSegment::held): that encoder's work goes on in a new raw encoder, the planned state applied at its first dispatch
    for (size_t at = from; at-- > 0;) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpCompute) {
            ce_.emplace(*rc_, d);
            serialCompute_ = c.a == MTLDispatchTypeSerial;
            ce_->setSerial(serialCompute_);
            computeUnit_ = true;
        }
        if (c.op == OpCompute || c.op == OpRender || c.op == OpBlit || c.op == OpEnd)
            break;
    }
    if (whole && cb->_borderSamplers && !borderHeaders(from, to))
        return false;
    if (whole && !conversions())
        return false;
    SWITCH_METAL_ENCODE_END(resourceSpan);
    return true;
}

// The resources [from, to) names: their pitch imports, their CPU lines cleaned, each named to the job
bool SwitchMetalRangeEncode::resources(size_t from, size_t to)
{
    gm20b::CommandBuffer *rc = rc_;
    rc->namingResources(true);
    const auto buffers = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, SwitchMetalResourceKind::Buffer, visit);
    };
    const auto textures = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, SwitchMetalResourceKind::Texture, visit);
    };
    const auto pitchBuffer = [&](const SwitchMetalRangeResource &use) {
        if (__unsafe_unretained SwitchMetalBuffer *buffer = (__bridge SwitchMetalBuffer *)use.facts.cpuBuffer)
            rc->requirePitch(buffer->_raw.import, use.access & gm20b::ResourceUse::Write);
    };
    buffers(pitchBuffer);
    bool hasNativePlaneWrite = false;
    textures([&](const SwitchMetalRangeResource &use) {
        pitchBuffer(use);
        __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
        const bool writes = use.access & gm20b::ResourceUse::Write;
        const bool eligibleNativeWrite =
            writes && !(use.access & gm20b::ResourceUse::Explicit) && !(use.access & gm20b::ResourceUse::Sample) &&
            texture->_raw.nativePlane != ~0u && texture->_raw.import && !texture->_raw.planar && !texture->_raw.ycbcr &&
            !texture->_raw.standIn && !texture->_raw.rotation && !texture->_raw.luma && !texture->_raw.chroma &&
            texture->_raw.type == gm20b::fx::kTexTwoD;
        if (eligibleNativeWrite) {
            hasNativePlaneWrite = true;
            return;
        }
        if (writes || (use.access & gm20b::ResourceUse::Explicit) || !(use.access & gm20b::ResourceUse::Sample)) {
            rc->requirePitch(texture->_raw.import, writes);
            for (const gm20b::Texture *plane : {texture->_raw.luma, texture->_raw.chroma})
                if (plane)
                    rc->requirePitch(plane->import, writes);
        }
    });
    if (hasNativePlaneWrite) {
        for (size_t at = from; at < to; ++at) {
            const Cmd c = cb_->_plan.legacy(at);
            if (c.op == OpBlit || c.op == OpCompute) {
                const size_t end = std::min(cb_->_plan.encoderEnd(at), to);
                cb_->_plan.forEachResource(
                    at, end, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
                        __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
                        if (texture->_raw.nativePlane != ~0u)
                            rc->requirePitch(texture->_raw.import, use.access & gm20b::ResourceUse::Write);
                    });
            } else if (c.op == OpRender) {
                const SwitchMetalPass &pass = cb_->_passes[c.x];
                for (const SwitchMetalPass::Color &a : pass.color) {
                    if (__unsafe_unretained SwitchMetalTexture *t = (SwitchMetalTexture *)a.texture;
                        t && t->_raw.nativePlane != ~0u &&
                        (a.level || a.slice || pass.layers > 1 || a.resolve || t->_raw.planarStandIn ||
                         t->_raw.planar || t->_raw.ycbcr || t->_raw.standIn || t->_raw.rotation))
                        rc->requirePitch(t->_raw.import, true);
                    if (__unsafe_unretained SwitchMetalTexture *r = (SwitchMetalTexture *)a.resolve;
                        r && r->_raw.nativePlane != ~0u)
                        rc->requirePitch(r->_raw.import, true);
                }
            }
        }
        textures([&](const SwitchMetalRangeResource &use) {
            if (!(use.access & gm20b::ResourceUse::Write))
                return;
            __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
            if (texture->_raw.nativePlane != ~0u && rc->writeTexture(&texture->_raw) == &texture->_raw)
                rc->requirePitch(texture->_raw.import, true);
        });
    }
    textures([&](const SwitchMetalRangeResource &use) {
        if (!(use.access & gm20b::ResourceUse::Sample))
            return;
        __unsafe_unretained SwitchMetalTexture *texture = (__bridge SwitchMetalTexture *)use.object;
        for (const gm20b::Texture *plane : {&texture->_raw, texture->_raw.luma, texture->_raw.chroma})
            if (plane && plane->nativePlane != ~0u && rc->readTexture(plane) == plane)
                rc->requirePitch(plane->import, false);
    });
    const auto cleanBuffer = [&](const SwitchMetalRangeResource &use) {
        const bool write = use.access & gm20b::ResourceUse::Write;
        if (__unsafe_unretained SwitchMetalBuffer *buffer = (__bridge SwitchMetalBuffer *)use.facts.cpuBuffer;
            buffer && buffer->_raw.hostVisible && !buffer->_raw.wc &&
            (write || (use.access & gm20b::ResourceUse::Explicit)))
            rc->clean(buffer->_raw.cpu, buffer->_raw.length, write);
    };
    buffers([&](const SwitchMetalRangeResource &use) {
        cleanBuffer(use);
        useBuffer(rc, (__bridge SwitchMetalBuffer *)use.object, use.access & gm20b::ResourceUse::Write);
    });
    textures([&](const SwitchMetalRangeResource &use) {
        cleanBuffer(use);
        useTexture(rc, (__bridge SwitchMetalTexture *)use.object, use.access & gm20b::ResourceUse::Write);
    });
    cb_->_plan.forEachResource(from, to, SwitchMetalResourceKind::Heap, [&](const SwitchMetalRangeResource &use) {
        if (__unsafe_unretained SwitchMetalHeap *heap = (__bridge SwitchMetalHeap *)use.object; heap->_members)
            rc->useHeap(*heap->_members);
    });
    rc->namingResources(false);
    return true;
}

// opaque borders' headers for the textures [from, to) samples (Sampler::bordered), which the draws' handles take. With no
// entry left in the sampler header table the command buffer fails OutOfMemory; encoded ahead, that is the commit's encode's
// to meet (fail's veto).
bool SwitchMetalRangeEncode::borderHeaders(size_t from, size_t to)
{
    bordered_.clear();
    cb_->_plan.forEachResource(from, to, SwitchMetalResourceKind::Sampler, [&](const SwitchMetalRangeResource &use) {
        if (SwitchMetalSampler *s = (__bridge SwitchMetalSampler *)use.object; s->_raw.desc.border)
            bordered_.push_back(s);
    });
    if (bordered_.empty())
        return true;
    bool made = true;
    cb_->_plan.forEachResource(from, to, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
        if (use.access & gm20b::ResourceUse::Sample)
            for (__unsafe_unretained SwitchMetalSampler *s : bordered_)
                made &= d_->borderVariant(s->_raw, ((__bridge SwitchMetalTexture *)use.object)->_raw);
    });
    if (!made) {
        encoder_ = SIZE_MAX;
        if (ahead_)
            return fail(PredCallSamplerForeign, "no sampler header for an opaque border");
        hit(cb_, PredCallSamplerForeign, 0, 0, "no sampler header left in the device's table for an opaque border");
        failCommandBuffer(cb_, @"no sampler header left in the device's table for an opaque border",
                          MTLCommandBufferErrorOutOfMemory, PredCallSamplerForeign);
        rc_->abandon();
        abandoned_ = true;
        return false;
    }
    return true;
}

// What an encode ahead cannot take from [from, until): work it is not built for, or what only the commit makes final;
// `stopAt`: the first command of the encoder it is in, or the command itself between encoders (an event)
uint32_t SwitchMetalRangeEncode::ineligible(size_t from, size_t until, size_t &stopAt)
{
    const SwitchMetalCommandPlan &plan = cb_->_plan;
    size_t begin = from;
    bool inside = false;
    for (size_t at = from; at < until; ++at) {
        const Op op = plan.operation(at).op;
        const bool begins = SwitchMetalCommandPlan::isEncoderBegin(op);
        if (begins) {
            begin = at;
            inside = true;
        }
        stopAt = inside ? begin : at;
        if (op == OpSignalEvent || op == OpWaitEvent || op == OpCompute || op == OpDispatch || isKernelCommand(op) ||
            op == OpExecuteCommands || op == OpDrawPatches || op == OpDrawPatchesIndirect || op == OpPredicate ||
            op == OpResetCommands || op == OpCopyCommands)
            return VetoOps;
        if (op == OpEnd)
            inside = false;
        if (!begins)
            continue;
        const size_t end = plan.encoderEnd(at);
        // its images are converted from their surfaces at the seeds the commit makes final (conversions)
        bool planar = false;
        plan.forEachResource(at, end, SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
            planar |=
                (use.access & gm20b::ResourceUse::Sample) && (use.facts.roles & SwitchMetalResourceFacts::Converted);
        });
        if (planar)
            return VetoPlanar;
    }
    return 0;
}

// Encoded ahead, the encoder at `begin` is entered: its resources named, its opaque borders' sampler headers made, the
// fresh textures it uses first written ahead of it (where the end of the encoder before it did not). A surface it writes
// that an encoder before it read through the native image stops it: the whole range's encode names the pitch view to every
// encoder.
bool SwitchMetalRangeEncode::enter(size_t begin)
{
    const size_t end = cb_->_plan.encoderEnd(begin);
    resources(begin, end);
    if (rc_->pitchAfterNativeRead()) {
        rc_->abandon();
        abandoned_ = true;
        return stop(VetoNative);
    }
    if (!borderHeaders(begin, end))
        return false;
    if (freshAt_ != begin) {
        freshAt_ = begin;
        if (firstContents_->claim(cb_, begin, end))
            firstContents_->written(*rc_);
    }
    return true;
}

bool SwitchMetalRangeEncode::step(size_t until)
{
    if (!whole_) {
        until = std::min(until, to_);
        // the encoder beginning at `until` too: the end before it is encoded now and looks into it
        size_t scan = until;
        if (ahead_ && until < to_ && until < ended_ &&
            SwitchMetalCommandPlan::isEncoderBegin(cb_->_plan.operation(until).op))
            scan = std::min(cb_->_plan.encoderEnd(until), ended_);
        size_t stopAt = 0;
        if (const uint32_t veto = ineligible(at_, scan, stopAt)) {
            const bool clean = stopAt > at_ || (stopAt == at_ && betweenEncoders());
            if (!ahead_ || !clean) {
                rc_->abandon();
                abandoned_ = true;
                return stop(veto);
            }
            halt_ = to_ = stopAt;
            until = std::min(until, stopAt);
            cb_->_aheadStopped.fetch_or(veto, std::memory_order_relaxed);
        }
    }
    SWITCH_METAL_ENCODE_BEGIN(loopSpan, Loop);
    for (; at_ < until; ++at_) {
        if (!whole_ && SwitchMetalCommandPlan::isEncoderBegin(cb_->_plan.operation(at_).op) && !enter(at_))
            return false;
        if (!command(at_))
            return false;
        if (!rewritten_.empty() && !continuing_ && cb_->_plan.operation(at_).op == OpEnd && !remakeImages(at_ + 1))
            return false;
    }
    SWITCH_METAL_ENCODE_END(loopSpan);
    return true;
}

// The range's GPU event waits are on producers this channel orders, or that succeeded. False: one failed (the command
// buffer's error), or one is pending on a channel made again since (producerPending())
bool SwitchMetalRangeEncode::producersOrdered()
{
    const gm20b::EventDependencyState producers = validateNativeEventWaits(cb_, generation_);
    if (producers == gm20b::EventDependencyState::ready)
        return true;
    rc_->abandon();
    abandoned_ = true;
    producerPending_ = producers == gm20b::EventDependencyState::blocked;
    *why_ = "an event producer failed or is pending on a channel made again since";
    return false;
}

bool SwitchMetalRangeEncode::resume(size_t to, const char **why)
{
    why_ = why;
    to_ = to;
    ahead_ = false;
    sampledShadows_ = cb_->_sampledShadows;
    standIns_ = cb_->_standIns;
    if (!producersOrdered())
        return false;
    // a surface it read through its native view before the commit, published or written since: the whole encode reads it
    // again
    if (!rc_->nativeReadsCurrent()) {
        rc_->abandon();
        abandoned_ = true;
        return stop(VetoNative);
    }
    return true;
}

void SwitchMetalRangeEncode::reset()
{
    if (rc_ && !abandoned_ && !sealed_)
        rc_->abandon();
    sealed_ = abandoned_ = ahead_ = producerPending_ = false;
    veto_ = 0;
    ended_ = halt_ = 0;
    firstContents_.reset();
    counters_.reset();
    ce_.reset();
    re_ = nullptr;
    be_ = nullptr;
    rc_ = nullptr;
    cb_ = nil;
    serialCompute_ = computeUnit_ = continuing_ = encoderShadows_ = encoderTracked_ = false;
    kernelShadows_ = {};
    stateCursor_ = {};
    namedBytesChunk_ = UINT32_MAX;
    fused_ = {};
    fusedAt_ = renderBegin_ = encoder_ = freshAt_ = SIZE_MAX;
    planned_.reset();
    standInLoads_.clear();
    planesWritten_.clear();
    overlapStores_.clear();
    overlapLoaded_.clear();
    standInStores_.clear();
    backings_ = {};
    planeLoads_.clear();
    loadedReach_ = {};
    loadedTargets_ = 0;
    formatTargets_.clear();
    formatStores_.clear();
    planeImages_.clear();
    resolves_.clear();
    resolveTargets_.clear();
    turned_ = {};
    depthResolve_ = {};
    stencilResolve_ = {};
    converted_.clear();
    rewritten_.clear();
    fresh_.clear();
    for (gm20b::Texture &t : levelTargets_)
        t = {};
    zetaPart_ = {};
    separatePart_[0] = separatePart_[1] = {};
    separateResolved_[0] = separateResolved_[1] = nullptr;
    depth16Scratch_ = nullptr;
    shadowsWritten_.clear();
    standInsWritten_.clear();
    planesImaged_.clear();
    morePlanes_.clear();
    standInFresh_.clear();
    codes_.clear();
    codeParts_.clear();
#if SWITCH_METAL_DIAGNOSTICS
    levelPasses_ = 0;
#endif
}

SwitchMetalEncoded encodeRaw(SwitchMetalCommandBuffer *cb, size_t from, size_t to, const char **why)
{
    SwitchMetalRangeEncode &encode = gm20bRangeEncode();
    // what was encoded ahead of the commit goes on: the rest of the range, then the seal; one that stopped submitted
    // nothing, and the range is encoded whole
    if (gm20bPipelineTake(cb, from)) {
        const bool encoded = encode.resume(to, why) && encode.step(to) && encode.seal(to);
        const uint32_t veto = !encoded ? encode.vetoed() : 0;
        const bool waits = encode.producerPending();
        encode.reset();
        if (encoded) {
            gm20bPipelineSealed(cb, to);
            return SwitchMetalEncoded::submitted;
        }
        cb->_released.clear();
        gm20bPipelineAbandoned(cb, veto);
        if (!veto)
            return waits ? SwitchMetalEncoded::producerPending : SwitchMetalEncoded::stopped;
    }
    const bool encoded = encode.open(cb, from, to, true, why) && encode.step(to) && encode.seal(to);
    const bool waits = encode.producerPending();
    encode.reset();
    if (!encoded) {
        cb->_released.clear();
        return waits ? SwitchMetalEncoded::producerPending : SwitchMetalEncoded::stopped;
    }
    gm20bPipelineSealed(cb, to);
    return SwitchMetalEncoded::submitted;
}
