// SPDX-License-Identifier: MIT
// See render_encoder.h (RenderEncoder::end: the pass prologue, its targets and clears).
#include "lock.h"
#include "clear.h"
#include "clear_box.h"
#include "attachment_lifetime.h"
#include "gpu.h"
#include "kit.h"
#include "mme.h"
#include "mixed_samples.h"
#include "multisample.h"
#include "rotated_pass.h"
#include "shader_tests.h"
#include "shader_test_push.h"
#include "stamps.h"
#include "pass_uses.h"
#include "target.h"
#include "tiled_cache.h"

#include <os/log.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
// A turned pass's turn words into the root table (rotation.h kRootTurn), which its fragment programs' turned variants read
constexpr uint32_t kTurnWordsMethods = 16;

// A zeta shadow's words: a compressed zeta's decompression, a copy a layer, the clear of the layers past its own
constexpr uint32_t zetaShadowWords(uint32_t layers)
{
    return kZetaTargetWords + 24 + 32 * layers;
}

void turnWordsInto(Push &p, uint64_t root, const Rotation &r)
{
    uint32_t w[kTurnWords];
    turnWords(r, w);
    p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    p.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kRootTurn);
    p.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), w, kTurnWords);
}
}  // namespace

// The clear registers' words: an extended-range target's codes (xr.h; `straight`: Metal's straight route), else m2v's
// value for the format (clear.h)
static void clearWords(uint32_t format, const float in[4], uint32_t out[4], bool straight)
{
    if (const XrForm xr = xrForm(format); xr != XrForm::None) {
        xrClearTexel(xr, in, straight, out);
        return;
    }
    clearValue(format, in, out);
}

// Clear [x0, x1) x [y0, y1) of target `target` to `v` (its format's clear words), with depth (bit 0) and stencil (bit 1)
// of `zsBits`, in each of `layers` layers, whatever the render enable says
static void clearRect(Push &p, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const uint32_t v[4], uint32_t target,
                      uint32_t zsBits, uint32_t layers)
{
    switch_gm20b_clear_state s = {};
    for (uint32_t i = 0; i < 4; i++)
        s.value[i] = v[i];
    s.xmin = uint16_t(x0);
    s.xmax = uint16_t(x1);
    s.ymin = uint16_t(y0);
    s.ymax = uint16_t(y1);
    s.target = target;
    s.depth = zsBits & 1;
    s.stencil = zsBits & 2;
    s.layers = layers;
    s.layer_macro = mme::kClearLayers;
    kit(p, switch_gm20b_clear, &s);
}

// The depth and stencil of `layers` layers from `first` of [a.x0, a.x1) x [a.y0, a.y1), whatever the render enable says
static void clearZeta(Push &p, const Rect &a, uint32_t zsBits, uint32_t first, uint32_t layers)
{
    const uint32_t payload = zsBits | first << 10;  // CLEAR_SURFACE RT_ARRAY_INDEX
    p.inc(CLEAR_RECT_H, {a.x1 << 16 | a.x0, a.y1 << 16 | a.y0});
    p.m(CLEAR_SURFACE_CONTROL, 0x10);
    p.m(RENDER_ENABLE_OVERRIDE, 1);
    if (layers > 1)
        mme::clearLayers(p, payload, layers);
    else
        p.m(CLEAR_SURFACE, payload);
    p.m(RENDER_ENABLE_OVERRIDE, 0);
}

// A pass of `layers` layers binds that many from the target's own (pitch memory: one)
static void colorTarget(Push &p, uint32_t index, const Texture &t, uint32_t layers)
{
    const uint32_t rows = t.import && t.import->kernelTiled() ? t.import->layout.height : sampleRows(t);
    ColorTargetDesc d = colorTargetDesc(t, formatInfo(t.format)->rt, rows);
    d.layers = layers;
    d.depth = t.type == fx::kTexThreeD;
    d.blockDepth = t.blockDepth;
    const switch_gm20b_target k = kitTarget(d);
    kit(p, switch_gm20b_color_target, index, &k, t.comptags);
}

// A slot with no target below the pass's last one, as NVK writes it: FORMAT DISABLED, VA 0, compressed. Left as it was,
// the slot would still name the target of an earlier pass, which CT_SELECT's count makes the hardware render to.
static void disabledTarget(Push &p, uint32_t index, uint32_t layers)
{
    switch_gm20b_target k = {};
    k.width = 64;
    k.layers = layers;
    kit(p, switch_gm20b_color_target, index, &k, true);
}

// NVK's nvk_flush_rs_state for the state m2v makes of Metal's (render_state.rs `fixed`, `vk_depth_bias`): culling on and
// the face culled, the front face, the polygon mode of both faces, the polygon offset and the depth bias factors (in the
// depth format's units: the pack's SET_DEPTH_BIAS_CONTROL); a register already holding its value is not written
void RenderEncoder::emitSwizzle(Push &p, uint32_t word)
{
    kit(p, switch_gm20b_viewport_swizzle, 0u, word);
}

void RenderEncoder::timestampAtStart(uint64_t va, counters::Point point)
{
    if (startStampCount_ == startStamps_.size())
        return;
    startStamps_[startStampCount_++] = {va, point};
    cb_->useAddress(va, counters::kRecordBytes, true);
}

void RenderEncoder::end()
{
    SWITCH_METAL_ENCODE_SPAN(PassEnd);
    Device &d = *dev_;
    endShaderTests();
    if (p_.room() < 64 + kSampleWords + kTirWords + 2 * kMaxViewports + kDrawStampWords)
        reserve();
    if (zcull_.store && !zcull_.invalidated) {
        uint32_t *store = p_.at;
        *p_.at++ = kZcullUnstoredWord;
        const Texture &zeta = *pass_.depthStencil.texture;
        const auto memory = zeta.storageRange();
        cb_->zcullPlanes_.stored(zcullStoredBy(zcull_, zeta.va, memory.va, memory.bytes, store));
    }
    // a slot taken for a region the pass ended without (invalidated, or a depth whose memory no write could be matched
    // against) goes back: the job's owner could never take it again
    if (zcull_.slot >= 0 && !cb_->zcullPlanes_.holdsSlot(zcull_.slot))
        d.zcullSlots_.release(zcull_.slot);
    writeRaster(p_, Raster());
    if (rot_.turned()) {
        emitSwizzle(p_, kSwizzleIdentity);
        for (uint32_t turned = views_.swizzled & ~1u; turned; turned &= turned - 1)
            kit(p_, switch_gm20b_viewport_swizzle, uint32_t(__builtin_ctz(turned)), kSwizzleIdentity);
    }
    // the draw parameters the next encoder starts from: the root table's and the base registers, 0
    if (params_.unknown || params_.root || params_.baseInstance) {
        const uint32_t zero[2] = {};
        p_.m(LOAD_CB_OFFSET, 0);
        p_.nonInc(LOAD_CB, zero, 2);
        p_.m(GLOBAL_BASE_INSTANCE_INDEX, 0);
    }
    if (params_.unknown || params_.baseVertex) {
        p_.m(GLOBAL_BASE_VERTEX_INDEX, 0);
        p_.m(VERTEX_ID_BASE, 0);
    }
    if (rasterSamples_ > samples_) {
#if SWITCH_METAL_DIAGNOSTICS
        if (tirProbe_)
            tirOffProbed(p_, tirProbe_);
        else
#endif
            tirOff(p_);
        if (cb_->sampleMaskHeld_ != kSampleMaskAll) {
            p_.inc(NVB197_SET_SAMPLE_MASK_X0_Y0, {kSampleMaskAll, kSampleMaskAll, kSampleMaskAll, kSampleMaskAll});
            cb_->sampleMaskHeld_ = kSampleMaskAll;
        }
    }
    if (std::max(samples_, rasterSamples_) > 1)
        sampleWords(p_, 1, d.root_.va);
    // the 3D engine's work outside the pass (the blit encoder's draws) meets no iterated blend and the pass's
    // targets, and the pixel stage and rasteriser on; the state's segment, run again at its next bind, turns them off
    if (const Pipeline *live = cb_->extrasLive_;
        live && (live->freeForm.rung == FetchIteratedBlend || live->stagesOff)) {
        p_.raw(live->restore.data(), uint32_t(live->restore.size()));
        cb_->extrasLive_ = nullptr;
        cb_->heldSegment_.lose();
    }
    if (narrowed_) {
        p_.m(CT_SELECT, targetSelect(colorTargets()));
        narrowed_ = false;
    }
    const bool flush = keepsTiledFlush(cb_->barrierForms().switchForm);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (drew_ && !flush && tiledCache(d.k_.info()))
        cb_->taken_.flushes++;
#endif
#if SWITCH_METAL_DIAGNOSTICS
    nvnPassEnd();
#endif
    const bool measured = measurePass(drew_ && flush);
    if (drew_ && flush)
        tiledCacheDrain(p_, d.k_.info());
    if (measured)
        emitDrawStamp(p_, d.stamps_.va, candidate_.slot, true, candidate_.payload);
    const bool bodyShaderFlushed =
        attachmentWrites_.storage || cb_->passBufferWrites_ || passShaderWrites_ || planned_.shaderWrites;
    if (bodyShaderFlushed)
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    checkRoom(p_, "a pass body");
    if (p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - d.push_.bo.cpu);  // give back the unused reservation
    // pass prologue: the full barrier m2v puts between passes, the targets, then the clears
    SWITCH_METAL_ENCODE_BEGIN(prologueSpan, PassPrologue);
    if (!clearsSealed_)
        clears(initialClears_);
    const std::vector<Clear> &cl = initialClears_;
    const uint32_t layers = std::max(pass_.layers, 1u);
    const uint32_t restoreCount = extrasAtBegin_ ? uint32_t(extrasAtBegin_->restore.size()) : 0u;
    const uint32_t room = 160 + SWITCH_GM20B_KIND_SWITCH_WORDS + 8 * 32 * layers + uint32_t(cl.size()) * 48 +
                          3 * (kZetaTargetWords + kZetaClearWords + 24) + kZcullRegionWords + kSampleWords +
                          kTurnWordsMethods + restoreCount + kSpanMarkWords +
                          startStampCount_ * counters::kTimestampWords + kDrawStampWords +
                          (separate_.stencil ? 64 + 128 * layers : 0) + (zetaShadowOf_ ? zetaShadowWords(layers) : 0) +
                          (rasterSamples_ > samples_ ? 3 * kSampleWords + kTirWords + 16 : 0) + kTimedPassWords;
    const uint32_t totalRoom = room + (uint32_t(cl.size()) + 3) * kTiledCacheSizeWords + 2 * kTiledCacheFlushWords;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, totalRoom * 4, 256, va));
    Push p{start, start + totalRoom};
    SWITCH_METAL_DIAGNOSTIC(if (cb_->drawTimes_) d.emitDrawTime(p, betweenTime_));
    if (restoreCount)
        p.raw(extrasAtBegin_->restore.data(), restoreCount);
    const BarrierEngine before = cb_->lastEngine_;
    const BarrierForms forms = cb_->barrierForms();
    const PassUses &uses = passUses();
    const bool hadBufferWrites =
        cb_->passBufferWrites_ || planned_.bufferWrites || passWritesMemory_ || (!uses.known && !rot_.turned());
    cb_->passBufferWrites_ = false;
    cb_->inRender_ = false;
    const PassForm pass = forms.pass != PassForm::Rule ? forms.pass : passRule(before, uses);
    const bool alreadyIdle = forms.pass == PassForm::Rule && cb_->passShaderFlushed_;
    const Opened opened = passOpening(p, pass, switchFormFor(forms.switchForm, before, BarrierEngine::ThreeD),
                                      entryFormFor(forms.entry), before, cb_->prevBufferWrites_, alreadyIdle);
    if (cb_->takeKindSwitch())
        kit(p, switch_gm20b_kind_switch);
    advanceChain(cb_->chain_, uses, opened != Opened::Pass);
    if (opened != Opened::Pass)
        cb_->prevBufferWrites_ = false;
    cb_->prevBufferWrites_ |= hadBufferWrites;
    cb_->passShaderFlushed_ = bodyShaderFlushed;
    cb_->computeReleased_ = false;
    cb_->lastEngine_ = BarrierEngine::ThreeD;
    SWITCH_METAL_DIAGNOSTIC(cb_->taken_.count(opened));
    SWITCH_METAL_DIAGNOSTIC(cb_->pixelOpenings_ += opened == Opened::Pass && pass == PassForm::PixelOrdered);
    for (uint8_t i = 0; i < startStampCount_; i++)
        counters::emit(p, startStamps_[i]);
    if (rot_.turned()) {
        emitSwizzle(p, rot_.swizzle());
        turnWordsInto(p, d.root_.va, rot_);
    }
    // ahead of the clears: a clear reaches every sample of its rectangle's pixels through it
    if (samples_ > 1)
        sampleWords(p, samples_, d.root_.va, pass_.positions, rot_);
    // shadow targets' pool images: their texels over the render area first, as NVK's tiled shadow loads them (a load that
    // keeps them: Load, and a DontCare the clears leave; a Clear clears it all), then the barrier to the pass
    // (a target of fewer layers than the pass: its own layers alone)
    bool copied = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (const Texture *o = shadowOf_[j]) {
            if (pass_.color[j].load == ColorAttachment::Clear || attachmentStartsCleared(j, areaW_, areaH_, cl))
                continue;
            cb_->use(*o, false);
            cb_->use(*pass_.color[j].texture, true);
            const Rect a = planeArea();
            for (uint32_t l = 0; l < std::min(std::max(o->layers, 1u), layers); l++)
                copySides(p, sideOf(*o, a.x0, a.y0, l), sideOf(*pass_.color[j].texture, a.x0, a.y0, l), a.x1 - a.x0,
                          a.y1 - a.y0, o->bpp, d.stats);
            SWITCH_METAL_DIAGNOSTIC(d.stats.compCopies += pass_.color[j].texture->comptags);
            copied = true;
        }
    if (zetaShadowOf_ && (pass_.depthStencil.loaded || pass_.depthStencil.stored)) {
        copyZetaShadow(p, true);
        copied = true;
    }
    if (copied)
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    if (separate_.stencil)
        copySeparateZeta(
            p, (separate_.loadDepth ? kShaderTestDepth : 0) | (separate_.loadStencil ? kShaderTestStencil : 0), true);
    // the clears m2v makes by passes of their own first (Clear loads reaching past the render area): each its attachment alone
    // as target 0, over its reach, then the barrier between passes
    bool own = false;
    for (const Clear &c : cl) {
        if (!c.own)
            continue;
        const Texture *t = pass_.color[c.target].texture;
        const Rect r = planeRect(c.x0, c.y0, c.x1, c.y1);
        colorTarget(p, 0, *t, layers);
        kit(p, switch_gm20b_render_area, 1u, r.x1, r.y1);
        p.m(ZT_SELECT, 0);
        tiledCacheSetSize(p, d.k_.info(), tiledCacheTileSize(t->bpp * std::max<uint32_t>(t->samples, 1u)),
                          cb_->tiledCacheSize_);
        uint32_t v[4] = {};
        clearWords(pass_.color[c.target].texture->format, c.value, v,
                   !called_ && pass_.color[c.target].texture->compressed);
        d.clearZbc(*t, !r.x0 && !r.y0 && r.x1 >= t->width && r.y1 >= t->height, v);
        clearRect(p, r.x0, r.y0, r.x1, r.y1, v, 0, 0, layers);
        cb_->tiledCacheDirty_ = true;
        own = true;
    }
    const DepthStencilAttachment &zs = pass_.depthStencil;
    uint32_t ownZetaCleared = 0;
    if (!called_ && !pass_.drawn) {
        bool zetaLayout = false;
        const auto ownZetaClear = [&](const Texture &t, uint32_t bits) {
            // a turned pass's zeta is a stand-in of its plane (turned_pass.h): the pass's frame turned
            const Rotation frame = rot_.turned() ? rot_ : Rotation{0, t.width, t.height};
            if (!bits || (frame.width <= areaW_ && frame.height <= areaH_))
                return false;
            const uint32_t zetaLayers = std::min(layers, std::max(t.layers, 1u));
            cb_->use(t, true);
            // a mixed pass's four-sample zeta under its own sample layout (the prologue left the colour's), and outside
            // the zcull region the previous pass left active
            if (rasterSamples_ > samples_ && !zetaLayout) {
                sampleWords(p, t.samples, d.root_.va, pass_.positions, rot_);
                zetaLayout = true;
            }
            if (d.zcullOn_)
                zcullOffWords(p);
            const Rect r = planeRect(0, 0, frame.width, frame.height);
            kit(p, switch_gm20b_render_area, 0u, r.x1, r.y1);
            zetaTargetWords(p, &t, zetaLayers);
            tiledCacheSetSize(p, d.k_.info(), tiledCacheTileSize(t.bpp * std::max<uint32_t>(t.samples, 1u)),
                              cb_->tiledCacheSize_);
            zetaClearValues(p, bits & 1, zs.depth, bits & 2, zs.stencil);
            if (bits & 1)
                d.clearZbcDepth(t, true, zs.depth);
            clearZeta(p, r, bits, 0, zetaLayers);
            cb_->tiledCacheDirty_ = true;
            own = true;
            return true;
        };
        if (separate_.stencil) {
            if (separate_.depth && separate_.storeDepth && zs.clearDepth && zetaHasDepth(separate_.depth->format))
                ownZetaClear(*separate_.depth, 1);
            if (separate_.storeStencil && zs.clearStencil && zetaHasStencil(separate_.stencil->format))
                ownZetaClear(*separate_.stencil, 2);
        } else if (zs.texture && !zetaShadowOf_ && zs.stored) {
            const uint32_t bits = zetaClearBits(zs.clearDepth && zetaHasDepth(zs.texture->format),
                                                zs.clearStencil && zetaHasStencil(zs.texture->format));
            // a zcull candidate's depth is cleared again over the render area once its region is set, which that
            // clear initialises (zcull_planes.h zcullBegin)
            if (ownZetaClear(*zs.texture, bits))
                ownZetaCleared = zcull_.candidate ? bits & ~1u : bits;
        }
        if (zetaLayout)
            sampleWords(p, samples_, d.root_.va, pass_.positions, rot_);
    }
    if (own) {
        if (flush) {
            tiledCacheDrain(p, d.k_.info());
            cb_->tiledCacheDirty_ = false;
        }
        kit(p, switch_gm20b_pass_drain, false);
    }
#if SWITCH_METAL_DIAGNOSTICS
    nvnKnob(p);
#endif
    uint32_t count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    for (uint32_t j = 0; j < count; j++) {
        if (const Texture *t = pass_.color[j].texture)
            colorTarget(p, j, *t, layers);
        else
            disabledTarget(p, j, layers);
    }
    // targets map to themselves (NVK writes all eight slots of the map; the hardware reads the first `count`), drawn from
    // (0, 0) over the render area
    kit(p, switch_gm20b_render_area, count, planeArea().x1, planeArea().y1);
    zetaTargetWords(p, zs.texture, layers);
    uint32_t totalBpp = 0;
    for (uint32_t j = 0; j < count; j++)
        if (const Texture *t = pass_.color[j].texture)
            totalBpp += t->bpp * std::max<uint32_t>(t->samples, 1u);
    if (const Texture *z = zs.texture)
        totalBpp += z->bpp * std::max<uint32_t>(z->samples, 1u);
    tiledCacheSetSize(p, d.k_.info(), tiledCacheTileSize(totalBpp), cb_->tiledCacheSize_);
    // ahead of the depth clear, which sets the region, or the load of a stored one; over the plane the pass renders (a
    // turned drawable's own axes, zcullBegin)
    bool zcullAfterTir = false;
#if SWITCH_METAL_DIAGNOSTICS
    zcullAfterTir = zcull_.candidate && (tirProbe_ & kTirProbeZcullAfter);
#endif
    const auto zcullWords = [&] {
        uint32_t criterion = 0, regionFormat = 0, zFormat = ~0u;
#if SWITCH_METAL_DIAGNOSTICS
        if (const uint32_t c = d.probeZcullCriterion.load(std::memory_order_relaxed))
            criterion = zcullCriterion(c & 0xff, c >> 8 & 0xff, c >> 16 & 0xff);
        nvnZcullFormats(p, zs, regionFormat, zFormat);
#endif
        zcullRegionWords(p, zcull_.region, zs.texture->format, zcull_.dir, zcull_.storage, zcull_.storageBytes,
                         criterion, regionFormat, zFormat);
        if (zcull_.load)
            p.m(kLoadZcull, 0);
        SWITCH_METAL_DIAGNOSTIC(cb_->zcullCounts_.passes++; cb_->zcullCounts_.loads += zcull_.load);
    };
    if (zcull_.candidate && !zcullAfterTir)
        zcullWords();
    else if (!zcull_.candidate && d.zcullOn_)
        zcullOffWords(p);
    // the backing's layers past a shorter zeta's own, which no copy fills, hold what an M4 Max tests there
    // (m2v-pass-forms/probe/layerprobe.mm): a loaded depth above 1.0 (a float zeta's 2.0) or Depth16Unorm's 1.0, a loaded
    // stencil 0xff; a DontCare aspect its clear value, as Apple's DontCare depth and stencil test against
    bool cleared = false;
    if (const uint32_t own = zetaShadowOf_ ? std::min(std::max(zetaShadowOf_->layers, 1u), layers) : layers;
        own < layers) {
        const bool depth = zetaHasDepth(zs.texture->format) && !zs.clearDepth,
                   stencil = zetaHasStencil(zs.texture->format) && !zs.clearStencil;
        constexpr uint32_t kDepth16Unorm = 250;
        zetaClearValues(p, depth,
                        !zs.loaded                            ? zs.depth
                        : zs.texture->format == kDepth16Unorm ? 1.0f
                                                              : 2.0f,
                        stencil, zs.loaded ? 0xffu : zs.stencil);
        if (depth || stencil) {
            clearZeta(p, planeArea(), zetaClearBits(depth, stencil), own, layers - own);
            cb_->tiledCacheDirty_ = true;
            cleared = true;
        }
    }
    // NVK clears depth and stencil with the first color the pass clears over its render area, else on their own
    uint32_t zsBits = zs.texture ? zetaClearBits(zs.clearDepth && zetaHasDepth(zs.texture->format),
                                                 zs.clearStencil && zetaHasStencil(zs.texture->format)) &
                                       ~ownZetaCleared
                                 : 0;
    zetaClearValues(p, zsBits & 1, zs.depth, zsBits & 2, zs.stencil);
    if (zsBits & 1)
        d.clearZbcDepth(*zs.texture, areaW_ >= zs.texture->width && areaH_ >= zs.texture->height, zs.depth);
    // A mixed pass's colour is cleared with its zeta deselected and target-independent rasterisation off: under the mode,
    // beside the four-sample zeta, its CLEAR_SURFACE is class error 0x124 (INVALID_TIR_COMBINATION02, gv100
    // dev_graphics.ref.txt; gpu.gm20b.mixed_samples on the device). The zeta is cleared in its own sample layout after.
    const bool mixed = rasterSamples_ > samples_;
    if (mixed)
        p.m(ZT_SELECT, 0);
    for (const Clear &c : cl) {
        if (c.own)
            continue;
        uint32_t v[4] = {};
        const Texture *t = pass_.color[c.target].texture;
        clearWords(t->format, c.value, v, !called_ && t->compressed);
        const Rect r = planeRect(c.x0, c.y0, c.x1, c.y1);
        d.clearZbc(*t, !r.x0 && !r.y0 && r.x1 >= t->width && r.y1 >= t->height, v);
        const bool area = !mixed && !c.x0 && !c.y0 && c.x1 == areaW_ && c.y1 == areaH_;
        clearRect(p, r.x0, r.y0, r.x1, r.y1, v, c.target, area ? zsBits : 0, layers);
        cb_->tiledCacheDirty_ = true;
        cleared = true;
        zsBits = area ? 0 : zsBits;
    }
    if (mixed) {
        p.m(ZT_SELECT, 1);
        sampleWords(p, rasterSamples_, d.root_.va, pass_.positions, rot_);
        if (zsBits) {
            clearZeta(p, planeArea(), zsBits, 0, layers);
            cb_->tiledCacheDirty_ = true;
            cleared = true;
        }
        zsBits = 0;
        tirOn(p, rasterSamples_);
#if SWITCH_METAL_DIAGNOSTICS
        if (tirProbe_ & kTirProbeModeOff)
            p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
#endif
        if (zcullAfterTir)
            zcullWords();
    }
    if (zsBits) {
        clearZeta(p, planeArea(), zsBits, 0, layers);
        cb_->tiledCacheDirty_ = true;
        cleared = true;
    }
    if (drew_) {
        cb_->tiledCacheDirty_ = !flush;
    } else if (cleared && flush) {
        tiledCacheDrain(p, d.k_.info());
        cb_->tiledCacheDirty_ = false;
    }
    if (measured)
        emitDrawStamp(p, d.stamps_.va, candidate_.slot, false, candidate_.payload);
    SWITCH_METAL_PROFD(markSpan(p, prologueMark_, SpanKind::Prologue));
    SWITCH_METAL_DIAGNOSTIC(if (cb_->drawTimes_) d.emitDrawTime(p, prologueTime_));
    checkRoom(p, "a pass prologue");
    cb_->segment(start, p.at);
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - d.push_.bo.cpu);
    SWITCH_METAL_ENCODE_END(prologueSpan);
    [[maybe_unused]] const uint64_t bodyWords = appendBodies(cb_->segments_, bodies_, [&d](const uint32_t *word) {
        return d.pushVa(word);
    });
    SWITCH_METAL_DIAGNOSTIC(d.stats.pushBytes += (bodyWords + uint64_t(p.at - start)) * 4);
    bodies_.clear();
    // shadow targets the pass stores: their pool images back over what it rendered (the render area; a Clear's reach past it),
    // after the 3D engine is done with them
    bool stores = zetaShadowOf_ && pass_.depthStencil.stored;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        stores |= shadowOf_[j] && pass_.color[j].store;
    if (stores) {
        const uint32_t room2 = 8 + 8 * 32 * layers + (zetaShadowOf_ ? zetaShadowWords(layers) : 0);
        uint32_t *at = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room2 * 4, 256, va));
        Push q{at, at + room2};
        for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
            const Texture *o = shadowOf_[j];
            if (!o || !pass_.color[j].store)
                continue;
            uint32_t w = areaW_, h = areaH_;
            for (const Clear &c : cl)
                if (c.target == j && pass_.color[j].load == ColorAttachment::Clear) {
                    w = std::max(w, c.x1);
                    h = std::max(h, c.y1);
                }
            cb_->use(*pass_.color[j].texture, false);
            cb_->use(*o, true);
            const Rect r = planeRect(0, 0, w, h);
            for (uint32_t l = 0; l < std::min(std::max(o->layers, 1u), layers); l++)
                copySides(q, sideOf(*pass_.color[j].texture, r.x0, r.y0, l), sideOf(*o, r.x0, r.y0, l), r.x1 - r.x0,
                          r.y1 - r.y0, o->bpp, d.stats);
            SWITCH_METAL_DIAGNOSTIC(d.stats.compCopies += pass_.color[j].texture->comptags);
        }
        if (zetaShadowOf_ && pass_.depthStencil.stored)
            copyZetaShadow(q, false);
        checkRoom(q, "a pass's shadow stores");
        cb_->segment(at, q.at);
        cb_->lastEngine_ = BarrierEngine::Copy;
        cb_->passShaderFlushed_ = false;
        d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(q.at) - d.push_.bo.cpu);
        SWITCH_METAL_DIAGNOSTIC(d.stats.pushBytes += uint64_t(q.at - at) * 4);
    }
    if (const uint8_t aspects =
            (separate_.storeDepth ? kShaderTestDepth : 0) | (separate_.storeStencil ? kShaderTestStencil : 0);
        separate_.stencil && aspects) {
        const uint32_t room3 = 64 + 128 * std::max(pass_.layers, 1u);
        uint32_t *at = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room3 * 4, 256, va));
        Push q{at, at + room3};
        copySeparateZeta(q, aspects, false);
        checkRoom(q, "a pass's separate depth and stencil stores");
        cb_->segment(at, q.at);
        cb_->lastEngine_ = BarrierEngine::Copy;
        cb_->passShaderFlushed_ = false;
        d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(q.at) - d.push_.bo.cpu);
        SWITCH_METAL_DIAGNOSTIC(d.stats.pushBytes += uint64_t(q.at - at) * 4);
    }
}

}  // namespace gm20b
