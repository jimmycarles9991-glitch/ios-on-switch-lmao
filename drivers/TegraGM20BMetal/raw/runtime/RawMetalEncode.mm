// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#import "RawMetalCounters.h"
#import "RawMetalPlanAdapters.h"
#import "RawMetalPassFusion.h"
#import "RawMetalReach.h"
#import "RawMetalResolveAsk.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalIndirect.h"
#import "RawMetalTargetViews.h"
#import "RawMetalFresh.h"
#include <array>
#include <optional>
#include "raw_compute.h"
#include "raw_compute_lookup.h"
#include "raw_clear.h"
#include "raw_samples.h"
#include "raw_turned_pass.h"

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then. A refusal inside one
// encoder names that encoder's first command in *refusedEncoder; one no encoder owns (the channel's, the kernel's, a planar
// texture sampled and rendered across the range) leaves it alone.
__attribute__((objc_externally_retained)) static bool
encodeRawAttempt(RMCommandBuffer *cb, size_t from, size_t to, const char **why, bool &stale, size_t *refusedEncoder)
{
    struct Encoding {
        Encoding()
        {
            tRawEncoding = true;
        }

        ~Encoding()
        {
            tRawEncoding = false;
        }
    } encoding;

    raw::Device &d = *gRM->raw;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    d.nextForms = cb->_probeForms;
#endif
    raw::CommandBuffer *rc = d.begin();
    if (!rc) {
        *why = "the kernel gave the raw path no channel";
        if (!predLogged(PredEncNoChannel))
            logPred(PredEncNoChannel, 0, 0, @(d.channelError().c_str()));
        hit(cb, PredEncNoChannel);
        return false;
    }
    const uint64_t generation = d.channelGeneration();
    rc->internalCompletion();
    beginInlineSignals(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    rc->forceWholePrograms(cb->_forceWholePrograms || rmWholePrograms());
    rc->inlineStates(cb->_inlineStates);
#endif
    if (validateNativeEventWaits(cb, generation) != raw::EventDependencyState::ready) {
        rc->abandon();
        *why = "an event producer failed or is pending on a channel made again since";
        return false;
    }
    FreshWrites firstContents(cb, from, to, *rc);
    RAW_ENCODE_BEGIN(resourceSpan, Resources);
    const auto buffers = [&](auto visit) {
        cb->_plan.forEachResource(from, to, RMResourceKind::Buffer, visit);
    };
    const auto textures = [&](auto visit) {
        cb->_plan.forEachResource(from, to, RMResourceKind::Texture, visit);
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
    cb->_plan.forEachResource(from, to, RMResourceKind::Heap, [&](const RMRangeResource &use) {
        if (__unsafe_unretained RMHeap *heap = (__bridge RMHeap *)use.object; heap->_members)
            rc->useHeap(*heap->_members);
    });
    raw::RenderEncoder *re = nullptr;
    raw::BlitEncoder *be = nullptr;
    std::optional<raw::ComputeEncoder> ce;
    bool serialCompute = false, computeUnit = false;
    raw::StateCursor stateCursor;
    raw::FusionPass fused;
    size_t fusedAt = SIZE_MAX;
    RMCounterEmit counters(cb, from, to, *rc);
    size_t renderBegin = to;
    bool continuing = false;
    size_t encoder = to;
    // A range that starts inside a compute encoder (a loop's continuation, or what follows a block that holds loops:
    // RMSegment::held): that encoder's work goes on in a new raw encoder, the planned state applied at its first dispatch
    for (size_t at = from; at-- > 0;) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpCompute) {
            ce.emplace(*rc, d);
            serialCompute = c.a == MTLDispatchTypeSerial;
            ce->setSerial(serialCompute);
            computeUnit = true;
        }
        if (c.op == OpCompute || c.op == OpRender || c.op == OpBlit || c.op == OpEnd)
            break;
    }

    auto fail = [&](RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0) {
        *why = w;
        hit(cb, p, a, b);
        if (encoder < to)
            *refusedEncoder = encoder;
        rc->abandon();
        return false;
    };
    // a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
    auto refused = [&]() {
        static const RMPred map[] = {PredEncPassRender,  PredEncIndexedRange, PredEncColorFormats, PredEncUboOffset,
                                     PredEncNoTic,       PredEncDepthStencil, PredEncSamples,      PredEncGeometry,
                                     PredEncStorageView, PredPassRotated};
        const raw::Refusal &f = rc->refusal;
        hit(cb, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
        if (encoder < to)
            *refusedEncoder = encoder;
        rc->abandon();
        return false;
    };
    // A planar render target's stand-in passes (raw::Texture::planarStandIn), the frontend's: the planes' codes into the stand-in
    // before a pass that loads it (its code image from the planes first, the copy engine's remaps), the stand-in into the planes
    // after one that stores it. 0: drawn; else fail()'s or refused()'s answer.
    std::vector<__unsafe_unretained RMTexture *> standInLoads, planesWritten;
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> standInStores;  // and their attachment slots
    auto standInPass = [&](__unsafe_unretained RMTexture *t, bool store) -> int {
        raw::Texture &x = t->_raw;
        if (!store) {
            raw::BlitEncoder *b = rc->blit();
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
        raw::RenderEncoder *e = rc->render(p);
        if (!e) {
            fail(PredEncPlanarRT, "a planar texture's stand-in pass the raw path cannot render", store);
            return 1;
        }
        e->setPipeline(d.standInPipeline(store));
        e->setTexture(0, store ? x.planarStandIn : &x);
        if (!e->draw(3 /* MTLPrimitiveTypeTriangle */, 0, 3, 1, 0, why)) {
            refused();
            return 2;
        }
        e->end();
        if (store)
            planesWritten.push_back(t);
        return 0;
    };
    // The pass's backings of its YCbCr targets' stand-ins, read before the resolve passes take the encoder over, then the
    // stand-ins stored into the planes
    std::array<const raw::Texture *, 8> backings{};
    auto standInBackings = [&] {
        backings = {};
        for (const auto &[t, j] : standInStores)
            if (t->_raw.ycbcrStore && re)
                backings[j] = re->target(j);
    };
    auto storeStandIns = [&]() -> bool {
        for (const auto &[t, j] : standInStores) {
            if (!t->_raw.ycbcrStore) {
                if (standInPass(t, true))
                    return false;
                continue;
            }
            if (!backings[j])
                return fail(PredEncPlanarRT, "a YCbCr texture's pass left no stand-in backing", j);
            if (const int failed = storeYcbcrPlanes(rc, t->_raw, *backings[j], why))
                return failed == 1 ? fail(PredEncPlanarRT, *why, j) : refused();
            planesWritten.push_back(t);
        }
        standInStores.clear();
        return true;
    };

    // Multisampled attachments' resolves (MTLStoreActionMultisampleResolve): after their pass, a pass of the pack's resolve state
    // of the format each, into the resolve texture over the pass's render area (an 8-bit sRGB one as its UNORM twin, whose codes
    // the state writes), sampling what the pass rendered (the resolve pass's prologue barrier orders its reads after those
    // writes)
    struct Resolve {
        uint32_t attachment;
        __unsafe_unretained RMTexture *into;
        bool keepSamples;  // MTLStoreActionStoreAndMultisampleResolve
        uint32_t level, slice;
    };

    std::vector<Resolve> resolves;
    std::deque<raw::Texture> resolveTargets;  // the encoder reads them until its end()
    raw::TurnedPass turned;                   // the open pass's turn copies (raw_turned_pass.h)

    // A depth attachment's resolve (raw_samples.h depthResolveState): a pass of the depth resolve state of its format and filter
    // into the resolve texture, writing each pixel's depth whatever it held
    struct DepthResolve {
        __unsafe_unretained RMTexture *into = nil;
        uint32_t filter = 0;
    } depthResolve;

    struct StencilResolve {  // CommandBuffer::stencilResolve's pass (raw_stencil_bits.h)
        __unsafe_unretained RMTexture *into = nil;
        raw::StencilSample pick = raw::StencilSample::Zero;
    } stencilResolve;

    static const raw::DepthStencilState depthWrites = [] {
        raw::DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return raw::DepthStencilState(desc);
    }();
    auto resolvePasses = [&]() -> bool {
        const raw::Texture *from[8] = {};
        for (const Resolve &r : resolves)
            from[r.attachment] = re->target(r.attachment);
        const raw::Texture *depthFrom = depthResolve.into ? re->depthTarget() : nullptr;
        const raw::Texture *stencilFrom = stencilResolve.into ? re->depthTarget() : nullptr;
        // a memoryless zeta's compressed backing is made without a texture header (Device::compBacking)
        if (const raw::Texture *z = depthFrom ? depthFrom : stencilFrom;
            z && !d.sampleable(*const_cast<raw::Texture *>(z)))
            return fail(PredEncDepthStencil, "no texture header for a multisampled zeta's resolve", z->format);
        const uint32_t width = re->areaWidth(), height = re->areaHeight();
        for (const Resolve &r : resolves) {
            raw::Texture part;
            const raw::Texture *into = rawAttachmentTarget(r.into, r.level, r.slice, false, part);
            if (!into)
                return fail(PredEncPassRender, "a resolve level, slice or plane the raw path cannot render",
                            r.attachment);
            const raw::ResolveAsk ask = resolveAsk(r.into, *into, *from[r.attachment], width, height);
            raw::ResolveRoute route = raw::resolveRoute(ask);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            if (d.probeResolveDraw.load(std::memory_order_relaxed))
                route = raw::ResolveByPass;
            else if (raw::twodResolveShape(ask) && d.probeResolveExact.load(std::memory_order_relaxed))
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
            resolveTargets.push_back(*into);
            resolveTargets.back().format = raw::resolveTargetFormat(into->format);
            raw::PassDesc p;
            p.color[0].texture = &resolveTargets.back();
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
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            e->end();
            RAW_DIAGNOSTIC(d.stats.resolveRoute[raw::ResolveByPass][raw::resolveSampleSlot(samples)]++);
        }
        resolves.clear();
        if (__unsafe_unretained RMTexture *into = depthResolve.into) {
            raw::PassDesc p;
            p.depthStencil.texture = &into->_raw;
            p.width = width;
            p.height = height;
            useTexture(rc, into, true);
            raw::RenderEncoder *e = depthFrom ? rc->render(p) : nullptr;
            if (!e)
                return fail(PredEncDepthStencil, "a depth resolve texture the raw path cannot render",
                            into->_raw.format);
            const uint32_t samples = depthFrom->samples;
            e->setPipeline(d.depthResolvePipeline(into->_raw.format, depthResolve.filter));
            e->setDepthStencil(&depthWrites);
            e->setTexture(0, depthFrom);
            e->setBytes(2, 0, &samples, sizeof samples);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            e->end();
            depthResolve = {};
        }
        if (__unsafe_unretained RMTexture *into = stencilResolve.into) {
            useTexture(rc, into, true);
            if (!stencilFrom)
                return fail(PredEncDepthStencil, "a stencil resolve without its source", into->_raw.format);
            if (!rc->stencilResolve(into->_raw, *stencilFrom, stencilResolve.pick, width, height, why))
                return fail(PredEncDepthStencil, *why, into->_raw.format);
            stencilResolve = {};
        }
        return true;
    };

    // the planar textures its draws sample: their images from their planes first, once (m2v converts at each encoder that uses
    // one; nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's
    // full barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
    // image is made when a draw samples it. A sampler that clamps to zero or to the border reads a 4:4:4 code texture as clamp to
    // edge (m2v's address map), which the raw path does not derive: such a command buffer is m2v's.
    struct Conversion {
        __unsafe_unretained RMTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    std::vector<Conversion> converted;  // planar images converted here, at their surfaces' seeds and write counts
    {
        std::vector<__unsafe_unretained RMTexture *> planar;
        std::vector<__unsafe_unretained RMSampler *> samplers;
        textures([&](const RMRangeResource &use) {
            if ((use.access & raw::ResourceUse::Sample) && (use.facts.roles & RMResourceFacts::Converted))
                planar.push_back((__bridge RMTexture *)use.object);
        });
        if (!planar.empty())
            cb->_plan.forEachResource(from, to, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
                samplers.push_back((__bridge RMSampler *)use.object);
            });
        if (!planar.empty()) {
            // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
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
                    converted.push_back({t, seed, writes, nativeGeneration});
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
                if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                    return refused();
                conv->end();
            }
        }
    }
    RAW_ENCODE_END(resourceSpan);
    // textures sampled through a pool image (rows 16 but not 32 apart, raw::Texture::sampled): refreshed from their texels by the
    // copy engine before the first pass that samples them, and again after work here wrote them (m2v copies such a surface into
    // its shadow at each encoder that uses it once written since)
    std::set<RMTexture *> fresh;
    raw::Texture levelTargets[8];  // the open pass's level targets: the encoder reads them until its end()
    raw::Texture zetaPart;         // and its depth/stencil attachment's layer
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t levelPasses = 0;
    static std::atomic<bool> loggedLevel{false};
#endif
    auto refresh = [&](size_t pass) {
        cb->_plan.forEachResource(pass, cb->_plan.encoderEnd(pass), RMResourceKind::Texture,
                                  [&](const RMRangeResource &use) {
                                      if (!(use.access & raw::ResourceUse::Sample))
                                          return;
                                      __unsafe_unretained RMTexture *t = (__bridge RMTexture *)use.object;
                                      if (rc->readTexture(&t->_raw) != &t->_raw || !t->_raw.sampled ||
                                          (!t->_raw.surfaceView && !fresh.insert(t).second))
                                          return;
                                      const raw::Texture &x = t->_raw;
                                      raw::BlitEncoder *b = rc->blit();
                                      b->copyTexels(x, 0, 0, 0, *x.sampled, 0, 0, 0, x.width, x.height);
                                      b->end();
                                  });
    };
    // extended-range textures (raw_xr.h): a draw samples the stand-in, drawn from the codes by the pack's conversion state
    // before the first pass whose draws sample it, and again after work here writes the codes. Their passes' prologue barrier
    // orders the conversion between the codes' writes and the samples.
    std::set<RMTexture *> standInFresh;
    std::deque<raw::Texture>
        codes;  // the conversions' sources: the codes' headers (the encoder reads them until its end)
    auto standIns = [&](size_t pass) {
        std::vector<__unsafe_unretained RMTexture *> stale;
        cb->_plan.forEachResource(pass, cb->_plan.encoderEnd(pass), RMResourceKind::Texture,
                                  [&](const RMRangeResource &use) {
                                      if (!(use.access & raw::ResourceUse::Sample))
                                          return;
                                      __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
                                      if (texture->_raw.standIn && !standInFresh.count(texture) &&
                                          (!texture->_raw.xrPlanes || cb->_xrStandIns.count(texture)))
                                          stale.push_back(texture);
                                  });
        for (__unsafe_unretained RMTexture *t : stale) {
            raw::Texture &x = t->_raw;
            raw::PassDesc p;
            p.color[0].texture = x.standIn;
            p.color[0].load = raw::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            raw::RenderEncoder *conv = rc->render(p);
            if (!conv)
                return fail(PredEncPassRender, "an extended-range texture's stand-in the raw path cannot render");
            codes.push_back(x);
            codes.back().standIn = nullptr;
            useTexture(rc, t, false);
            conv->setPipeline(d.xrPipeline(raw::xrForm(x.format), x.xrPlanes));
            conv->setTexture(0, x.xrPlanes ? x.luma : &codes.back());
            if (x.xrPlanes)
                conv->setTexture(1, x.chroma);
            conv->setViewport(0, 0, x.width, x.height, 0, 1);
            conv->setScissor(0, 0, x.width, x.height);
            if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return refused();
            conv->end();
            standInFresh.insert(t);
        }
        return true;
    };
    RAW_ENCODE_BEGIN(loopSpan, Loop);
    for (size_t at = from; at < to; at++) {
        const Cmd c = cb->_plan.legacy(at);
        if ((c.op == OpRender && !rmContinuesPass(cb, at)) || c.op == OpCompute || c.op == OpBlit) {
            encoder = at;
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
            std::array<__unsafe_unretained RMTexture *, 31> shadows{};
            size_t count = 0;
            for (uint32_t slots = dispatch.surfaceSlots; slots; slots &= slots - 1) {
                __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)textures[__builtin_ctz(slots)];
                if (texture && rc->readTexture(&texture->_raw) == &texture->_raw &&
                    std::find(shadows.begin(), shadows.begin() + count, texture) == shadows.begin() + count)
                    shadows[count++] = texture;
            }
            auto copyShadows = [&](bool store) {
                ce->end();
                ce.reset();
                raw::BlitEncoder *copy = rc->blit();
                for (size_t i = 0; i < count; ++i) {
                    const raw::Texture &view = shadows[i]->_raw, &image = *view.sampled;
                    copy->copyTexels(store ? image : view, 0, 0, 0, store ? view : image, 0, 0, 0, view.width,
                                     view.height);
                }
                copy->end();
                ce.emplace(*rc, d);
                ce->setSerial(serialCompute);
                stateCursor.reset();
            };
            if (count)
                copyShadows(false);
            encodePlannedState(cb, at, *rc, re, &*ce, stateCursor);
            const auto &regions = dispatch.regions;
            ce->setStageInRegion(dispatch.stageIn);
            if (!kernels || kernels->count != regions.count)
                return fail(PredCompute, "a dispatch with an incomplete region plan", c.op);
            for (uint8_t region = 0; region < regions.count; ++region) {
                ce->setState(kernels->jobs[region]->value());
                if (const char *w = nullptr; !ce->dispatchRegion(regions.regions[region], regions.grid, regions.nominal,
                                                                 !region, region + 1 == regions.count, &w))
                    return fail(PredCompute, w, c.op);
            }
            if (count) {
                copyShadows(true);
                fresh.clear();
            }
            continue;
        }
        {
            RAW_ENCODE_SPAN(State);
            encodePlannedState(cb, at, *rc, re, ce ? &*ce : nullptr, stateCursor);
        }
        if (isKernelCommand(c.op)) {
            RAW_ENCODE_SPAN(Compute);
            if (const char *w = nullptr; !encodeKernel(*ce, *rc, cb, c, at, &w))
                return fail(PredCompute, w, c.op);
            continue;
        }
        switch (c.op) {
        case OpCompute:
            if (!computeUnit || serialCompute != (c.a == MTLDispatchTypeSerial))
                rc->endPredicationUnit();
            computeUnit = true;
            ce.emplace(*rc, d);
            serialCompute = c.a == MTLDispatchTypeSerial;
            ce->setSerial(serialCompute);
            counters.started(at, *ce);
            break;
        case OpRender: {
            computeUnit = false;
            rc->endPredicationUnit();
            RAW_ENCODE_SPAN(Pass);
            const RMPass &pass = cb->_passes[c.x];
            renderBegin = at;
            if (continuing) {
                continuing = false;
                uint8_t stores = 0;
                for (uint32_t i = 0; i < 8; ++i)
                    if (pass.color[i].texture && pass.color[i].store == MTLStoreActionStore)
                        stores |= uint8_t(1u << i);
                re->continuePass(stores);
                planSplitPrograms(cb, at, *re);
                break;
            }
            raw::PassDesc p;
            for (size_t sub = at; sub == at || (sub < to && rmContinuesPass(cb, sub));
                 sub = cb->_plan.encoderEnd(sub)) {
                if (cb->_sampledShadows)
                    refresh(sub);
                if (cb->_standIns && !standIns(sub))
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
                        standInLoads.push_back(t);
                    if (stores)
                        standInStores.push_back({t, j});
                }
                if (!fresh.empty())
                    fresh.erase(t);  // this pass writes it
                standInFresh.erase(t);
                // a one-layer pass's part has a header of its own (a fetch of the attachment reads it)
                raw::Texture *target =
                    rawAttachmentTarget(t, uint32_t(a.level), uint32_t(a.slice), pass.layers <= 1, levelTargets[j]);
                if (!target)
                    return fail(PredEncPassRender, "a level, slice or plane the raw path cannot render to",
                                a.level | a.slice << 8);
                const bool whole = target == &t->_raw;
                if (!whole) {
#if SWITCH_RAW_DIAGNOSTICS
                    levelPasses++;
                    if (!loggedLevel.exchange(true))
                        os_log(rawLog(),
                               "raw: renders into levels and slices (first: level %lu slice %lu of %ux%u format %u "
                               "type %u)",
                               (unsigned long)a.level, (unsigned long)a.slice, t->_raw.width, t->_raw.height,
                               t->_raw.format, unsigned(t->_raw.type));
#endif
                }
                p.color[j].texture = !whole                  ? &levelTargets[j]
                                     : t->_raw.planarStandIn ? t->_raw.planarStandIn
                                                             : &t->_raw;
                p.color[j].image = whole ? nullptr : &t->_raw;
                p.color[j].level = uint32_t(a.level);
                p.color[j].slice = uint32_t(a.slice);
                p.color[j].load = a.load == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                                  : a.load == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                                : raw::ColorAttachment::DontCare;
                p.color[j].store =
                    a.store == MTLStoreActionStore || a.store == MTLStoreActionStoreAndMultisampleResolve;
                if (a.store == MTLStoreActionMultisampleResolve || a.store == MTLStoreActionStoreAndMultisampleResolve)
                    if (__unsafe_unretained RMTexture *r = (RMTexture *)a.resolve)
                        resolves.push_back({j, r, a.store == MTLStoreActionStoreAndMultisampleResolve,
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
                depthResolve = {r, pass.depthFilter};
            if (__unsafe_unretained RMTexture *r = (RMTexture *)pass.stencilResolve)
                stencilResolve = {r, pass.stencilPick};
            if (__unsafe_unretained RMTexture *z = (RMTexture *)pass.depth) {  // (rawDepthStencil: decided at record)
                p.depthStencil.texture = &z->_raw;
                if (pass.depthSlice && !z->_raw.memoryless) {  // a layer of a 2D array or cube: its layers from there
                    zetaPart = z->_raw;
                    zetaPart.va += pass.depthSlice * z->_raw.layerBytes;
                    zetaPart.layers -= uint32_t(pass.depthSlice);
                    zetaPart.tic = 0;
                    p.depthStencil.texture = &zetaPart;
                }
                p.depthStencil.clearDepth = pass.depthLoad == MTLLoadActionClear;
                p.depthStencil.clearStencil = pass.stencilLoad == MTLLoadActionClear;
                p.depthStencil.loaded = pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad;
                p.depthStencil.stored =
                    pass.zeta[0].store != MTLStoreActionDontCare || pass.zeta[1].store != MTLStoreActionDontCare;
                p.depthStencil.depth = float(std::clamp(pass.clearDepth, 0.0, 1.0));
                p.depthStencil.stencil = pass.clearStencil;
            }
            p.width = uint32_t(std::min<NSUInteger>(pass.width, UINT32_MAX));
            p.height = uint32_t(std::min<NSUInteger>(pass.height, UINT32_MAX));
            p.layers = pass.layers;
            p.positions = pass.positions;
            for (__unsafe_unretained RMTexture *t : standInLoads)
                if (standInPass(t, false))
                    return false;
            standInLoads.clear();
            PlanDraws draws(cb, at, to, *rc);
            if (const char *refused = raw::expandXrPlanes(p, *gRM->raw))
                return fail(PredEncPassRender, refused);
            if (!turned.begin(*rc, p, draws, why))
                return fail(PredPassRotated, *why);
            if (!(re = rc->render(p)))
                return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
            counters.started(at, *re);
            planSplitPrograms(cb, at, *re);
            break;
        }
        case OpBlit:
            computeUnit = false;
            rc->endPredicationUnit();
            be = rc->blit();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            for (const auto &o : cb->_probeOps)
                if (o.at == at)
                    be->probe(o.op, o.read ? &o.read->_raw : nullptr, o.written->_raw);
#endif
            counters.started(at, *be);
            break;
        case OpEnd:
            if (re)
                re->finishVisibility();
            if (re && at + 1 < to && cb->_plan.operation(at + 1).op == OpRender) {
                if (rmContinuesPass(cb, at + 1)) {
                    continuing = true;
                    break;
                }
                if (!re->turned() && !rmGroupedPass(cb, renderBegin) && !rmGroupedPass(cb, at + 1) &&
                    !counters.at(at) && !counters.at(at + 1)) {
                    // the next pass's facts are the next boundary's first pass's
                    const raw::FusionPass first = fusedAt == renderBegin ? fused : fusionPass(cb, renderBegin);
                    fused = fusionPass(cb, at + 1);
                    fusedAt = at + 1;
                    if (raw::canFusePasses(first, fused)) {
                        continuing = true;
                        break;
                    }
                }
            }
            if (re)
                re->end();
            standInBackings();
            if (re && !resolvePasses())
                return false;
            if (be)
                be->end();
            if (ce) {
                ce->end();
                ce.reset();
            }
            if (!turned.end(*rc, why))
                return fail(PredPassRotated, *why);
            if (!storeStandIns())
                return false;
            counters.ended(at);
            re = nullptr;
            be = nullptr;
            break;
        case OpDraw:
            if (!re->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why))
                return refused();
            break;
        case OpBarrier:
            re->barrier();
            break;
        case OpVisibility:
            if (!re->setVisibility(
                    c.a,
                    [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr,
                    c.c))
                return fail(PredCallVisibility, "a visibility offset the raw path cannot take", c.a, c.c);
            break;
        case OpDrawIndexed: {
            const auto &draw = cb->_plan.draw(at);
            __unsafe_unretained RMBuffer *indices = (__bridge RMBuffer *)draw.indices;
            useBuffer(rc, indices, false);
            if (!re->drawIndexed(draw.primitive, draw.count, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4,
                                 &indices->_raw, draw.indexOffset, draw.instances, draw.baseVertex, draw.baseInstance,
                                 why))
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
                    ? re->drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why)
                    : re->drawIndexedIndirect(draw.primitive, draw.indexType == MTLIndexTypeUInt16 ? 2 : 4,
                                              &indices->_raw, draw.indexOffset, &arguments->_raw, draw.argumentOffset,
                                              why);
            if (!accepted)
                return refused();
            break;
        }
        case OpExecuteCommands:
            if (!encodeCommands(re, rc, cb->_plan.draw(at), why))
                return refused();
            break;
        case OpDrawPatches:
        case OpDrawPatchesIndirect:
            if (!encodePatches(re, rc, cb->_plan.draw(at), cb->_plan.patches(at), c.op == OpDrawPatchesIndirect, why))
                return refused();
            break;
        case OpResetCommands:
        case OpCopyCommands:
            encodeCommandsBlit(be, c);
            break;
        case OpMipmaps: {
            RAW_ENCODE_SPAN(Blit);
            if (!encodeMipmaps(rc, (__bridge RMTexture *)c.obj, uint32_t(c.c), be, why))
                return refused();
            break;
        }
        case OpFillTexture: {
            RAW_ENCODE_SPAN(Blit);
            be->end();
            if (!fillTexturePass(cb, c, rc))
                return fail(PredBlitTexFill, "a texture fill into a target the raw path cannot render",
                            ((__bridge RMTexture *)c.obj)->_descriptor.pixelFormat);
            be = rc->blit();
            break;
        }
        case OpCopyTexture:
        case OpCopyBuffer:
        case OpFillBuffer:
        case OpCopyBufferToTexture:
        case OpCopyTextureToBuffer: {
            RAW_ENCODE_SPAN(Blit);
            encodeBlit(cb, c, rc, be, fresh, standInFresh);
            break;
        }
        case OpSignalEvent:
            if (!isInlineSignal(cb, at))
                break;
            computeUnit = false;
            rc->endPredicationUnit();
            if (!releaseInlineSignal(cb, at, rc)) {
                encoder = to;
                return fail(PredEvent, "a signal inside a raw range whose event the kernel no longer holds");
            }
            break;
        default:  // states, fences, events, resources the raw path needs nothing for
            break;
        }
    }
    RAW_ENCODE_END(loopSpan);
    RAW_ENCODE_SPAN(Tail);
    if (re)
        re->end();
    standInBackings();
    if (re && !resolvePasses())
        return false;
    if (be)
        be->end();
    if (ce)
        ce->end();
    if (!turned.end(*rc, why))
        return fail(PredPassRotated, *why);
    if (!storeStandIns())
        return false;
    encoder = to;
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
            firstContents.submitted(cb);
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
    firstContents.submitted(cb);
    publishSharedEvents(cb);
    if (rc->promisesKept) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promises.clear();
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb->_probeTaken.add(rc->probeTaken());
    noteReplayShape(cb, from, to);
    d.stats.slotsCompared += stateCursor.slotsCompared;
    d.stats.slotsInTables += stateCursor.slotsInTables;
#endif
    for (const Conversion &c : converted) {  // (submitted: their images hold the surfaces at these seeds)
        c.t->_imageSeed = c.seed;
        c.t->_imageWrites = c.writes;
        c.t->_imageNativeGeneration = c.nativeGeneration;
        c.t->_imageCurrent = true;
    }
    noteSurfaceWrites(cb, from, to);
    for (__unsafe_unretained RMTexture *t :
         planesWritten)  // a write the surface's seed does not show: its image is converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    cb->_channelGeneration = generation;
    cb->_stamp = cb->_segs.size() == 1 && cb->_segs.front().raw ? rc->stamp : 0;
    cb->_rangeLoopStamp = rc->loopsLaid ? rc->stamp : 0;
    RAW_DIAGNOSTIC(d.stats.levelPasses += levelPasses);
    RAW_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    RAW_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    RAW_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}

bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why, size_t *refusedEncoder)
{
    if (RMTexture *feedback = (__bridge RMTexture *)cb->_plan.nativeRangeRefusal(from, to)) {
        *why = "a planar texture sampled and rendered in one command buffer";
        hit(cb, PredEncPlanarRT, feedback->_raw.width, feedback->_raw.height);
        return false;
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        bool stale = false;
        if (encodeRawAttempt(cb, from, to, why, stale, refusedEncoder))
            return true;
        cb->_released.clear();
        if (!stale)
            return false;
    }
    *why = "the native surface changed repeatedly before submission";
    failCommandBuffer(cb, @"Native surface changed during submission");
    return false;
}
