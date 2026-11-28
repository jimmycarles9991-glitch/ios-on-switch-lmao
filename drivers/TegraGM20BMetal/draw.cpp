// SPDX-License-Identifier: MIT
// See render_encoder.h (RenderEncoder's draws).
#include "lock.h"
#include "icb.h"
#include "gpu.h"
#include "kit.h"
#include "mixed_samples.h"
#include "mme.h"
#include "probe_restores.h"
#include "tiled_cache.h"
#include "view.h"
#include "frame_cpu_counts.h"

#include <algorithm>
#include <iterator>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
// An instance of a draw: BEGIN with INSTANCE_ID SUBSEQUENT (two words), the first vertex or index and the count (three), END
constexpr uint32_t kInstanceWords = 6;

// A split draw's samples 1-3 (mixed_samples.h splitsSamples): `again` drawn under their sample mask, colour writes off,
// and the pixel stage off for MixedDraw::Split. `p` is the encoder's body, which `reserve` replaces when it runs short.
template <typename Reserve, typename Again>
void drawLaterSamples(Push &p, Reserve reserve, uint32_t &maskHeld, uint32_t rasterMask, const uint32_t (&ctWrite)[8],
                      uint32_t targets, MixedDraw mixed, Again again)
{
    const bool stageOff = mixed == MixedDraw::Split;
    if (p.room() < kLaterSamplesWords)
        reserve();
    laterSamplesOn(p, rasterMask, targets, stageOff);
    maskHeld = rasterMask & kLaterSamplesMask;
    again();
    if (p.room() < kLaterSamplesWords)
        reserve();
    laterSamplesOff(p, ctWrite, targets, stageOff);
}
}  // namespace

static_assert(kFetchPatch == icb::kPatchPrimitive);

uint64_t CommandBuffer::batchLimit() const
{
    return dev_->role() == Device::Role::RenderServer ? BatchWork::kRenderServerLimit : BatchWork::kLimit;
}

// ---- render encoder ----

// Room for the pass body: 16K dwords at a time (a CA pass of 100 draws is ~6K); a full one becomes a segment and the body goes on
// in a new one, so a pass never drops a draw.
void RenderEncoder::reserve()
{
    checkRoom(p_, "a pass body");
    if (bodyStart_ && p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    if (bodyStart_)
        dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    const uint32_t room = 1 << 14;
    bodyStart_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, room * 4, 256, va));
    p_ = Push{bodyStart_, bodyStart_ + room};
}

void RenderEncoder::finishDraw(uint32_t vertices, uint32_t instances, bool indirect)
{
    const Pipeline &pl = *emitted_;
#if SWITCH_METAL_PROF
    if (dev_->spans_.on()) {
        if (p_.room() < kSpanMarkWords)
            reserve();
        markSpan(p_, dev_->spans_.reserve(), SpanKind::Draws);
    }
#endif
    SWITCH_METAL_DIAGNOSTIC(if (cb_->drawTimes_) timeDraw(vertices, instances, indirect));
    const uint64_t vertexWork = BatchWork::product(BatchWork::product(vertices, instances), pl.vertexInstructions);
    const uint64_t fragmentCost =
        BatchWork::product(uint64_t(pl.fragmentInstructions) * pl.samplePasses, pl.writesMemory[1] ? 32u : 1u);
    const uint64_t fragmentWork = BatchWork::product(BatchWork::product(drawPixels_, instances), fragmentCost);
    if (cb_->batchWork_.add(indirect ? BatchWork::kSaturated : vertexWork + fragmentWork, cb_->batchLimit()))
        cutBatch();
}

void RenderEncoder::cutBatch()
{
    if (p_.at > bodyStart_) {
        bodies_.push_back({bodyStart_, p_.at, 0, 0, false, true});
        bodyStart_ = p_.at;
    } else if (!bodies_.empty()) {
        bodies_.back().batchEnd = true;  // complete indirect argument continuation, never its method header
    }
    cb_->batchWork_ = {};
}

bool RenderEncoder::drawnWhole(const Pipeline &pl) const
{
    return amplifies() || (pl.viewportIndex && views_.viewports > 1);
}

void RenderEncoder::wholeDraw(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                              uint32_t baseVertex, uint32_t baseInstance)
{
    SWITCH_METAL_DIAGNOSTIC(if (drawUnits(instances) >= kMeasureUnits) dev_->stats.unpricedLarge++);
    if (amplifies()) {
        amplifyCall(pl, indexed, first, count, instances, baseVertex, baseInstance);
        return;
    }
    if (p_.room() < kInstanceWords)
        reserve();
    mme::draw(p_, params_, indexed, first, count, instances, baseVertex, baseInstance);
}

// The full barrier m2v puts between passes: the shaders' data writes flushed (storage buffers and textures; ROP writes are
// behind the wait for idle) and their data and texture caches invalidated
void RenderEncoder::barrier()
{
    if (p_.room() < 64)
        reserve();
    kit(p_, switch_gm20b_pass_barrier, tiledCache(dev_->k_.info()));
    attachmentWrites_ = {};
    written_.clear();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    anyWritten_ = false;
    ropWritten_ = storageWritten_ = 0;
#endif
}

bool RenderEncoder::draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
                         const char **why)
{
    if (amplifies() && runsPerAmplification(primitive, count, instances, false))
        return amplified([&] {
            return draw(primitive, first, count, instances, baseInstance, why);
        });
    SWITCH_METAL_ENCODE_SPAN(Draw);
    Pipeline *pl = formFor(pipeline_, {primitive, count, instances, false, false});
    if (!pl)
        return true;  // m2v drops a draw without a pipeline (parity 0006) the same way
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count || !instances)
        return true;
    const bool ordered = orderedFetch(*pl, {primitive, count, instances, false, false});
    if (ordered && primitive == icb::kPatchPrimitive) {
        refuseFetch(*pl, FetchRefusal::Geometry, primitive, count, instances, why);
        return false;
    }
    if (!prepare(pl, primitive, first, first, count, false, why))
        return skippedDraw();
    const MixedDraw mixed = mixedForm(*emitted_);
    const bool split = splitsSamples(mixed);
    const uint32_t drawnInstances = amplifies() ? instances * amplifications_ : instances;
    // samples 1-3 write no colour: no fetch of a colour they wrote to order, so the plain draw
    const auto laterSamples = [&] {
        drawLaterSamples(
            p_,
            [this] {
                reserve();
            },
            cb_->sampleMaskHeld_, compatibleRasterMask_, emitted_->ctWrite, std::min(pass_.count, 8u), mixed,
            [&] {
                wholeDraw(*pl, false, first, count, instances, first, baseInstance);
                finishDraw(count, drawnInstances);
            });
    };
    bool wholeQuad = instances == 1 && written_.lastNonOverlappingQuad();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeFetchRung_ == kProbeBlendTwinTriangles)
        wholeQuad = false;
#endif
    if (ordered && !wholeQuad) {
        fetchDraw(primitive, false, first, count, instances, first, baseInstance);
        if (split)
            laterSamples();
        cb_->draws++;
        return true;
    }
    cb_->draws += amplifies() ? amplifications_ : 1;
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.amplifiedDraws += amplifies());
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (instances == 1 && dev_->probeArmed.load(std::memory_order_relaxed))
        if (const auto probe = dev_->armedProbe()) {
            if (p_.room() < kInstanceWords + probe->before.size() + probe->after.size() + 2 * std::size(kProbeRestores))
                reserve();
            if (!probe->before.empty())
                p_.raw(probe->before.data(), uint32_t(probe->before.size()));
            dev_->probeEmitted.fetch_add(1, std::memory_order_relaxed);
            wholeDraw(*pl, false, first, count, instances, first, baseInstance);
            if (!probe->after.empty())
                p_.raw(probe->after.data(), uint32_t(probe->after.size()));
            for (uint32_t i = 0; i < std::size(kProbeRestores); i++)
                if (probe->restores >> i & 1)
                    p_.m(kProbeRestores[i].method, kProbeRestores[i].value);
            cb_->sampleMaskHeld_ = kSampleMaskUnknown;
            finishDraw(count, drawnInstances);
            return true;
        }
#endif
    if (drawnWhole(*pl)) {
        wholeDraw(*pl, false, first, count, instances, first, baseInstance);
        finishDraw(count, drawnInstances);
    } else {
        emitDraw(false, first, count, instances, first, baseInstance);
    }
    if (split)
        laterSamples();
    return true;
}

// A draw of more than one primitive, or of a count only the GPU knows, whose state fetches a colour it writes through
// the texture unit: the fetch barrier orders draws, not the primitives of one, so it is drawn a primitive at a time
// (fetchDraw). Inside the pixel interlock (the storage form) it is drawn whole.
bool RenderEncoder::orderedFetch(const Pipeline &pl, const FetchDraw &shape) const
{
    if (!fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits) || (!shape.indirect && singlePrimitive(shape)))
        return false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeFetchRung_ == kProbeBlendTwinWhole)
        return false;
#endif
    return true;
}

// Such a draw, prepared, as one call of the fetch macro (mme.h): the GPU draws each instance's primitives one BEGIN
// at a time with the fetch barrier before each, the BEGINs of an instance one split draw (SPLIT_MODE open between them),
// so primitive assembly, the strip's winding, primitive restart and the primitive and instance IDs are the whole draw's.
// The CPU writes the same words whatever the primitive count.
void RenderEncoder::fetchDraw(uint32_t primitive, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                              uint32_t root, uint32_t baseInstance)
{
    if (p_.room() < mme::kFetchShapeWords + mme::kFetchCallWords)
        reserve();
    mme::fetchShape(p_, primitive);
    mme::fetchDraw(p_, params_, indexed, first, count, instances, root, baseInstance);
    finishDraw(count, instances);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    dev_->stats.fetch.sequential++;
    dev_->stats.fetch.sequentialTriangles += uint64_t(primitives(primitive, count)) * instances;
    if (drawUnits(instances) >= kMeasureUnits)
        dev_->stats.unpricedLarge++;
#endif
}

StorageFallback RenderEncoder::storageFallback(const Pipeline &pl, const Pipeline *barrierTwin) const
{
    if (!pl.storageBits)
        return StorageFallback::None;
    if (barrierTwin &&
        fetchesOnlyUnwritten(pl.fetchBits, pl.writeBits, pl.storageWriteBits, barrierTwin->fetchBits,
                             barrierTwin->writeBits))
        return StorageFallback::ReadOnly;
    StorageFallback reason = StorageFallback::None;
    for (uint32_t slot = 0; slot < 8 && reason == StorageFallback::None; slot++)
        if (const Texture *t = pass_.color[slot].texture; t && (pl.storageBits >> slot & 1))
            reason = storageViewMismatch(texelBytes(pl.storageTic[slot]), pl.rec->storageSamples, *t,
                                         dev_->storageCompressedOk());
    return reason;
}

// The state's fetch-free form where this draw takes it (fetch_free.h): the blended colour's target in the pass,
// CPU-visible memory that is not a surface, the rung-2 blends already over it, whether every image its program samples
// is of unorm8 codes, and whether the pass loads it over an earlier command buffer's rung-2 blends decide rule fetch.blend
Pipeline *RenderEncoder::fetchFreeFor(const Pipeline &pl, [[maybe_unused]] StorageRoute *route)
{
    Pipeline *free = __atomic_load_n(&pl.fetchFree, __ATOMIC_ACQUIRE);
    SWITCH_METAL_DIAGNOSTIC(if (route) route->free = FreeMiss::NoForm);
    if (!free)
        return nullptr;
    const FetchFreeForm &f = free->freeForm;
    SWITCH_METAL_DIAGNOSTIC(if (route) {
        route->free = FreeMiss::NoTarget;
        route->rung = f.rung;
    });
    const Texture *t = f.colour < pass_.count ? pass_.color[f.colour].texture : nullptr;
    if (t && f.colour < 8 && shadowOf_[f.colour])
        t = shadowOf_[f.colour];  // the texture the client holds, not the pool image the pass renders it through
    if (!t)
        return nullptr;
    const uint64_t blended = __atomic_load_n(&t->ropBlendSerial, __ATOMIC_RELAXED);
    FetchFreeDraw draw = {t->format, t->storage.hostVisible && !t->import, ropLayers_[f.colour]};
    draw.eightBitSources = !f.samplesPast32 && eightBitSamples(f.sampledSlots, textures_[1]);
    draw.accumulates = pass_.color[f.colour].load == ColorAttachment::Load && !(covered_ >> f.colour & 1) && blended &&
                       blended != cb_->serial;
    uint8_t forced = 0;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    forced = probeFetchRung_;
#endif
    SWITCH_METAL_DIAGNOSTIC(if (route) {
        route->free = FreeMiss::Rule;
        route->draw = draw;
    });
    if (!fetchFreeTaken(f, draw, forced))
        return nullptr;
    SWITCH_METAL_DIAGNOSTIC(if (route) route->free = FreeMiss::None);
    if (f.rung == FetchBlend && fixedBlendCountsLayer(uint16_t(t->format), draw.eightBitSources) &&
        ropLayers_[f.colour] < 255)
        ropLayers_[f.colour]++;
    if (f.rung == FetchBlend)
        __atomic_store_n(&t->ropBlendSerial, cb_->serial, __ATOMIC_RELAXED);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetch.fetchFree[f.rung == FetchBlend ? 0 : 1]++);
    return free;
}

Pipeline *RenderEncoder::formFor(Pipeline *pl, const FetchDraw &shape)
{
    if (!pl || (!pl->fetchBits && !pl->storageBits))
        return pl;
    uint8_t forced = 0;
    Pipeline *blendTwin = pl->storageBlendBits ? __atomic_load_n(&pl->blendTwin, __ATOMIC_ACQUIRE) : nullptr;
    Pipeline *barrierTwin = pl->storageBits ? __atomic_load_n(&pl->barrierTwin, __ATOMIC_ACQUIRE) : nullptr;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    forced = probeFetchRung_;
    if (forced == kProbeBlendTwinTriangles || forced == kProbeBlendTwinWhole)
        if (Pipeline *twin = blendTwin ? blendTwin : barrierTwin) {
            dev_->stats.fetch.blendTwin++;
            return twin;
        }
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (pl->storageBits && cb_->forms_.barrierFetch)
        if (Pipeline *twin = barrierTwin ? barrierTwin : blendTwin)
            return twin;
#endif
    StorageRoute route;
    if (Pipeline *free = fetchFreeFor(*pl, SWITCH_METAL_STATS ? &route : nullptr))
        return free;
    if (!pl->storageBits) {
#if SWITCH_METAL_STATS
        if (pl->fetchBits && pl->rec) {
            route.key = pl->rec->keyHash;
            route.barrier = true;
            route.triangles = triangles(shape);
            route.areaPixels = areaW_ * areaH_;
            noteStorageRoute(route);
        }
#endif
        return pl;
    }
    const StorageFallback reason = storageFallback(*pl, barrierTwin);
    if (reason == StorageFallback::None) {
        Pipeline *twin = blendTwin ? blendTwin : barrierTwin;
        if (forced || !twin || !twinTaken({shape, twin->primitiveId, uint64_t(areaW_) * areaH_, amplifications_ > 1})) {
#if SWITCH_METAL_STATS
            route.key = pl->rec ? pl->rec->keyHash : 0;
            route.twin = !twin ? TwinMiss::NoTwin : forced ? TwinMiss::Probe : TwinMiss::Shape;
            route.triangles = triangles(shape);
            route.areaPixels = areaW_ * areaH_;
            noteStorageRoute(route);
#endif
            return pl;
        }
        SWITCH_METAL_DIAGNOSTIC(cb_->counts_.twinRule++);
        return twin;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const StorageFallback counted = barrierTwin ? reason : StorageFallback::NoTwin;
    dev_->stats.fetch.storageFallback[size_t(counted)]++;
    dev_->stats.fetch.fallbackKeys.note(pl->rec->keyHash);
#endif
    return barrierTwin ? barrierTwin : pl;
}

void RenderEncoder::refuseFetch(const Pipeline &pl, FetchRefusal refusal, uint32_t primitive, uint32_t count,
                                uint32_t instances, const char **why)
{
    *why = fetchRefusalWhy(refusal);
    cb_->refusal = {RefGeometry, primitive, count, instances};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    dev_->stats.fetch.refusedAtEncode[size_t(refusal)]++;
    dev_->stats.fetch.encodeKeys.note(pl.rec ? pl.rec->keyHash : 0);
#else
    (void)pl;
#endif
}

// NVK's indexed draw on MAXWELL_B: the index buffer as NVK_MME_BIND_IB sets it (start, last byte, the restart index of the type,
// the index size) where it changed, the draw parameters as NVK_MME_DRAW_INDEXED's (the base vertex in the root table and the
// base registers, the base instance), then per instance BEGIN, SET_INDEX_BUFFER_F (first index) and DRAW_INDEX_BUFFER (count),
// END. m2v binds the indices the draw reads.
bool RenderEncoder::drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib,
                                uint64_t offset, uint32_t instances, int32_t baseVertex, uint32_t baseInstance,
                                const char **why)
{
    if (amplifies() && runsPerAmplification(primitive, count, instances, true))
        return amplified([&] {
            return drawIndexed(primitive, count, indexSize, ib, offset, instances, baseVertex, baseInstance, why);
        });
    SWITCH_METAL_ENCODE_SPAN(Draw);
    Pipeline *pl = formFor(pipeline_, {primitive, count, instances, false, true});
    if (!pl)
        return true;
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count || !instances)
        return true;
    if (!ib || (indexSize != 2 && indexSize != 4)) {
        cb_->skipped = {RefIndexedRange, uint32_t(offset), indexSize, 0};
        cb_->skips++;
        return true;
    }
    // Metal's fetch (m2v render.rs draw_indexed_primitives, measured): the offset's low 32 bits rounded down to the index size,
    // indices read up to the length rounded up to 4 bytes, index 0 at every position past that
    offset = uint32_t(offset);
    offset -= offset % indexSize;
    const uint64_t fetchEnd = offset < ib->length ? (ib->length + 3) & ~uint64_t(3) : ib->length,
                   inside = fetchEnd > offset ? (fetchEnd - offset) / indexSize : 0;
    Buffer zero;
    zero.hostVisible = false;
    drawIndexCpuSafe_ = false;
    // the index fetch's last byte: the draw's last index where the buffer holds every one, else the buffer's fetch end,
    // the hardware reading index 0 past it (caps row 3D-B11); a draw of no index the buffer holds reads the zero page under
    // the same limit
    uint64_t indexLimit = uint64_t(count) * indexSize;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint32_t probedLimit = cb_->forms_.indexLimit;
    if (probedLimit)
        cb_->taken_.indexLimits++;
#else
    constexpr uint32_t probedLimit = 0;
#endif
    if (probedLimit) {
        indexLimit = probedLimit;
    } else if (count > inside && !inside) {
        zero.va = dev_->zero_.va;
        zero.cpu = dev_->zero_.cpu;
        zero.length = dev_->zero_.bytes;
        ib = &zero;
        indexLimit = std::min<uint64_t>(indexLimit, dev_->zero_.bytes);
        drawIndexCpuSafe_ = uint64_t(count) * indexSize <= dev_->zero_.bytes;
        offset = 0;
    } else if (count > inside) {
        indexLimit = fetchEnd - offset;
    }
    cb_->use(*ib, false);
    // Never scan potentially GPU-produced indices to choose a CPU cache range. Cached vertex bindings are
    // cleaned conservatively below; WC vertices need no clean.
    const uint8_t *at = ib->cpu + offset;
    if (ib->hostVisible && !ib->wc)
        cb_->clean(at, std::min<uint64_t>(indexLimit, uint64_t(count) * indexSize));
    drawIndices_ = at;
    drawIndexSize_ = indexSize;
    drawIndexCount_ = count;
    const bool ordered = orderedFetch(*pl, {primitive, count, instances, false, true});
    const bool prepared = prepare(pl, primitive, baseVertex, 0, 0, true, why);
    drawIndices_ = nullptr;
    if (!prepared)
        return skippedDraw();
    const uint64_t va = ib->va + offset, last = va + indexLimit - 1;
    if (va != ibVa_ || last != ibEnd_ || indexSize != ibSize_) {
        p_.inc(INDEX_BUFFER_A, {uint32_t(va >> 32), uint32_t(va), uint32_t(last >> 32), uint32_t(last)});
        p_.m(DA_PRIMITIVE_RESTART_INDEX, indexSize == 2 ? 0xffffu : 0xffffffffu);
        cb_->restart_ = indexSize == 2 ? 0xffffu : 0xffffffffu;
        p_.m(INDEX_BUFFER_E, indexSize == 2 ? 1 : 2);  // INDEX_SIZE TWO_BYTES / FOUR_BYTES
        ibVa_ = va;
        ibEnd_ = last;
        ibSize_ = indexSize;
    }
    const MixedDraw mixed = mixedForm(*emitted_);
    const bool split = splitsSamples(mixed);
    const uint32_t drawnInstances = amplifies() ? instances * amplifications_ : instances;
    const auto laterSamples = [&] {
        drawLaterSamples(
            p_,
            [this] {
                reserve();
            },
            cb_->sampleMaskHeld_, compatibleRasterMask_, emitted_->ctWrite, std::min(pass_.count, 8u), mixed,
            [&] {
                wholeDraw(*pl, true, 0, count, instances, uint32_t(baseVertex), baseInstance);
                finishDraw(count, drawnInstances);
            });
    };
    bool wholeQuad = instances == 1 && written_.lastNonOverlappingQuad();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeFetchRung_ == kProbeBlendTwinTriangles)
        wholeQuad = false;
#endif
    if (ordered && !wholeQuad) {
        fetchDraw(primitive, true, 0, count, instances, uint32_t(baseVertex), baseInstance);
        if (split)
            laterSamples();
        cb_->draws++;
        return true;
    }
    if (drawnWhole(*pl)) {
        wholeDraw(*pl, true, 0, count, instances, uint32_t(baseVertex), baseInstance);
        finishDraw(count, drawnInstances);
    } else {
        emitDraw(true, 0, count, instances, uint32_t(baseVertex), baseInstance);
    }
    if (split)
        laterSamples();
    cb_->draws += amplifies() ? amplifications_ : 1;
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.amplifiedDraws += amplifies());
    return true;
}

// GPU-owned indirect arguments are fetched by the command streamer after the method header. No argument bytes, indices,
// or positions are read back to the CPU. Geometry uses the conservative render/scissor extent for this draw.
bool RenderEncoder::indirect(uint32_t primitive, bool indexed, const Buffer *arguments, uint64_t offset,
                             const char **why)
{
    SWITCH_METAL_ENCODE_SPAN(Draw);
    if (!pipeline_)
        return true;
    Pipeline *form = formFor(pipeline_, {primitive, 0, 1, true, indexed});
    const Pipeline &pl = *form;
    const bool ordered = orderedFetch(pl, {primitive, 0, 1, true, indexed});
    if (ordered && primitive == icb::kPatchPrimitive) {
        refuseFetch(pl, FetchRefusal::Geometry, primitive, 0, 0, why);
        return false;
    }
    if (ordered && amplifies())
        return amplified([&] {
            return indirect(primitive, indexed, arguments, offset, why);
        });
    const uint32_t bytes = indexed ? 20 : 16;
    if (!arguments || !arguments->va || (offset & 3)) {
        cb_->skipped = {RefIndexedRange, uint32_t(offset), bytes, 0};
        cb_->skips++;
        return true;
    }
    // arguments past their buffer read as zeros, never the memory after it
    const uint32_t inside = argumentWordsInside(arguments->length, offset, bytes / 4);
    called_ = true;
    indirectGeometry_ = true;
    const bool prepared = prepare(form, primitive, 0, 0, UINT32_MAX, indexed, why);
    indirectGeometry_ = false;
    if (!prepared)
        return skippedDraw();
    if (inside) {
        cb_->use(*arguments, false);
        if (arguments->hostVisible && !arguments->wc && arguments->cpu)
            cb_->clean(arguments->cpu + offset, inside * 4);
    }
    if (p_.room() < 8 + bytes / 4 + mme::kAmplificationScratchWords + mme::kAmplifyCallWords + mme::kFetchShapeWords)
        reserve();
    // Work earlier in this command buffer that wrote the arguments must be done and out of the shader caches before the
    // command fetcher reads them; NO_PREFETCH on the argument entry prevents early fetch. Earlier command buffers' work
    // ended in its job's wait-for-idle fence release, which the host passes before this job's entries.
    const bool drain = cb_->bufferWrites_.meets(arguments->va + offset, bytes) ||
                       cb_->textureWrites_.meets(arguments->va + offset, bytes);
    if (drain) {
        p_.m(WAIT_FOR_IDLE, 0);
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateConstant);
        p_.m(0x0050, 0);  // NV906F_SET_REFERENCE: pre-Turing MME command-fetch visibility (NVK indirect-read barrier)
    }
    SWITCH_METAL_STAT((drain ? frameCpuCounts().indirectDrained : frameCpuCounts().indirectFree)
                          .fetch_add(1, std::memory_order_relaxed));
    const auto argumentWords = [&] {
        if (inside) {
            bodies_.push_back({bodyStart_, p_.at, 0, 0, true});
            bodies_.push_back(PushBody::resident(arguments->va + offset, inside, TegraGM20BPushNoPrefetch));
            dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
            bodyStart_ = nullptr;
            reserve();
        }
        for (uint32_t k = inside; k < bytes / 4; k++)
            *p_.at++ = 0;
    };
    if (ordered) {
        mme::fetchShape(p_, primitive);
        mme::fetchHeader(p_, indexed);
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetch.sequential++);
    } else if (amplifies()) {
        amplificationScratch(p_, pl);
        mme::amplifyHeader(p_, indexed);
        heldAmplifyWords(pl);
    } else {
        mme::indirectHeader(p_, indexed);
    }
    argumentWords();
    if (const MixedDraw mixed = mixedForm(*emitted_); splitsSamples(mixed))
        drawLaterSamples(
            p_,
            [this] {
                reserve();
            },
            cb_->sampleMaskHeld_, compatibleRasterMask_, emitted_->ctWrite, std::min(pass_.count, 8u), mixed,
            [&] {
                if (p_.room() < 1 + bytes / 4)
                    reserve();
                if (amplifies())
                    mme::amplifyHeader(p_, indexed);
                else
                    mme::indirectHeader(p_, indexed);
                argumentWords();
            });
    params_.unknown = true;
    cb_->draws += amplifies() ? amplifications_ : 1;
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.amplifiedDraws += amplifies());
    finishDraw(0, 0, true);
    return true;
}

bool RenderEncoder::drawIndirect(uint32_t primitive, const Buffer *arguments, uint64_t offset, const char **why)
{
    return indirect(primitive, false, arguments, offset, why);
}

bool RenderEncoder::drawIndexedIndirect(uint32_t primitive, uint32_t indexSize, const Buffer *indices,
                                        uint64_t indexOffset, const Buffer *arguments, uint64_t argumentOffset,
                                        const char **why)
{
    if (!pipeline_)
        return true;
    if (!indices || !indices->va || (indexSize != 2 && indexSize != 4) || indexOffset % indexSize) {
        cb_->skipped = {RefIndexedRange, uint32_t(indexOffset), indexSize, 0};
        cb_->skips++;
        return true;
    }
    // An offset at or past the end, which Metal's validation takes: index 0 at every position, as NVK binds an empty
    // range (nvk_mme_bind_ib), from the zero page and past its limit (3D-B11)
    const bool inside = indexOffset <= indices->length && indices->length - indexOffset >= indexSize;
    if (p_.room() < 16)
        reserve();
    if (inside)
        cb_->use(*indices, false);
    else
        cb_->useAddress(dev_->zero_.va, dev_->zero_.bytes, false);
    const uint64_t va = inside ? indices->va + indexOffset : dev_->zero_.va,
                   last = inside ? indices->va + indices->length - 1 : dev_->zero_.va + dev_->zero_.bytes - 1;
    p_.inc(INDEX_BUFFER_A, {uint32_t(va >> 32), uint32_t(va), uint32_t(last >> 32), uint32_t(last)});
    cb_->restart_ = indexSize == 2 ? 0xffffu : 0xffffffffu;
    p_.m(DA_PRIMITIVE_RESTART_INDEX, cb_->restart_);
    p_.m(INDEX_BUFFER_E, indexSize == 2 ? 1 : 2);
    ibVa_ = va;
    ibEnd_ = last;
    ibSize_ = indexSize;
    if (inside && indices->hostVisible && !indices->wc && indices->cpu)
        cb_->clean(indices->cpu + indexOffset, indices->length - indexOffset);
    return indirect(primitive, true, arguments, argumentOffset, why);
}

// The state the commands inherit prepared once, as an indirect draw's, the range into the macros' scratch, then the
// commands' words by reference, once per amplification with its id between. Each command sets its topology, restart, index
// buffer and draw parameters, which the encoder holds unknown after them; patch commands their control-stage words too.
bool RenderEncoder::executeSlots(const IcbExecute &x, const char **why)
{
    SWITCH_METAL_ENCODE_SPAN(Draw);
    if (!pipeline_ || !x.count)
        return true;
    Pipeline *form = formFor(pipeline_, {x.primitive, 0, 1, true, true});
    const Pipeline &pl = *form;
    const bool ordered = orderedFetch(pl, {x.primitive, 0, 1, true, x.indexed});
    if (ordered && x.primitive == icb::kPatchPrimitive) {
        refuseFetch(pl, FetchRefusal::Geometry, x.primitive, 0, 0, why);
        return false;
    }
    const uint64_t slotBytes = uint64_t(x.slotWords) * 4, offset = x.first * slotBytes, bytes = x.count * slotBytes;
    if (!x.slots->va || offset > x.slots->length || x.slots->length - offset < bytes) {
        *why = "indirect command buffer commands outside its memory";
        cb_->refusal = {RefCommandsRange, x.first, x.count,
                        uint32_t(std::min<uint64_t>(x.slots->length / std::max<uint64_t>(slotBytes, 1), ~0u))};
        return false;
    }
    if (x.range && (!x.range->va || (x.rangeOffset & 3) || x.rangeOffset > x.range->length ||
                    x.range->length - x.rangeOffset < 8)) {
        *why = "an indirect command buffer's execution range outside its buffer or not four-byte aligned";
        cb_->refusal = {RefCommandsRangeBuffer, uint32_t(std::min<uint64_t>(x.rangeOffset, ~0u)),
                        x.range ? uint32_t(std::min<uint64_t>(x.range->length, ~0u)) : 0};
        return false;
    }
    called_ = true;
    indirectGeometry_ = true;
    const bool prepared = prepare(form, x.primitive, 0, 0, UINT32_MAX, x.indexed, why);
    indirectGeometry_ = false;
    if (!prepared)
        return skippedDraw();
    const bool patches = x.primitive == icb::kPatchPrimitive;
    if (patches && !icbPatchLayout(why))
        return false;
    cb_->use(*x.slots, false);
    if (x.slots->hostVisible && !x.slots->wc && x.slots->cpu)
        cb_->clean(x.slots->cpu + offset, bytes);
    for (size_t i = 0; i < x.bufferCount; i++) {
        const Buffer &b = *x.buffers[i];
        cb_->use(b, false);
        if (b.hostVisible && !b.wc && b.cpu)
            cb_->clean(b.cpu, b.length);
    }
    if (p_.room() < 16)
        reserve();
    if (x.written || x.range) {
        p_.m(WAIT_FOR_IDLE, 0);
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateConstant);
        p_.m(0x0050, 0);  // NV906F_SET_REFERENCE, as indirect(): the fetches wait for the writes before them
    }
    if (x.range) {
        cb_->use(*x.range, false);
        if (x.range->hostVisible && !x.range->wc && x.range->cpu)
            cb_->clean(x.range->cpu + x.rangeOffset, 8);
        mme::icbRangeHeader(p_);
        bodies_.push_back({bodyStart_, p_.at, 0, 0, true});
        bodies_.push_back(PushBody::resident(x.range->va + x.rangeOffset, 2, TegraGM20BPushNoPrefetch));
        bodyStart_ = p_.at;
    } else {
        mme::icbRange(p_, x.first, x.count);
    }
    if (ordered) {
        if (p_.room() < mme::kFetchCommandsWords)
            reserve();
        mme::fetchCommands(p_, true);
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetch.sequential++);
    }
    const auto commands = [&] {
        for (uint32_t a = 0; a < amplifications_; a++) {
            if (amplifications_ > 1)
                icbAmplification(a);
            if (p_.at > bodyStart_)
                bodies_.push_back({bodyStart_, p_.at});
            bodies_.push_back(PushBody::resident(x.slots->va + offset, uint32_t(bytes / 4), TegraGM20BPushNoPrefetch));
            dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
            bodyStart_ = nullptr;
            reserve();
        }
    };
    commands();
    if (ordered)
        mme::fetchCommands(p_, false);
    // the commands again for samples 1-3, which write no colour: drawn whole, not a primitive at a time
    if (const MixedDraw mixed = mixedForm(*emitted_); splitsSamples(mixed))
        drawLaterSamples(
            p_,
            [this] {
                reserve();
            },
            cb_->sampleMaskHeld_, compatibleRasterMask_, emitted_->ctWrite, std::min(pass_.count, 8u), mixed, commands);
    if (patches)
        icbPatchesDone();
    // the commands set the polygon offset of their own kinds (icb::polygonOffset)
    p_.m(NVB197_SET_POLY_OFFSET_POINT, rasterHeld_.bias);
    p_.m(NVB197_SET_POLY_OFFSET_LINE, rasterHeld_.bias);
    p_.m(NVB197_SET_POLY_OFFSET_FILL, rasterHeld_.bias);
    topology_ = restart_ = ~0u;
    cb_->alphaHeld_ = kAlphaControlUnknown;
    cb_->sampleMaskHeld_ = kSampleMaskUnknown;
    ibVa_ = ibEnd_ = ~uint64_t(0);
    ibSize_ = 0;
    params_.unknown = true;
    ++cb_->draws;
    finishDraw(0, 0, true);
    return true;
}

}  // namespace gm20b
