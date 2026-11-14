// SPDX-License-Identifier: MIT
#import "RawMetalRangeEncode.h"
#import "RawMetalPipeline.h"
#include "raw_lock.h"

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then. A refusal inside one
// encoder names that encoder's first command in *refusedEncoder; one no encoder owns (the channel's, the kernel's, a planar
// texture sampled and rendered across the range) leaves it alone.

RMRangeEncode &rmRangeEncode()
{
    static RMRangeEncode *encode = new RMRangeEncode;
    return *encode;
}

// Encoded ahead, a failure is not the command buffer's yet: its encode at the commit meets it again and counts it
bool RMRangeEncode::fail(RMPred p, const char *w, uint64_t a, uint64_t b)
{
    *why_ = w;
    if (ahead_)
        veto_ = VetoFailed;
    else
        hit(cb_, p, a, b);
    if (encoder_ != SIZE_MAX)
        *refusedEncoder_ = encoder_;
    rc_->abandon();
    abandoned_ = true;
    return false;
}

bool RMRangeEncode::stop(uint32_t veto)
{
    veto_ = veto;
    return false;
}

// a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
bool RMRangeEncode::refused()
{
    static const RMPred map[] = {PredEncPassRender,  PredEncIndexedRange, PredEncColorFormats, PredEncUboOffset,
                                 PredEncNoTic,       PredEncDepthStencil, PredEncSamples,      PredEncGeometry,
                                 PredEncStorageView, PredPassRotated};
    const raw::Refusal &f = rc_->refusal;
    if (rc_->contentsDeferred)
        veto_ = VetoContents;
    else if (ahead_)
        veto_ = VetoFailed;
    else
        hit(cb_, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
    if (encoder_ != SIZE_MAX)
        *refusedEncoder_ = encoder_;
    rc_->abandon();
    abandoned_ = true;
    return false;
}

__attribute__((objc_externally_retained)) bool RMRangeEncode::open(RMCommandBuffer *cb, size_t from, size_t to,
                                                                   bool whole, const char **why, size_t *refusedEncoder)
{
    const Encoding encoding;
    cb_ = cb;
    d_ = gRM->raw;
    why_ = why;
    refusedEncoder_ = refusedEncoder;
    from_ = at_ = from;
    to_ = to;
    whole_ = whole;
    ahead_ = !whole;
    abandoned_ = false;
    veto_ = 0;
    sampledShadows_ = whole && cb->_sampledShadows;
    standIns_ = whole && cb->_standIns;
    raw::Device &d = *d_;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (whole)
        d.nextForms = cb->_probeForms;
#endif
    rc_ = d.begin();
    if (!rc_) {
        *why = "the kernel gave the raw path no channel";
        if (!predLogged(PredEncNoChannel))
            logPred(PredEncNoChannel, 0, 0, @(d.channelError().c_str()));
        hit(cb, PredEncNoChannel);
        return false;
    }
    generation_ = d.channelGeneration();
    rc_->internalCompletion();
    beginInlineSignals(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    rc_->forceWholePrograms(cb->_forceWholePrograms);
    rc_->inlineStates(cb->_inlineStates);
#endif
    rc_->deferContents = ahead_;
    if (whole && validateNativeEventWaits(cb, generation_) != raw::EventDependencyState::ready) {
        rc_->abandon();
        abandoned_ = true;
        *why = "an event producer failed or is pending on a channel made again since";
        return false;
    }
    if (whole)
        firstContents_.emplace(cb, from, to, *rc_);
    else
        firstContents_.emplace();
    RAW_ENCODE_BEGIN(resourceSpan, Resources);
    if (whole) {
        resources(from, to);
        counters_.emplace(cb, from, to, *rc_);
    } else {
        counters_.emplace(*rc_);
    }
    // A range that starts inside a compute encoder (a loop's continuation, or what follows a block that holds loops:
    // RMSegment::held): that encoder's work goes on in a new raw encoder, the planned state applied at its first dispatch
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
    RAW_ENCODE_END(resourceSpan);
    return true;
}

// The resources [from, to) names: their pitch imports, their CPU lines cleaned, each named to the job
bool RMRangeEncode::resources(size_t from, size_t to)
{
    raw::CommandBuffer *rc = rc_;
    const auto buffers = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, RMResourceKind::Buffer, visit);
    };
    const auto textures = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, RMResourceKind::Texture, visit);
    };
    const auto pitchBuffer = [&](const RMRangeResource &use) {
        if (__unsafe_unretained RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer)
            rc->requirePitch(buffer->_raw.import);
    };
    buffers(pitchBuffer);
    textures([&](const RMRangeResource &use) {
        pitchBuffer(use);
        __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
        if ((use.access & (raw::ResourceUse::Write | raw::ResourceUse::Explicit)) ||
            !(use.access & raw::ResourceUse::Sample)) {
            rc->requirePitch(texture->_raw.import);
            for (const raw::Texture *plane : {texture->_raw.luma, texture->_raw.chroma})
                if (plane)
                    rc->requirePitch(plane->import);
        }
    });
    textures([&](const RMRangeResource &use) {
        if (!(use.access & raw::ResourceUse::Sample))
            return;
        __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
        for (const raw::Texture *plane : {&texture->_raw, texture->_raw.luma, texture->_raw.chroma})
            if (plane && plane->nativePlane != ~0u && rc->readTexture(plane) == plane)
                rc->requirePitch(plane->import);
    });
    const auto cleanBuffer = [&](const RMRangeResource &use) {
        const bool write = use.access & raw::ResourceUse::Write;
        if (__unsafe_unretained RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer;
            buffer && buffer->_raw.hostVisible && !buffer->_raw.wc &&
            (write || (use.access & raw::ResourceUse::Explicit)))
            rc->clean(buffer->_raw.cpu, buffer->_raw.length, write);
    };
    buffers([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useBuffer(rc, (__bridge RMBuffer *)use.object, use.access & raw::ResourceUse::Write);
    });
    textures([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useTexture(rc, (__bridge RMTexture *)use.object, use.access & raw::ResourceUse::Write);
    });
    cb_->_plan.forEachResource(from, to, RMResourceKind::Heap, [&](const RMRangeResource &use) {
        if (__unsafe_unretained RMHeap *heap = (__bridge RMHeap *)use.object; heap->_members)
            rc->useHeap(*heap->_members);
    });
    return true;
}

// opaque borders' headers for the textures [from, to) samples (Sampler::bordered), which the draws' handles take
bool RMRangeEncode::borderHeaders(size_t from, size_t to)
{
    bordered_.clear();
    cb_->_plan.forEachResource(from, to, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
        if (RMSampler *s = (__bridge RMSampler *)use.object; s->_raw.desc.border)
            bordered_.push_back(s);
    });
    if (bordered_.empty())
        return true;
    bool made = true;
    cb_->_plan.forEachResource(from, to, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        if (use.access & raw::ResourceUse::Sample)
            for (__unsafe_unretained RMSampler *s : bordered_)
                made &= d_->borderVariant(s->_raw, ((__bridge RMTexture *)use.object)->_raw);
    });
    if (!made) {
        encoder_ = SIZE_MAX;
        return fail(PredCallSamplerForeign, "no sampler header for an opaque border");
    }
    return true;
}

// the planar textures its draws sample: their images from their planes first, once (m2v converts at each encoder that uses
// one; nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's
// full barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
// image is made when a draw samples it. Its samplers read it under its address map (Texture::addressMap): a sampler state
// through its header made here, a constexpr sampler through the one placed with it (Device::constexprMapped_).
bool RMRangeEncode::conversions()
{
    raw::Device &d = *d_;
    raw::CommandBuffer *rc = rc_;
    RMCommandBuffer *cb = cb_;
    std::vector<__unsafe_unretained RMTexture *> planar;
    std::vector<__unsafe_unretained RMSampler *> samplers;
    cb->_plan.forEachResource(from_, to_, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        if ((use.access & raw::ResourceUse::Sample) && (use.facts.roles & RMResourceFacts::Converted))
            planar.push_back((__bridge RMTexture *)use.object);
    });
    if (planar.empty())
        return true;
    cb->_plan.forEachResource(from_, to_, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
        samplers.push_back((__bridge RMSampler *)use.object);
    });
    // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
    if (!d.constexprMapsPlaced())
        return fail(PredEncPlanarClamp, "no constexpr sampler header for an address map");
    for (__unsafe_unretained RMTexture *t : planar)
        for (__unsafe_unretained RMSampler *sm : samplers)
            if (t->_raw.addressMap && !d.samplerVariant(sm->_raw, t->_raw.addressMap))
                return fail(PredEncPlanarClamp, "no sampler header for an address map");
    // an image converted already from its surface at the surface's seed now is not converted again: the work that
    // converted it went before on this channel (the decoder and ImageIO write a surface under a write lock, which
    // moves its seed). The new seeds take effect once this command buffer is submitted.
    std::vector<__unsafe_unretained RMTexture *> stale;
    for (__unsafe_unretained RMTexture *t : planar) {
        const uint32_t seed = t->_surface ? IOSurfaceGetSeed(t->_surface) : 0;
        const uint64_t writes = t->_surface ? surfaceWrites(t->_surfaceWrites) : 0;
        const raw::Texture *luma = rc->readTexture(t->_raw.luma);
        const uint64_t nativeGeneration = luma ? luma->nativeGeneration : 0;
        if (!t->_surface || !t->_imageCurrent || t->_imageSeed != seed || t->_imageWrites != writes ||
            t->_imageNativeGeneration != nativeGeneration) {
            stale.push_back(t);
            converted_.push_back({t, seed, writes, nativeGeneration});
#if SWITCH_FRAME_TIMELINE
            traceSurfaceImage(t, "converted", seed);
#endif
        }
    }
    planar.swap(stale);
    // 4:4:4 code textures: two copy-engine remaps each
    bool copies = false;
    for (__unsafe_unretained RMTexture *t : planar)
        copies |= t->_raw.planar;
    if (copies) {
        raw::BlitEncoder *conv = rc->blit();
        for (__unsafe_unretained RMTexture *t : planar)
            if (t->_raw.planar)
                conv->convertPlanar(t->_raw);
        conv->end();
    }
    // YCbCr textures' planes whose rows or offset are 16 bytes apart but not 32 (ImageIO's photo thumbnails, a width of
    // 360: rows of 368) are sampled through pool images (raw::Texture::sampled, createSurfaceTexture): filled from the
    // planes by the copy engine first, as refresh() fills a sampled surface's (r7u: without it the conversion read
    // whatever the pool held, magenta and green stripes of older images)
    raw::BlitEncoder *fill = nullptr;
    for (__unsafe_unretained RMTexture *t : planar)
        if (t->_raw.ycbcr)
            for (const raw::Texture *x : {t->_raw.luma, t->_raw.chroma})
                if (x && x->sampled && rc->readTexture(x) == x) {
                    if (!fill)
                        fill = rc->blit();
                    fill->copyTexels(*x, 0, 0, 0, *x->sampled, 0, 0, 0, x->width, x->height);
                }
    if (fill)
        fill->end();
    // YCbCr textures: a pass of the pack's conversion state each, into the image the draws sample
    for (__unsafe_unretained RMTexture *t : planar) {
        raw::Texture &x = t->_raw;
        if (!x.ycbcr)
            continue;
        raw::PassDesc p;
        p.color[0].texture = &x;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *conv = rc->render(p);
        if (!conv)
            return fail(PredEncPassRender, "a YCbCr texture's image the raw path cannot render");
        conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
        conv->setBytes(2, 0, x.shape, sizeof x.shape);
        conv->setTexture(0, x.luma);
        conv->setTexture(1, x.chroma ? x.chroma : x.luma);  // (over plane 1: chroma codes 0, none read)
        conv->setViewport(0, 0, x.width, x.height, 0, 1);
        conv->setScissor(0, 0, x.width, x.height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_))
            return refused();
        conv->end();
    }
    return true;
}

// What an encode ahead cannot take from [from, until): work it is not built for, or what only the commit makes final
uint32_t RMRangeEncode::ineligible(size_t from, size_t until)
{
    const RMCommandPlan &plan = cb_->_plan;
    for (size_t at = from; at < until; ++at) {
        const Op op = plan.operation(at).op;
        if (op == OpSignalEvent || op == OpWaitEvent || op == OpCompute || op == OpDispatch || isKernelCommand(op) ||
            op == OpExecuteCommands || op == OpDrawPatches || op == OpDrawPatchesIndirect || op == OpInvocation ||
            op == OpPredicate || op == OpResetCommands || op == OpCopyCommands)
            return VetoOps;
        if (!RMCommandPlan::isEncoderBegin(op))
            continue;
        const size_t end = plan.encoderEnd(at);
        if (plan.begin(at).refused)
            return VetoRefused;
        if (plan.nativeRangeRefusal(at, end))
            return VetoPlanar;
        bool planar = false;
        plan.forEachResource(at, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            planar |= (use.access & raw::ResourceUse::Sample) && (use.facts.roles & RMResourceFacts::Converted);
        });
        if (planar)
            return VetoPlanar;
        if (op == OpRender && loadsAStoredDepth(plan.legacy(at).x))
            return VetoZcull;
    }
    return 0;
}

// The pass at `x` loads a depth an earlier pass of the command buffer stores: whether that one keeps its zcull region
// (raw::DepthStencilAttachment::loadedLater) is read from the passes after it, which only the whole range's encode has
bool RMRangeEncode::loadsAStoredDepth(size_t x)
{
    const RMPass *passes = cb_->_passes.data();
    const RMPass &pass = passes[x];
    return pass.depth && pass.depthLoad == MTLLoadActionLoad &&
           std::any_of(passes, passes + x, [&](const RMPass &earlier) {
               return earlier.depth == pass.depth && (earlier.zeta[0].store != MTLStoreActionDontCare ||
                                                      earlier.zeta[1].store != MTLStoreActionDontCare);
           });
}

// Encoded ahead, the encoder at `begin` is entered: its resources named, its opaque borders' sampler headers made, the
// fresh textures it uses first written ahead of it (where the end of the encoder before it did not). A surface it writes
// that an encoder before it read through the native image stops it: the whole range's encode names the pitch view to every
// encoder.
bool RMRangeEncode::enter(size_t begin)
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

bool RMRangeEncode::step(size_t until)
{
    const Encoding encoding;
    if (!whole_)
        if (const uint32_t veto = ineligible(at_, until)) {
            rc_->abandon();
            abandoned_ = true;
            return stop(veto);
        }
    RAW_ENCODE_BEGIN(loopSpan, Loop);
    for (; at_ < until; ++at_) {
        if (!whole_ && RMCommandPlan::isEncoderBegin(cb_->_plan.operation(at_).op) && !enter(at_))
            return false;
        if (!command(at_))
            return false;
        if (rc_->contentsDeferred) {
            rc_->abandon();
            abandoned_ = true;
            return stop(VetoContents);
        }
    }
    RAW_ENCODE_END(loopSpan);
    return true;
}

bool RMRangeEncode::resume(size_t to, const char **why, size_t *refusedEncoder)
{
    why_ = why;
    refusedEncoder_ = refusedEncoder;
    to_ = to;
    ahead_ = false;
    rc_->deferContents = false;
    sampledShadows_ = cb_->_sampledShadows;
    standIns_ = cb_->_standIns;
    if (validateNativeEventWaits(cb_, generation_) != raw::EventDependencyState::ready) {
        rc_->abandon();
        abandoned_ = true;
        *why = "an event producer failed or is pending on a channel made again since";
        return false;
    }
    return true;
}

void RMRangeEncode::reset()
{
    if (rc_ && !abandoned_ && !sealed_)
        rc_->abandon();
    sealed_ = abandoned_ = ahead_ = false;
    veto_ = 0;
    firstContents_.reset();
    counters_.reset();
    ce_.reset();
    re_ = nullptr;
    be_ = nullptr;
    rc_ = nullptr;
    cb_ = nil;
    serialCompute_ = computeUnit_ = continuing_ = encoderShadows_ = false;
    kernelShadows_ = {};
    stateCursor_ = {};
    fused_ = {};
    fusedAt_ = renderBegin_ = encoder_ = freshAt_ = SIZE_MAX;
    planned_ = {};
    standInLoads_.clear();
    planesWritten_.clear();
    standInStores_.clear();
    backings_ = {};
    resolves_.clear();
    resolveTargets_.clear();
    turned_ = {};
    depthResolve_ = {};
    stencilResolve_ = {};
    converted_.clear();
    fresh_.clear();
    for (raw::Texture &t : levelTargets_)
        t = {};
    zetaPart_ = {};
    standInFresh_.clear();
    codes_.clear();
#if SWITCH_RAW_DIAGNOSTICS
    levelPasses_ = 0;
#endif
}

bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why, size_t *refusedEncoder)
{
    if (RMTexture *feedback = (__bridge RMTexture *)cb->_plan.nativeRangeRefusal(from, to)) {
        *why = "a planar texture sampled and rendered in one command buffer";
        hit(cb, PredEncPlanarRT, feedback->_raw.width, feedback->_raw.height);
        return false;
    }
    RMRangeEncode &encode = rmRangeEncode();
    // what was encoded ahead of the commit goes on: the rest of the range, then the seal
    if (rmPipelineTake(cb, from)) {
        bool stale = false;
        const bool encoded = encode.resume(to, why, refusedEncoder) && encode.step(to) && encode.seal(to, stale);
        const bool stopped = !encoded && encode.vetoed();
        encode.reset();
        if (encoded) {
            rmPipelineSealed(cb, to);
            return true;
        }
        cb->_released.clear();
        if (!stopped && !stale)
            return false;
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        bool stale = false;
        const bool encoded =
            encode.open(cb, from, to, true, why, refusedEncoder) && encode.step(to) && encode.seal(to, stale);
        encode.reset();
        if (encoded) {
            rmPipelineSealed(cb, to);
            return true;
        }
        cb->_released.clear();
        if (!stale)
            return false;
    }
    *why = "the native surface changed repeatedly before submission";
    failCommandBuffer(cb, @"Native surface changed during submission");
    return false;
}
