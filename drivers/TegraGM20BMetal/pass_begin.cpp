// SPDX-License-Identifier: MIT
// See command_buffer.h (CommandBuffer::render: its attachments, their backings (pass_backings.cpp) and shadows).
#include "lock.h"
#include "fx/fx_image.h"
#include "attachment_lifetime.h"
#include "gpu.h"
#include "layers.h"
#include "mixed_samples.h"
#include "rotated_pass.h"
#include "zeta_colour.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

RenderEncoder *CommandBuffer::render(const PassDesc &pass, const char **why)
{
    const auto refused = [why](const char *rule) -> RenderEncoder * {
        if (why)
            *why = rule;
        return nullptr;
    };
    constexpr const char *kNoMemory =
        "no memory for a pass's attachment backing: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)";
    SWITCH_METAL_ENCODE_SPAN(PassBegin);
    RenderEncoder &e = render_;
    // a pass's lists keep the last one's capacity: no allocation a steady frame
    std::vector<RenderEncoder::Body> bodies = std::move(e.bodies_);
    std::vector<std::array<uint32_t, 4>> rects = std::move(e.rects_);
    SplitPassPlan splitPlan = std::move(e.splitPlan_);
    std::vector<RenderEncoder::Clear> clears = std::move(e.initialClears_);
    std::vector<bool> tiles = std::move(e.tiles_);
    std::vector<GeometryAnalysis> written = e.written_.release();
    bodies.clear();
    rects.clear();
    splitPlan.clear();
    clears.clear();
    // made again where it is, not built aside and moved in (the encoder is 7 KB)
    e.~RenderEncoder();
    new (&e) RenderEncoder();
    e.bodies_ = std::move(bodies);
    e.rects_ = std::move(rects);
    e.splitPlan_ = std::move(splitPlan);
    e.initialClears_ = std::move(clears);
    e.tiles_ = std::move(tiles);
    e.written_.adopt(std::move(written));
    e.cb_ = this;
    e.dev_ = dev_;
    e.pass_ = pass;
    SWITCH_METAL_DIAGNOSTIC(e.prologueMark_ = dev_->spans_.reserve());
    SWITCH_METAL_DIAGNOSTIC(if (drawTimes_) e.timePassBegin());
    // the render hardware takes one memory layout for all of a pass's color targets (nvk_rendering_linear): block-linear when
    // one of them is, or is a pitch target the hardware cannot take (rendered through a shadow either way), or beside a zeta,
    // its pitch ones then rendered through block-linear shadows as NVK does, or when every one is memoryless; memoryless ones
    // get a backing of the pass's layout
    const Texture *zeta = pass.depthStencil.texture;
    if (zeta && !zeta->zeta && !colourAsDepth(*zeta))
        return refused("a depth/stencil attachment of a colour pixel format, which Metal's validation layer stops");
    // every colour attachment of one sample count (Metal's rule), the depth/stencil of theirs or a mixed pass's
    // (mixed_samples.h)
    constexpr const char *kSampleCounts =
        "attachments of different sample counts, which Metal's validation layer stops "
        "(MTLRenderPipeline.h rasterSampleCount: the samples per fragment of the "
        "render pass)";
    uint8_t samples = 0;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture) {
            if (samples && t->samples != samples)
                return refused(kSampleCounts);
            samples = t->samples;
        }
    const uint8_t zetaSamples = zeta ? zeta->samples : 0;
    if (zetaSamples && samples && zetaSamples != samples) {
        if (!mixedSamples(samples, zetaSamples))
            return refused(kSampleCounts);
        if (pass.separate.stencil)
            return refused("a separate depth and stencil pair of more samples than its pass's colour: no form built");
        e.rasterSamples_ = zetaSamples;
    }
    e.samples_ = std::max<uint8_t>(samples ? samples : zetaSamples, 1);
    if (const RotatedRefusal turn = passRotation(pass, e.rot_); turn != RotatedRefusal::None)
        return refused(rotatedRefusalText(turn));
    if (e.rot_.turned() && e.rasterSamples_ > e.samples_)
        return refused("a turned pass beside a depth/stencil of more samples than its colour: no form built");
    if (const char *layered = layeredRefusal(pass, e.samples_, e.rot_.turned()))
        return refused(layered);
    const uint32_t layers = e.pass_.layers = std::max(pass.layers, 1u);
    const auto layered = [layers](Texture shape) {
        shape.layers = layers;
        return shape;
    };
    // a turned pass's attachment that is not turned is transient (passRotation): a backing laid out as the plane stands in
    const auto backed = [&](const Texture &t) {
        return t.memoryless || standsInTurned(t, e.rot_);
    };
    // an attachment of fewer layers than the pass renders through a backing of the pass's, its own layers copied in and back:
    // what the GPU does with a layer past a target's THIRD_DIMENSION is not in the class header, and Apple's drops it
    const auto shortOf = [layers](const Texture &t) {
        return layers > 1 && !t.memoryless && std::max(t.layers, 1u) < layers;
    };
    const bool hidden = !zeta && pass.hiddenZeta;
    bool blockLinear = zeta != nullptr || hidden || layers > 1, pitchTarget = false;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture; t && !backed(*t)) {
            if (t->blockLinear || t->shadowTarget || (pass.arrayedStorageMask >> j & 1))
                blockLinear = true;
            else
                pitchTarget = true;
        }
    blockLinear |= !pitchTarget;
    // the device's backings: those the last passes asked the same of with no lock, else under the device's lock once a
    // pass however many attachments need one (the compression thread trims them under it)
    std::unique_lock<gm20b::Mutex> backingLock(dev_->lock_, std::defer_lock);
    const auto backing = [&](const Texture &shape, uint32_t tag, bool linear, bool compression, bool copied = false) {
        if (Texture *b = backingLock.owns_lock() ? nullptr : dev_->lastBacking(shape, tag, linear, compression, copied))
            return b;
        if (!backingLock.owns_lock()) {
            backingLock.lock();
            SWITCH_METAL_ENCODE_COUNT(Locks);
        }
        return dev_->backing(shape, tag, linear, compression, copied);
    };
    // a colour texture as the depth attachment: a Depth32Float backing (zeta_colour.h)
    const Texture zetaShape = zeta && !zeta->zeta ? colourAsDepthShape(*zeta) : zeta ? *zeta : Texture();
    if (zeta && backed(*zeta)) {
        Texture *b = backing(layered(planeShaped(zetaShape, e.rot_)), 0, false, true);
        if (!b)
            return refused(kNoMemory);
        e.pass_.depthStencil.texture = b;
        backings_.push_back(b);
    } else if (zeta && !pass.separate.stencil && (!zeta->zeta || shortOf(*zeta))) {
        // the copy engine is proven over no compressed zeta: the backing takes no compression
        bool compressed = false;
#if SWITCH_METAL_DIAGNOSTICS
        compressed = e.zetaShadowCompressed_ = dev_->probeZetaShadow.load(std::memory_order_relaxed);
#endif
        Texture *b = backing(layered(zetaShape), 0, false, compressed, true);
        if (!b)
            return refused(kNoMemory);
        e.pass_.depthStencil.texture = b;
        e.zetaShadowOf_ = zeta;
        backings_.push_back(b);
    }
    uint8_t scratchMask = 0;
    for (uint32_t j = 0; j < pass.count && j < 8; ++j)
        if (const Texture *t = pass.color[j].texture; t && (backed(*t) || t->shadowTarget || t->zetaPages || t->zeta ||
                                                            shortOf(*t) || (blockLinear && !t->blockLinear)))
            scratchMask |= uint8_t(1u << j);
    const auto scratchTags = transientColorTags(scratchMask);
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const Texture *t = pass.color[j].texture;
        if (!t)
            continue;
        // a depth texture as a colour attachment: a view of its colour twin (zeta_colour.h)
        const uint32_t depthColour = t->zeta && t->samples <= 1 ? depthAsColourFormat(t->format) : 0;
        if (t->zeta && !depthColour)
            return refused("a depth/stencil texture as a colour attachment the driver keeps no bits of: a "
                           "multisampled one, or of a format with no colour twin");
        if (!t->renderable && !t->shadowTarget && !depthColour)
            return refused("a colour attachment the driver does not render to (pass.fmt's at record)");
        const Texture own = depthColour ? zetaColourView(*t, depthColour) : *t;
        const bool mixed = blockLinear && !t->blockLinear && !backed(*t);
        if (((t->shadowTarget || own.zetaPages || shortOf(*t)) && !backed(*t)) ||
            mixed) {  // rendered through a pool image (end(): copied in before, back after)
            SWITCH_METAL_DIAGNOSTIC(dev_->stats.xrPlaneShadows += t->xrPlane != 0);
            if (mixed) {
                SWITCH_METAL_DIAGNOSTIC(dev_->stats.rtMixed++);
            } else if (t->shadowTarget) {
                SWITCH_METAL_DIAGNOSTIC(dev_->stats.rtUnaligned++);
                SWITCH_METAL_DIAGNOSTIC(dev_->stats.rtUnalignedPitch = t->pitch);
                SWITCH_METAL_DIAGNOSTIC(dev_->stats.rtUnalignedVa = uint32_t(t->va & 255));
            }
            // a zeta's view takes plain memory: gpu.x1.ce_compressed proved the copy engine over compressed BGRA8 and
            // RGBA16Float texels, not over a zeta's depth bits and stencil
            Texture *b = backing(shortOf(*t) ? layered(own) : own, scratchTags[j], blockLinear,
                                 !own.zetaPages && (dev_->storageCompressedOk() || !(pass.storageMask >> j & 1)), true);
            if (!b || !b->renderable)
                return refused(kNoMemory);
            e.pass_.color[j].texture = b;
            e.shadowOf_[j] = t;
            backings_.push_back(b);
            continue;
        }
        if (backed(*t)) {  // back it for this pass
            Texture *b = backing(layered(planeShaped(own, e.rot_)), scratchTags[j], blockLinear,
                                 layers == 1 && (dev_->storageCompressedOk() || !(pass.storageMask >> j & 1)));
            if (!b)
                return refused(kNoMemory);
            e.pass_.color[j].texture = b;
            e.memoryless_[j] = true;
            backings_.push_back(b);
        }
    }
    if (hidden) {
        Texture *b = backing(e.hiddenZetaShape(pass), 0, false, true);
        if (!b)
            return refused(kNoMemory);
        e.pass_.depthStencil = {b, true, true, 1.0f, 0};
        e.hiddenZeta_ = true;
        backings_.push_back(b);
    }
    if (const Texture *stencil = pass.separate.stencil) {
        constexpr uint32_t kRG8Unorm = 30;
        Texture shape;
        shape.width = stencil->width;
        shape.height = stencil->height;
        shape.samples = stencil->samples;
        shape.layers = layers;
        shape.format = kZetaFormat;
        shape.zeta = shape.memoryless = true;
        // a Depth16Unorm's pair names no depth here: the caller hands the pass a converted scratch (depthConvert)
        Texture *b = pass.separate.depth ? backing(shape, Device::kSeparateZetaTag, false, true, true) : nullptr;
        shape.format = kRG8Unorm;
        shape.zeta = false;
        Texture *temporary =
            b || !pass.separate.depth ? backing(shape, Device::kSeparateTempTag, true, false, true) : nullptr;
        if (!temporary)
            return refused(kNoMemory);
        if (b) {
            e.pass_.depthStencil.texture = b;
            backings_.push_back(b);
        }
        e.separate_ = pass.separate;
        e.separateTemp_ = temporary;
        backings_.push_back(temporary);
    }
    if (backingLock.owns_lock())
        backingLock.unlock();
    // the depth's stored region (zcull_planes.h), taken before this pass's own write leaves it stale
    const ZcullStored zcullHeld = zeta ? zcullPlanes_.held(zeta->va) : ZcullStored();
    for (uint32_t j = 0; j < e.pass_.count && j < 8; ++j)
        if (const Texture *texture = e.pass_.color[j].texture)
            use(*texture, true);
    if (const Texture *texture = e.pass_.depthStencil.texture)
        use(*texture, true);
    // the render area: from the origin, the smallest attachment's extent, each axis within the pass's target size where it
    // names one (m2v render_pass.rs `area`); Metal's default viewport and scissor cover it (render_state.rs `metal_defaults`)
    // (a separate pair's stencil is an attachment of its own extent, as an M4 Max counts it)
    const Texture *extents[10] = {zeta, pass.separate.stencil};
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        extents[2 + j] = e.pass_.color[j].texture;
    for (const Texture *t : extents)
        if (t) {
            e.areaW_ = e.areaW_ ? std::min(e.areaW_, t->width) : t->width;
            e.areaH_ = e.areaH_ ? std::min(e.areaH_, t->height) : t->height;
        }
    if (e.rot_.turned())
        frameArea(pass, e.rot_, e.areaW_, e.areaH_);
    if (pass.width)
        e.areaW_ = std::min(e.areaW_, pass.width);
    if (pass.height)
        e.areaH_ = std::min(e.areaH_, pass.height);
    // (a separate depth and stencil pair renders a scratch zeta of the pass's, which keeps zcull off)
    // (a mixed pass keeps zcull off: which sample layout its regions take under target-independent rasterisation is not
    // in the class header)
    ZcullAsk zask;
    zask.on = dev_->zcullOn_ && e.rasterSamples_ <= e.samples_;
#if SWITCH_METAL_DIAGNOSTICS
    zask.on |= dev_->zcullOn_ && e.samples_ == 1 && e.rasterSamples_ == 4 &&
               dev_->probeZcullMixed.load(std::memory_order_relaxed);
#endif
    zask.depth = zeta && !pass.separate.stencil && zetaHasDepth(zeta->format);
    zask.cleared = pass.depthStencil.clearDepth && !std::isnan(pass.depthStencil.depth);
    zask.layers = layers;
    zask.zetaLayers = zeta ? std::max(zeta->layers, 1u) : 1;
    zask.atStorageBase = zeta && (!zeta->storage.va || zeta->va == zeta->storage.va);
    const Rect zcullArea = e.planeArea();
    const bool zcullKeep =
        zeta && e.pass_.depthStencil.texture == zeta && pass.depthStencil.stored && pass.depthStencil.loadedLater;
    zcullBegin(e.zcull_, zask, dev_->zcullInfo_, zcullArea.x0, zcullArea.y0, zcullArea.x1 - zcullArea.x0,
               zcullArea.y1 - zcullArea.y0, layers, zcullHeld, zcullKeep, [this](uint64_t bytes) {
                   uint64_t va = 0;
                   dev_->alloc(dev_->upload_, uint32_t(bytes), kZcullStorageAlign, va);
                   return va;
               });
    // DontCare can become a clear after later draws establish readBits_. Treat its deferred prologue write like Clear.
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (e.pass_.color[j].texture && e.pass_.color[j].load != ColorAttachment::Load)
            e.attachmentWrites_.rop |= uint8_t(1u << j);
    if (e.attachmentWrites_.rop) {
        DrawGeometry cleared;
        cleared.limit = {0, 0, e.areaW_, e.areaH_};
        e.written_.add(cleared);
    }
    // the body goes straight into the push ring; the pass prologue (targets, clears) is written at end() and runs first
    e.reserve();
    e.setViewport(0, 0, e.areaW_, e.areaH_, 0, 1);
    e.setScissor(0, 0, e.areaW_, e.areaH_);
    e.extrasAtBegin_ = extrasLive_;
    if (extrasLive_) {
        heldSegment_.lose();
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.segmentExtrasLosses++);
    } else {
        heldSegment_.passBoundary();
    }
    extrasLive_ = nullptr;
    // a target the pass does not load starts over: no earlier rung-2 blend is under what it draws (rule fetch.blend)
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (pass.color[j].texture && pass.color[j].load != ColorAttachment::Load)
            for (const Texture *t : {static_cast<const Texture *>(pass.color[j].texture), e.shadowOf_[j]})
                if (t)
                    __atomic_store_n(&t->ropBlendSerial, uint64_t(0), __ATOMIC_RELAXED);
    passes++;
    return &e;
}

}  // namespace gm20b
