// SPDX-License-Identifier: MIT
// See render_encoder.h (RenderEncoder::prepare: a draw's state, set 0 and barriers).
#include "lock.h"
#include "nonfinite.h"
#include "clear_box.h"
#include "icb.h"
#include "dynamic_push.h"
#include "bound_texture.h"
#include "gpu.h"
#include "layers.h"
#include "kit.h"
#include "mme.h"
#include "cbuf.h"
#include "residual.h"
#include "sampler_map.h"
#include "mixed_samples.h"
#include "multisample.h"
#include "rotated_pass.h"
#include "fragment_tests.h"
#include "shader_tests.h"
#include "shader_test_push.h"
#include "srgb_table.h"
#include "storage.h"
#include "tiled_cache.h"
#include "view.h"
#include "xr_pass.h"
#include "written_textures.h"

#include <mach/mach_time.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
// The viewport's words (scale X Y Z, offset X Y Z as floats' bits, then each clip's x | width << 16) as the kit's state
switch_gm20b_viewport_state viewportState(const uint32_t w[8])
{
    switch_gm20b_viewport_state v = {};
    memcpy(v.scale, w, sizeof v.scale);
    memcpy(v.offset, w + 3, sizeof v.offset);
    v.clip_x = uint16_t(w[6]);
    v.clip_width = uint16_t(w[6] >> 16);
    v.clip_y = uint16_t(w[7]);
    v.clip_height = uint16_t(w[7] >> 16);
    return v;
}

constexpr uint32_t kRtLayerFromShader = NVDEF(NVB197, SET_RT_LAYER, CONTROL, GEOMETRY_SHADER_SELECTS_LAYER);

// Whether a draw may store into `t` through a record whose programs were compiled for `compiled`: its own format, or an
// RGB10A8_2P_XR10 (_sRGB) texel buffer over a surface's plane, held as BGR10_XR's (BGR10_XR_sRGB's) words, which its
// format's programs store alone
bool storesAs(const Texture &t, uint32_t compiled)
{
    constexpr uint32_t kBgr10Xr = 554, kBgr10XrSrgb = 555;
    return t.format == compiled || (t.type == kTexelBufferType && twoPlaneXr(compiled) &&
                                    t.format == (compiled == kRgb10A8XrSrgb ? kBgr10XrSrgb : kBgr10Xr));
}
}  // namespace

uint32_t nvTopology(uint32_t mtl)
{
    return icb::topology(mtl);
}

// Viewports and scissors 1 .. count - 1 as NVK writes viewport j (nvk_emit_viewport, nvk_emit_scissor), through a turned
// pass's transform and its clip-space swizzle at each index (index 0's is the pass's prologue's)
void RenderEncoder::emitViewports(Push &p)
{
    for (uint32_t dirty = views_.dirtyViewports; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        switch_gm20b_viewport_state v = viewportState(views_.viewport[j]);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, j, &v);
        if (rot_.turned() && !(views_.swizzled >> j & 1)) {
            kit(p, switch_gm20b_viewport_swizzle, j, rot_.swizzle());
            views_.swizzled |= 1u << j;
        }
    }
    for (uint32_t dirty = views_.dirtyScissors; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        const ScissorBox &b = views_.scissor[j];
        const Rect s = planeRect(b.x0, b.y0, b.x1, b.y1);
        kit(p, switch_gm20b_scissor, j, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
    }
    views_.dirtyViewports = views_.dirtyScissors = 0;
}

// The colour targets the pass binds: through its last attachment (CommandBuffer::render's targets)
uint32_t RenderEncoder::colorTargets() const
{
    uint32_t count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    return count;
}

#if SWITCH_METAL_DIAGNOSTICS
static void probeUniforms(Pipeline &pl, uint32_t stage, uint32_t slot, const uint8_t *bytes, uint64_t length)
{
    const NonFiniteUniforms found = nonFiniteUniforms(bytes, length);
    if (!found.words && (!found.halves || pl.uniformHalvesReported))
        return;
    char hex[3 * 32 + 1] = "";
    const uint64_t from = found.first & ~uint64_t(15);
    for (uint64_t at = from, n = 0; at < length && n < 32; ++at, ++n)
        snprintf(hex + 3 * n, 4, "%02x ", bytes[at]);
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
           "SwitchMetal: uniform probe %{public}s stage %u slot %u: %u non-finite floats and %u halves in %llu bytes, "
           "first at "
           "+%llu; "
           "+%llu: %{public}s(mach %llu)",
           pl.uniformProbe, stage, slot, found.words, found.halves, (unsigned long long)length,
           (unsigned long long)found.first, (unsigned long long)from, hex, (unsigned long long)mach_absolute_time());
    if (found.words)
        pl.uniformProbe = nullptr;
    else
        pl.uniformHalvesReported = true;
}
#endif

// Set-0 entry `e` of `pl` into `set`, its buffer's bind residual into `prepared`; false: the draw is refused (`why`)
__attribute__((always_inline)) inline bool RenderEncoder::writeSetEntry(Pipeline *pl, const PackEntry &e, uint32_t *set,
                                                                        SetResiduals &prepared, const char **why)
{
    Device &d = *dev_;
    const PackPipeline &r = *pl->rec;
    uint32_t *at = set + e.offset / 4;
    if (e.kind == EntrySpillTable)
        return true;
    if (e.kind == EntryColorTable) {
        at[0] = uint32_t(d.colorTable_.va);
        at[1] = uint32_t(d.colorTable_.va >> 32);
        at[2] = kColorTableBytes;
        at[3] = 0;
        return true;
    }
    if (e.kind == EntryTextureTable || e.kind == EntrySamplerTable) {
        const bool textures = e.kind == EntryTextureTable;
        const uint64_t va = textures ? d.heapTables_.textureTableVa() : d.heapTables_.samplerTableVa();
        at[0] = uint32_t(va);
        at[1] = uint32_t(va >> 32);
        at[2] = textures ? d.heapTables_.textureTableBytes() : d.heapTables_.samplerTableBytes();
        at[3] = 0;
        return true;
    }
    if (e.kind == EntryUbo || e.kind == EntryTessBuffer) {
        const Bound b = e.kind == EntryUbo ? bufs_[e.stage][e.slot] : tessBuffer(e.slot);
        if (bufferStorage(e))
            cb_->bufferWrites_.note(b.va, b.size);
        if (b.buffer)
            cb_->use(*b.buffer, bufferStorage(e));
        else if (!b.named || bufferStorage(e))
            cb_->useAddress(b.va, b.size, bufferStorage(e));
        // A slot whose record names no residual word is one its programs read through the root table's address, never
        // its descriptor: the translation gives each slot it reads through a view its SlotBindOffset residual
        // (translate/NativeMetalDirectWrites.rs residual_words; a slot read by address has none), and every record is
        // the direct compile's, which places them. That descriptor takes the base aligned down.
        BufferDescriptor desc;
        if (!bufferDescriptor(e, b.va, b.size, desc))
            desc.residual = 0;
        // a word that held a residual takes the slot's next one even where it is 0: the set keeps its
        // words from the draws before
        if (const int word = bufferResidualWord(e); spilledWord(word)) {
            prepared.spilled[e.stage == 1 ? 0 : 1].set(word, desc.residual);
        } else if (word >= 0 && (desc.residual || (prepared.residualMask >> word & 1))) {
            prepared.residuals[word] = desc.residual;
            prepared.residualMask |= 1u << word;
        }
        if (b.buffer && b.buffer->hostVisible && !b.buffer->wc)
            cb_->clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
#if SWITCH_METAL_DIAGNOSTICS
        if (pl->uniformProbe && b.cpu)
            probeUniforms(*pl, e.stage, e.slot, b.cpu, std::min<uint64_t>(desc.range - desc.residual, b.size));
#endif
        // a slot bound to nothing reads the zero page: no buffer load is bounds-checked (nak_target.h)
        const uint64_t base = b.va ? desc.base : d.zero_.va;
        at[0] = uint32_t(base);
        at[1] = uint32_t(base >> 32);
        at[2] = b.va ? desc.range : 0;
        at[3] = 0;
        return true;
    }
    SWITCH_METAL_ENCODE_SPAN(Textures);
    uint32_t tic = 0, tsc = 0;
    if (e.kind == EntryStencilTest && shadowStencil(r.shaderTests) && !(r.shaderTests.flags & kShaderTestStencil)) {
        tic = 0;  // a shader-side stencil's program in a pass with no stencil tests none (m2v's neutral)
    } else if (e.kind == EntryDepthTest || e.kind == EntryStencilTest) {
        const bool depth = e.kind == EntryDepthTest;
        const Texture *t = depth ? shaderDepth_ : shaderStencil_;
        const uint32_t view = depth ? r.shaderTests.depthTic : r.shaderTests.stencilTic;
        if (t && layeredRecord(r.arrayLengthWord) && pass_.layers > 1) {
            AttachmentView layers;
            layers.word0 = view;
            layers.layers = pass_.layers;
            layers.arrayed = true;
            tic = texelBytes(view) == t->bpp ? passViewTic(*t, layers, why) : 0;
        } else if (t && texelBytes(view) == t->bpp) {
            tic = passViewTic(*t, view);
        }
        if (!t || t->samples != samples_) {
            *why = "a shader depth/stencil test's stand-in missing or of other samples than the pass's";
            cb_->refusal = {RefTestsInvariant, e.kind, t ? t->format : 0, samples_};
            return false;
        }
        if (!tic) {
            *why = "no texture header left for a shader depth/stencil test's stand-in view";
            cb_->refusal = {RefTestsMemory, e.kind, t->format, samples_};
            return false;
        }
        cb_->use(*t, true);
    } else if (e.kind == EntryTexture) {
        const Texture *t = boundTexture(e);
        if (t && !t->tic) {
            *why = "a sampled texture with no header: memoryless, or made without MTLTextureUsageShaderRead";
            cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
            return false;
        }
        // a two-plane texture's header pair where the program reads it so, else its stand-in
        const bool xrSlot = e.stage == 2 && xrPairSlot(pl->xrSlots, e.slot);
        const bool pair = t && t->xrPlanes && xrSlot;
        if (!pair && xrSlot) {
            *why = "a state that reads a two-plane texture's header pair drawn without one";
            cb_->refusal = {RefNoTic, e.slot, t ? t->format : 0, t ? t->width : 0};
            return false;
        }
        const bool written = handleStage(e) == 1 && writtenSlot(pl->writtenSlots, e.slot);
        const Texture *image = written && t && t->xrPlanes ? pass_.writtenImage(*t) : nullptr;
        if (written && t && t->xrPlanes && !image) {
            *why = "a written RGB10A8_2P_XR10 texture held in its two planes with no image";
            cb_->refusal = {RefWrittenPlanes, e.slot};
            return false;
        }
        if (written && t && !storesAs(*t, pl->rec->writtenFormat[e.slot])) {
            *why = "a written texture of another format than its program stores";
            cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
            return false;
        }
        if (t && !written && !pair && !drawsBound(*t) && !pipeline_->own) {
            *why = "a sampled texture whose codes a draw reads through a stand-in, bound with none";
            cb_->refusal = {RefNoStandIn, e.slot, t->format};
            return false;
        }
        // A written texture is stored through tic: its own memory, or the sampled shadow that stands for
        // rows its header cannot address, which the frontend copies back after the pass; its stand-in is
        // drawn again from the codes before work after the pass samples it (TegraGM20BMetalEncode.mm)
        if (pair) {
            for (const Texture *plane : {t->luma, t->chroma})
                cb_->use(plane->sampled ? *plane->sampled : *plane, false);
            SWITCH_METAL_DIAGNOSTIC(dev_->stats.xrPairBinds++);
        } else if (image) {
            cb_->use(*image, true);
        } else if (t && written) {
            cb_->use(t->sampled ? *t->sampled : *t, true);
        } else if (t) {
            cb_->use(t->standIn ? *t->standIn : t->sampled ? *t->sampled : *t, false);
        }
        tic = !t ? 0 : image ? image->tic : pair ? t->tic : t->standIn && !written ? t->standIn->tic : t->tic;
    } else if (e.kind == EntryFetch) {
        const ColorAttachment &a = pass_.color[e.slot];
        const Texture *t = a.texture;
        const bool storage = t && (pl->storageBits >> e.slot & 1);
        if (const StorageFallback mismatch =
                storage ? storageViewMismatch(texelBytes(pl->storageTic[e.slot]), pl->rec->storageSamples, *t,
                                              dev_->storageCompressedOk())
                        : StorageFallback::None;
            mismatch != StorageFallback::None) {
            const bool samples = mismatch == StorageFallback::Samples;
            *why = samples ? "a fetching state's storage form drawn into an attachment of another sample count"
                           : "a storage form whose view's texel size is not its attachment's";
            cb_->refusal = {samples ? RefStorageSamples : RefStorageView, e.slot, t->format,
                            samples ? uint32_t(t->samples) : t->width};
            return false;
        }
        if (t) {
            cb_->use(*t, pl->storageWriteBits >> e.slot & 1);
            tic = fetchTic(a, storage ? pl->storageTic[e.slot] : 0, layeredRecord(pl->rec->arrayLengthWord), why);
            if (!tic) {
                cb_->refusal = {RefViewHeader, e.slot, t->format, t->width};
                return false;
            }
        }
    } else if (e.kind == EntryTexture3d) {
        // a texture of another type there is no binding of the argument: m2v writes its neutral, raw an empty handle
        const Texture *bound = boundTexture(e);
        const Texture *t = bound && bound->type == fx::kTexThreeD ? bound : nullptr;
        if (t && !t->tic) {
            *why = "a sampled texture with no header: memoryless, or made without MTLTextureUsageShaderRead";
            cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
            return false;
        }
        const bool written = handleStage(e) == 1 && writtenSlot(pl->writtenSlots, e.slot);
        if (written && t && t->format != pl->rec->writtenFormat[e.slot]) {
            *why = "a written texture of another format than its program stores";
            cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
            return false;
        }
        if (t)
            cb_->use(*t, written);
        tic = t ? t->tic : 0;
    }
    const Texture *mapped = e.kind == EntryTexture ? boundTexture(e) : nullptr;
    const uint8_t map = !mapped ? 0 : mapped->levelZero ? kLevelZeroMap : mapped->addressMap;
    if (e.samplerKind == SamplerSlot) {
        const Sampler *s = boundSampler(e);
        const bool compares = entryCompares(e);
        tsc = s          ? (map        ? (compares ? s->compareMapped[map] : s->mapped[map])
                            : compares ? s->compareTsc
                                       : borderedTsc(*s, mapped))
              : compares ? d.neutralCompareTsc_
                         : d.neutralTsc_;
        if (s && !tsc) {
            *why = map == kLevelZeroMap ? "a sampler without its header for a texture whose every lod reads level 0"
                   : map                ? "a planar texture's sampler without its address map"
                   : compares           ? "a sampler without its compare header"
                                        : "a sampler without its opaque border's header for the texture";
            cb_->refusal = {RefSamplerHeader, e.sampler, map, compares};
            return false;
        }
    } else if (e.samplerKind == SamplerConstexpr) {
        tsc = map ? d.constexprMapped_[e.sampler][map] : d.constexprTsc_[e.sampler];
        if (!tsc) {
            *why = map == kLevelZeroMap
                       ? "a constexpr sampler without its header for a texture whose every lod reads level 0"
                   : map ? "a planar texture's constexpr sampler without its address map"
                         : "a constexpr sampler the device placed no header of";
            cb_->refusal = {RefSamplerHeader, e.sampler, map, 2};
            return false;
        }
    }
    at[0] = tic | tsc << 20;
    return true;
}

bool RenderEncoder::prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount,
                            bool indexed, const char **why)
{
    SWITCH_METAL_ENCODE_SPAN(Prepare);
    if (pl->strideTwin && bindStridePastStream(*pl->rec))
        pl = pl->strideTwin;
    if (tessellates(*pl->rec) != (primitive == icb::kPatchPrimitive)) {
        *why = primitive == icb::kPatchPrimitive ? "patches drawn with a state of no post-tessellation vertex function"
                                                 : "primitives drawn with a tessellation state";
        cb_->refusal = {RefTessellationDraw, primitive, tessellates(*pl->rec)};
        return false;
    }
    if (const char *refused = drawnWordsRefusal(*pl->rec, cb_->refusal)) {
        *why = refused;
        return false;
    }
    // A record of a vertex function that chooses its slice writes Layer below the pass's length (arrayLengthWord) and
    // one that chooses its viewport ViewportIndex below the encoder's count (viewportCountWord); a record of one layer
    // draws layer 0, and a record made before the viewport word viewport 0
    if (const char *missing = layeredDrawMissing(pl->rec->arrayLengthWord, pass_.layers, kLayerDropped)) {
        *why = missing;
        cb_->refusal = {RefLayerDropped, pass_.layers};
        return false;
    }
    if (views_.viewports > 1 && pl->viewportIndex && !pl->rec->viewportCountWord) {
        *why = "a vertex function choosing among several viewports: its record draws viewport 0";
        cb_->refusal = {RefViewports, views_.viewports, 0, 0};
        return false;
    }
    const uint32_t count = vCount;
    const bool firstDraw = !drew_;
    drew_ = true;
    written_.setLastNonOverlappingQuad(false);
    ++passDraws_;
    readBits_ |= pl->readBits | pl->fetchBits;
    // the pixels it may write: its scissor within its viewport's pixels (m2v vk_scissors, viewport_pixels), over each
    // viewport its vertex function may choose
    uint32_t x[2], y[2];
    viewportPixels(vp_, {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]}, areaW_, areaH_, x, y);
    for (uint32_t j = 1; pl->viewportIndex && j < views_.viewports; j++) {
        uint32_t xj[2], yj[2];
        viewportPixels(views_.vp[j], j < views_.scissors ? views_.scissor[j] : ScissorBox{0, 0, areaW_, areaH_}, areaW_,
                       areaH_, xj, yj);
        if (xj[1] <= xj[0] || yj[1] <= yj[0])
            continue;
        if (x[1] <= x[0] || y[1] <= y[0]) {
            x[0] = xj[0], x[1] = xj[1], y[0] = yj[0], y[1] = yj[1];
            continue;
        }
        x[0] = std::min(x[0], xj[0]), x[1] = std::max(x[1], xj[1]);
        y[0] = std::min(y[0], yj[0]), y[1] = std::max(y[1], yj[1]);
    }
    const uint32_t facts = bindingFacts(*pl);
    Pipeline *prepared = dev_->variant(*pl, facts | (rot_.turned() ? AssumeTurned : 0u));
    SWITCH_METAL_ENCODE_BEGIN(geometrySpan, Geometry);
    DrawGeometry g;
    if (firstDraw || pl->writeBits || (pl->fetchBits & ~pl->storageBits))
        g = geometry(*pl, primitive, first, indexed ? drawIndexCount_ : count, {x[0], y[0], x[1], y[1]});
    else
        g.limit = {x[0], y[0], x[1], y[1]};
    auto geometryAnalysis = geometryCache_.analysis(g, bindingRevision_, cb_->geometryBudget_, &cb_->geometryPool_);
    SWITCH_METAL_ENCODE_BEGIN(variantSpan, Variant);
    // a turned draw of a turned variant's state takes the variant over the generic program's split twin
    Pipeline *twin = indirectGeometry_ || (rot_.turned() && pl->turnedVariant) ? pl : splitTwin(pl, geometryAnalysis);
    const Pipeline *launchOnly = nullptr;
    bool generic = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (twin->splitOf && cb_->splitProbe_ == SplitProbe::LaunchOnly)
        launchOnly = twin;
    generic = twin->splitOf && (cb_->splitProbe_ == SplitProbe::Generic || launchOnly);
#endif
    if (!generic)
        pl = twin;
    // a variant has no split of its own: a draw large enough for the generic program's split twin keeps the twin
    if (rot_.turned() && pl->turnedSource) {
        Pipeline *turned = __atomic_load_n(&pl->turnedTwin, __ATOMIC_ACQUIRE);
        if (!turned && !(turned = dev_->makeTurned(*pl))) {
            *why = rotatedRefusalText(RotatedRefusal::Variant);
            cb_->refusal = {RefRotated, uint32_t(RotatedRefusal::Variant), rot_.turn};
            return false;
        }
        pl = turned;
    } else if (!pl->splitOf && !generic)
        pl = prepared;
    SWITCH_METAL_ENCODE_END(variantSpan);
    if (!whole_) {
        if (x[1] > x[0] && y[1] > y[0]) {
            const std::array<uint32_t, 4> r = {x[0], y[0], x[1], y[1]};
            if (std::find(rects_.begin(), rects_.end(), r) == rects_.end()) {
                rects_.push_back(r);
                growReached(reached_, {r[0], r[1], r[2], r[3]});
            }
            if (rects_.size() > 1024) {
                whole_ = true;
                rects_.clear();
            }
        }
    }
    SWITCH_METAL_ENCODE_END(geometrySpan);
    const Texture *zeta = pass_.depthStencil.texture;
    if (compatible_ != pl) {
        for (uint32_t j = 0; j < 8; j++) {  // the pass's attachments are the formats the state was made for
            const Texture *t = j < pass_.count ? pass_.color[j].texture : nullptr;
            const uint32_t f = t ? t->format : 0;
            if (f != pl->colorFormats[j] && !(t && pl->colorFormats[j] == 0) &&
                !(targetOrdersChannels(pl->colorFormats[j], f) && !(pl->storageBits >> j & 1))) {
                *why = "a state whose color formats differ from the pass's: the draw skipped";
                cb_->skipped = {RefColorFormats, j, f, pl->colorFormats[j]};
                cb_->skips++;
                skipDraw_ = true;
                return false;
            }
        }
        if (!fragmentTestsMatch(pl->rec->storageTests, ds_, zeta ? zeta->format : 0)) {
            *why = "an ordered storage variant whose depth/stencil tests differ from the draw";
            cb_->refusal = {RefTestsInvariant, pl->rec->storageTests,
                            uint8_t(fragmentTests(ds_, zeta ? zeta->format : 0)), 0};
            return false;
        }
        // A pass that declares no attachment tests depth and stencil against its hidden one (PassDesc::hiddenZeta), which the
        // pass is given when its recorded states need it; the pipeline was made for the attachment the pass declares
        const Texture *declared = hiddenZeta_ ? nullptr : zeta;
        // Apple's GPU draws a state of another depth/stencil format than its pass's against the pass's attachment,
        // depth tested and written as the pass's format holds it (an M4 Max, validation off, states of no depth,
        // Depth16Unorm, Depth32Float, Depth32Float_Stencil8 and Stencil8 under passes of none, Depth32Float and
        // Depth32Float_Stencil8): so does the driver, the state's format choosing only its variant
        if (!zeta && testsHiddenZeta(ds_)) {
            *why = "a depth/stencil state testing a hidden attachment the pass was not given";
            cb_->refusal = {RefTestsInvariant, pl->depthFormat, declared ? declared->format : 0, ds_.active()};
            return false;
        }
        if (!samplesDrawn(pl->samples, samples_, pl->samplePasses) ||
            !sampleShadingMasks(samples_, pl->samples == samples_ ? pl->samplePasses : 1, compatibleSampleMasks_)) {
            *why = "a state whose samples a pixel or per-sample shading the pass's attachments do not take: skipped";
            cb_->skipped = {RefSamples, pl->samples, samples_, pl->samplePasses};
            cb_->skips++;
            skipDraw_ = true;
            return false;
        }
        compatibleRasterMask_ = sampleMaskFor(pl->samples, samples_);
        if (rot_.turned() && pl->turnRefused) {
            *why =
                "a state drawn into a turned plane whose record holds no turned fragment program (PipelineTurnRefused)";
            cb_->refusal = {RefTurnRefused, uint32_t(pl->rec->keyHash), uint32_t(pl->rec->keyHash >> 32), rot_.turn};
            return false;
        }
        compatible_ = pl;
    }
    if (!prepareShaderTests(*pl, why))
        return false;
    if (p_.room() < 1024 + pl->methods.size() + (cb_->extrasLive_ ? cb_->extrasLive_->restore.size() : 0) +
                        (views_.dirtyViewports | views_.dirtyScissors ? 24 * kMaxViewports : 0))
        reserve();
    Device &d = *dev_;
    Push &p = p_;
    // NVK's dynamic state is dirty at a command buffer's first draw: its primitive restart index (0 then; an index buffer bind
    // has set its own before an indexed one) is written there
    if (!cb_->drawn_) {
        cb_->drawn_ = true;
        const uint32_t restart = indexed ? cb_->restart_ : 0;
        if (!indexed && cb_->restart_ != restart) {
            p.m(DA_PRIMITIVE_RESTART_INDEX, restart);
            cb_->restart_ = restart;
        }
    }
    const bool dsChanged = dsDirty_;
    if (dsDirty_) {
        SWITCH_METAL_ENCODE_SPAN(Bind);
        const bool depth = zeta && zetaHasDepth(zeta->format), stencil = zeta && zetaHasStencil(zeta->format);
        const bool fixedStencil = stencil && !shadowStencil_, shaderTests = shaderTestsActive_ && !shadowStencil_;
        SWITCH_METAL_DIAGNOSTIC(cb_->counts_.segmentCopies += !shaderTests && !measuringInline() && !bindsBySegment() &&
                                                              dsState_->segment(depth, fixedStencil));
        if (shaderTests)
            emitDepthStencil(p, shaderDepthStencilRegs(depth), cb_->ds_);
        else if (bindDepthStencil(p, bodies_, bodyStart_,
                                  dsState_->regs(depth, fixedStencil, stencilRef_[0], stencilRef_[1]), cb_->ds_,
                                  bindsBySegment() ? dsState_->segment(depth, fixedStencil) : StateSegment())) {
            cb_->segmentBinds_++;
            SWITCH_METAL_DIAGNOSTIC(cb_->counts_.segmentBinds++);
        }
        dsDirty_ = false;
    }
    if (zcull_.candidate && !zcull_.invalidated && (dsChanged || pl != zcullProgram_)) {
        zcullProgram_ = pl;
        const bool earlyTests = pl->writesMemory[1] && zcullEarlyFragmentTests(pl->methods.data(), pl->methods.size());
        if (zcullInvalidates(zcull_, ds_, zeta && zetaHasDepth(zeta->format), zeta && zetaHasStencil(zeta->format),
                             pl->fragmentDepth, shaderTestsActive_ != 0, pl->writesMemory[1], earlyTests)) {
            p.m(kInvalidateZcull, 0);
            SWITCH_METAL_DIAGNOSTIC(d.stats.zcullInvalidations++);
        }
    }
    uint32_t *root = cb_->rootPush_;
    const PackPipeline &r = *pl->rec;
    // set 0 first (it may refuse the draw before anything is written): the layout's entries only
    if (!pl->ubosNamed) {
        *why = "a program reads a set-0 uniform buffer its record does not name";
        cb_->refusal = {RefUnnamedUniform};
        return false;
    }
    if (__atomic_load_n(&pl->heapMissing, __ATOMIC_ACQUIRE) && !dev_->rebindArguments(*pl)) {
        *why = "no memory for the tables a state reads argument buffers through";
        cb_->refusal = {RefHeapMemory};
        return false;
    }
    const SetEntries &entries = pl->entries;
    if ((spillEntryOf(r, 0) && !pl->spill[0]) || (spillEntryOf(r, 1) && !pl->spill[1])) {
        *why = "an argument table whose words its record does not hold";
        cb_->refusal = {RefSpillTable, spillEntryOf(r, 0) && !pl->spill[0] ? 1u : 2u};
        return false;
    }
    const uint32_t setBytes = renderSetBytes(r.setBytes);
    const auto upload = [&](const uint32_t *words) {
        SWITCH_METAL_ENCODE_COUNT(DescriptorUploads);
        uint64_t address = 0;
        uint8_t *at = d.alloc(d.upload_, setBytes, 256, address);
        memcpy(at, words, setBytes);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        SWITCH_METAL_DIAGNOSTIC(cb_->counts_.uploadBytes += setBytes);
#endif
        return address;
    };
    const uint32_t *set = nullptr;
    const SetResiduals *built = nullptr;
    uint64_t setVa = 0;
    SWITCH_METAL_ENCODE_BEGIN(descriptorSpan, Descriptors);
    if (!entries.wide() && setBytes <= kRenderSetBytes) {
        wideDescriptors_.forget();
        const auto *prepared = descriptors_.resolve(
            pl, movedEntries(entries), pl->memoryEntries,
            [&](PreparedDescriptorWords &prepared, uint32_t moved) {
                SWITCH_METAL_ENCODE_COUNT(DescriptorBuilds);
                uint32_t *set = prepared.words.data();
                for (uint32_t i = 0; i < entries.size(); i++)
                    if ((moved >> i & 1) && !writeSetEntry(pl, entries[i], set, prepared, why))
                        return false;
                fillSpills(*pl, moved, false, prepared, set);
                return true;
            },
            [&](const DescriptorWords &words) {
                return upload(words.data());
            },
            setBytes);
        if (prepared)
            set = prepared->words.data(), built = prepared, setVa = prepared->address;
    } else {
        descriptors_.leave();
        const auto *prepared = wideDescriptors_.resolve(
            pl, anyEntryMoved(entries), setBytes,
            [&](WideDescriptorWords &prepared) {
                SWITCH_METAL_ENCODE_COUNT(DescriptorBuilds);
                uint32_t *set = prepared.words.data();
                for (uint32_t i = 0; i < entries.size(); i++)
                    if (!writeSetEntry(pl, entries[i], set, prepared, why))
                        return false;
                fillSpills(*pl, 0, true, prepared, set);
                return true;
            },
            [&](const std::vector<uint32_t> &words) {
                return upload(words.data());
            });
        if (prepared)
            set = prepared->words.data(), built = prepared, setVa = prepared->address;
    }
    SWITCH_METAL_ENCODE_END(descriptorSpan);
    if (!set)
        return false;
    moved_ = MovedBindings{{}, {}, {}, false};
    spillDirty_ = false;
    const auto &residuals = built->residuals;
    const uint32_t residualMask = built->residualMask;
    prepared_ = pl;
    preparedSet_ = setVa;
    SWITCH_METAL_ENCODE_BEGIN(bindSpan, Bind);
    const Raster raster = rasterFor(primitive);
    writeRaster(p, raster);
    const bool dynamicBlend = r.halfBlendColorWord;
    const bool slotRoots = r.rootSlots[0] || r.rootSlots[1];
    const bool patches = tessellates(r);
    const bool drawnWordsDirty = ((r.amplificationIdWord || r.amplificationCountWord || r.amplificationLayerWord ||
                                   r.amplificationViewportWord) &&
                                  amplificationDirty_) ||
                                 r.pulledStride[0][1] || r.arrayLengthWord || r.viewportCountWord;
    const bool rebinds = pl != emitted_;
    if (pl != emitted_ || residualMask || residualsPushed_ || (dynamicBlend && blendColorDirty_) ||
        r.shaderTests.flags || shadowStencil(r.shaderTests) || slotRoots || (patches && tessWordsDirty_) ||
        drawnWordsDirty) {
        if (pl != emitted_) {
            blendColorDirty_ |= pl->constantBlend || dynamicBlend;
            if (cb_->extrasLive_ && cb_->extrasLive_ != pl) {
                p.raw(cb_->extrasLive_->restore.data(), uint32_t(cb_->extrasLive_->restore.size()));
                cb_->extrasLive_ = nullptr;
                cb_->heldSegment_.lose();
            }
            // the iterated blend unit refuses a draw binding more than one colour target (class error 0x59)
            const uint32_t targets = colorTargets();
            const bool narrow = pl->freeForm.rung == FetchIteratedBlend && targets > 1;
            if (narrow != narrowed_) {
                p.m(CT_SELECT, targetSelect(narrow ? 1 : targets));
                narrowed_ = narrow;
            }
            bindState(p, *pl);
            clipHeld_ = pl->viewportClip;
            clampRangeHeld_ = clampRangeHeld_ && !pl->namesViewportClipRange;
            // a vertex program's Layer selects the slice where it writes one (NVK: the last pre-rasterization stage's)
            if (const uint32_t rtLayer = pl->writesLayer ? kRtLayerFromShader : 0; rtLayer != cb_->rtLayer_) {
                p.m(NVB197_SET_RT_LAYER, rtLayer);
                cb_->rtLayer_ = rtLayer;
            }
            SWITCH_METAL_DIAGNOSTIC(cb_->counts_.blendKillBinds += pl->blendKill);
            if (!pl->restore.empty())
                cb_->extrasLive_ = pl;
        }
        // the state's push words into the root table, as m2v's vkCmdPushConstants ranges reach it through NVK: the words its
        // ranges cover (the rest keep what an earlier state wrote), the draw's residuals in theirs, one LOAD_CONSTANT_BUFFER per
        // run of words that change
        uint32_t want[32];
        memcpy(want, pl->push, sizeof want);
        if (patches) {
            controlWords(r, want);
            tessWordsDirty_ = false;
        }
        drawnWords(*pl, want);
        for (uint32_t m = residualMask; m; m &= m - 1)
            want[__builtin_ctz(m)] = residuals[__builtin_ctz(m)];
        patchBlendPush(r, blendColor_, want);
        patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, want);
        for (uint32_t k = 0; k < 2; k++)
            if (const uint8_t word = r.rootTableWord[k]; r.rootSlots[k] && word && word <= 31) {
                const uint64_t table = rootTable(r, k, pl->writesMemory[k]);
                want[word - 1] = uint32_t(table);
                want[word] = uint32_t(table >> 32);
            }
        for (int i = 0; i < 32;) {
            if (!(pl->pushMask >> i & 1) || root[i] == want[i]) {
                i++;
                continue;
            }
            int j = i;
            while (j < 32 && (pl->pushMask >> j & 1) && root[j] != want[j])
                j++;
            p.m(LOAD_CB_OFFSET, 0x700 + 4 * i);
            p.nonInc(LOAD_CB, want + i, j - i);
            memcpy(root + i, want + i, (j - i) * 4);
            i = j;
        }
        emitted_ = pl;
        residualsPushed_ = residualMask != 0;
    }
    writeBiasUnits(p, raster);
    writeAlphaControl(p, cb_->alphaHeld_, alphaWanted_, rebinds, pl->namesAlphaControl, pl->ownsAlphaControl,
                      pl->blendKill);
    if (rebinds && cb_->sampleMaskHeld_ != kSampleMaskAll)
        cb_->sampleMaskHeld_ = kSampleMaskUnknown;
    if (compatibleRasterMask_ != cb_->sampleMaskHeld_) {
        const uint32_t m = compatibleRasterMask_;
        p.inc(NVB197_SET_SAMPLE_MASK_X0_Y0, {m, m, m, m});
        cb_->sampleMaskHeld_ = m;
    }
    if (rasterSamples_ > samples_ && tirControl(pl->samples) != tirControlHeld_) {
        tirControlHeld_ = tirControl(pl->samples);
        p.m(NVB197_SET_TIR_CONTROL, tirControlHeld_);
    }
    SWITCH_METAL_ENCODE_END(bindSpan);
    if (!writeClipControl(p, *pl, raster, why))
        return false;
    if (samplePasses_ != pl->samplePasses) {
        p.m(LOAD_CB_OFFSET, kRootSampleInfo + 8);
        p.nonInc(LOAD_CB, compatibleSampleMasks_, 4);
        samplePasses_ = pl->samplePasses;
    }
    const uint32_t topo = nvTopology(primitive), restart = (primitive == 2 || primitive == 4) ? 1 : 0;
    if (topo != topology_) {
        p.m(PRIMITIVE_TOPOLOGY, topo);
        topology_ = topo;
    }
    if (restart != restart_) {
        p.m(DA_PRIMITIVE_RESTART, restart);
        restart_ = restart;
    }
    if (viewportDirty_) {
        switch_gm20b_viewport_state v = viewportState(viewport_);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, 0u, &v);
        viewportDirty_ = false;
    }
    if (scissorDirty_) {
        const Rect s = planeRect(scissor_[0] & 0xffff, scissor_[1] & 0xffff, scissor_[0] >> 16, scissor_[1] >> 16);
        kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
        scissorDirty_ = false;
    }
    if (views_.dirtyViewports | views_.dirtyScissors)
        emitViewports(p);
    // NVK's SET_BLEND_CONST_RED..ALPHA, the floats' bits, written where a draw blends with them
    if ((pl->constantBlend || dynamicBlend) && blendColorDirty_) {
        p.incArray(BLEND_CONST_RED, blendColor_, 4);
        blendColorDirty_ = false;
    }
    bindStreams(p, *pl, indexed, vFirst, count);
    bool selected = false;
    if (descriptorBinding_.changed(pl, setVa)) {
        const HeapSet *heap = pl->heapSet;
        const uint64_t heapVa = heap ? heap->memory.va : 0;
        // NVK's root table sets[0] and sets[1]: set 0, and set 1 for a heap array a program reads past its constant buffer
        const uint32_t s[8] = {uint32_t(setVa),        uint32_t(setVa >> 32),  setBytes, 0, uint32_t(heapVa),
                               uint32_t(heapVa >> 32), heap ? r.heapBytes : 0, 0};
        p.m(LOAD_CB_OFFSET, 0x38);
        p.nonInc(LOAD_CB, s, heap ? 8 : 4);
        if (pl->heapSlot[0] || pl->heapSlot[1] || pl->heapSlot[2] || pl->heapSlot[3]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(r.heapBytes), uint32_t(heapVa >> 32), uint32_t(heapVa)});
            for (uint32_t k = 0; k < 4; k++)
                if (pl->heapSlot[k])
                    p.m(BIND_GROUP_CB + 0x20 * kSetGroups[k], 1 | uint32_t(pl->heapSlot[k]) << 4);
            selected = true;
        }
        const bool setRead = pl->setSlot[0] || pl->setSlot[1] || pl->setSlot[2] || pl->setSlot[3];
        if (setRead) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(setBytes), uint32_t(setVa >> 32), uint32_t(setVa)});
            for (uint32_t k = 0; k < 4; k++)
                if (pl->setSlot[k])
                    p.m(BIND_GROUP_CB + 0x20 * kSetGroups[k], 1 | uint32_t(pl->setSlot[k]) << 4);
            selected = true;
        }
    }
    // NVK_MME_BIND_CBUF_DESC's bindings, read from cached CPU words rather than the WC upload.
    for (uint8_t u = 0; u < pl->uboCount; u++) {
        const Pipeline::UboCbuf &c = pl->ubos[u];
        const uint32_t *desc = set + entries[c.entry].offset / 4;
        if (!descriptorBinding_.uboMoved(u, desc))
            continue;
        if (desc[2]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(desc[2]), desc[1], desc[0]});
            p.m(BIND_GROUP_CB + 0x20 * c.group, 1 | uint32_t(c.slot) << 4);
            selected = true;
        } else {
            p.m(BIND_GROUP_CB + 0x20 * c.group, uint32_t(c.slot) << 4);
        }
    }
    if (selected)
        p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
    if (pl->splitOf)
        launchSplit(p, *pl, set);
    else if (launchOnly)
        launchSplit(p, *launchOnly, set);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb_->splitCapturing_ && twin->splitOf && !indexed)
        captureSplit(*pl, geometryAnalysis, primitive, vFirst, count);
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    countRules(*pl);
#endif
    // framebuffer fetch reads a color attachment through the texture unit: a barrier where a draw since the last one wrote
    // any color, as m2v's barrier (0106) follows any color write, whichever colors this draw fetches.
    // The barrier holds this draw's pixel shading until the earlier pixels have left the ROP; vertex work and rasterization
    // go on (NVK's for a color-output to fragment-read dependency, as NVIDIA's drivers order one)
    // A storage access in the interlock is ordered after the earlier ones by the ticket, not after the ROP's writes; the
    // ROP's accesses are ordered after neither: a barrier where a texture fetch meets a write of either kind, a storage
    // access meets a ROP write, or a ROP access (a write, or the read a blend or a partial write mask makes) meets a
    // storage write.
    // A fetch through the texture unit that meets only ROP writes needs the barrier only where its pixels meet theirs
    // (region.h); nothing bounds a storage access's pixels.
    SWITCH_METAL_ENCODE_BEGIN(barrierSpan, Barriers);
    const uint8_t textureFetchBits = uint8_t(pl->fetchBits & ~pl->storageBits);
    const AttachmentAccess access{pl->readBits, pl->writeBits, textureFetchBits, pl->storageBits};
    bool fetchedCompressed = false;
    for (uint8_t m = textureFetchBits; m; m &= m - 1) {
        const uint32_t c = __builtin_ctz(m);
        if (c < pass_.count && pass_.color[c].texture && pass_.color[c].texture->comptags) {
            fetchedCompressed = true;
            break;
        }
    }
    if (firstDraw && !indirectGeometry_ && !pl->dropsPixels && !(alphaWanted_ & kAlphaToCoverage)) {
        const uint8_t opaque = pl->writeBits & ~(pl->readBits | pl->fetchBits | pl->storageBits);
        if (opaque && geometryAnalysis.covers(primitive == 3, areaW_, areaH_))
            covered_ = opaque;
    }
    const AttachmentBarrier needed = access.after(attachmentWrites_.rop, attachmentWrites_.storage);
    if (needed != AttachmentBarrier::None) {
        if (needed == AttachmentBarrier::Pixel && !fetchedCompressed && !written_.meets(geometryAnalysis)) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetchBarriersElided++);
#endif
#ifdef SWITCH_METAL_HARNESS_MARKS
            // the host frame harness's builds only (test/build.sh): nvdec names this mark ELIDED, and eqcheck takes a
            // barrier NVK has and raw left out only where it is
            p.m(NO_OPERATION, 0x0e11de);
#endif
        } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetchBarriers++);
#endif
            if (needed == AttachmentBarrier::Shader) {
                SWITCH_METAL_DIAGNOSTIC(d.stats.storageBarriers++);
                tiledCacheDrain(p, d.k_.info());
                p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
                p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
            } else {
                fetchBarrier(p);
            }
            attachmentWrites_.synchronized(needed);
            written_.clear();
        }
    }
    const uint32_t drawCount = indexed ? drawIndexCount_ : count;
    written_.setLastNonOverlappingQuad(
        !fetchedCompressed && fetchesOwnColor(pl->fetchBits, pl->writeBits, pl->storageBits) && !indirectGeometry_ &&
        !amplifies() && raster_.fill == 0 && (!pl->viewportIndex || views_.viewports <= 1) &&
        (primitive == 3 || primitive == 4) && drawCount == (primitive == 3 ? 6u : 4u) &&
        WrittenRegion::nonOverlappingQuad(geometryAnalysis, primitive == 3));
    SWITCH_METAL_ENCODE_END(barrierSpan);
#if SWITCH_METAL_DIAGNOSTICS
    if (pipeline_ && pipeline_->caName)
        probeDraw(*pipeline_, *pl, primitive, first, count, indexed, {x[0], y[0], x[1], y[1]});
#endif
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.interlockDraws += pl->storageBits != 0);
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.rasterOrderedDraws += pl->rec && (pl->rec->flags & PipelineRasterOrdered));
#if SWITCH_METAL_DIAGNOSTICS
    if (pl->rec && pl->storageBits)
        dev_->stats.fetch.interlockKeys.note(pl->rec->keyHash);
    if (pl->rec && pl->genericState)
        dev_->stats.fetch.genericKeys.note(pl->rec->keyHash);
#endif
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.interlocked += (pl->fetchBits & pl->storageBits) != 0);
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.interlockedBlends += pl->storageBlendBits != 0);
    attachmentWrites_.rop |= pl->writeBits;
    attachmentWrites_.storage |= pl->storageWriteBits;
    passShaderWrites_ |= pl->writtenSlots != 0 || pl->writesMemory[0] || pl->writesMemory[1];
    passWritesMemory_ |= pl->writesMemory[0] || pl->writesMemory[1];
    if (pl->writeBits)
        written_.add(geometryAnalysis);
    // Reuse a bound already established for split/coverage/fetch decisions. Admission never triggers new CPU geometry
    // reads; unknown geometry uses the clipped viewport/scissor extent rather than the whole render target.
    drawBox_ = geometryAnalysis.cachedBox();
    drawPixels_ = drawBox_.empty() ? 0 : uint64_t(drawBox_.x1 - drawBox_.x0) * (drawBox_.y1 - drawBox_.y0);
    return pl->freeForm.matrixSlot == kNoUniform || matrixConstants(*pl, why);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
const char *Device::fetchBarrierName(FetchBarrier b)
{
    switch (b) {
    case FetchBarrier::PixelShader:
        return "ps";
    case FetchBarrier::WaitForIdle:
        return "wfi";
    case FetchBarrier::PixelShaderOnly:
        return "psonly";
    case FetchBarrier::None:
        return "none";
    }
    return "?";
}
#endif

void RenderEncoder::fetchBarrier(Push &p)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    switch (dev_->fetchBarrier.load(std::memory_order_relaxed)) {
    case Device::FetchBarrier::None:
        return;
    case Device::FetchBarrier::WaitForIdle:
        p.m(WAIT_FOR_IDLE, 0);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        return;
    case Device::FetchBarrier::PixelShaderOnly:
        p.m(PIXEL_SHADER_BARRIER, 0);
        return;
    case Device::FetchBarrier::PixelShader:
        break;
    }
#endif
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const Texture *t = pass_.color[j].texture;
        if (t && !shadowOf_[j] && t->storageRange().gpuVolatile) {
            kit(p, switch_gm20b_pixel_shader_barrier_sysmem);
            return;
        }
    }
    kit(p, switch_gm20b_pixel_shader_barrier);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void RenderEncoder::countRules(const Pipeline &pl)
{
    if (pl.fetchBits && anyWritten_) {
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetchBarriersAnyColor++);
        anyWritten_ = false;
    }
    anyWritten_ |= pl.writeBits != 0;
    const uint8_t stored = pl.fetchBits & pl.writeBits, ropWrites = pl.writeBits & ~pl.fetchBits;
    if ((pl.fetchBits & ropWritten_) || (ropWrites & storageWritten_)) {
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.fetchBarriersInterlocked++);
        ropWritten_ = storageWritten_ = 0;
    }
    ropWritten_ |= ropWrites;
    storageWritten_ |= stored;
}

#endif

}  // namespace gm20b
