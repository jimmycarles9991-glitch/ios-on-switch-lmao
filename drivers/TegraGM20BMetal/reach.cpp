// SPDX-License-Identifier: MIT
// See reach.h.
#include "reach.h"

#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include <os/log.h>

#include "../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "area_load.h"
#include "blend_kill.h"
#include "clear.h"
#include "gpu.h"
#include "gm20b_gpu_internal.h"
#include "record.h"
#include "sample_shading.h"
#include "set_entries.h"
#include "stage_switches.h"

namespace gm20b {
namespace {

// The reach masks' and reach images' scratch backings (turned_pass.cpp's start at 48)
constexpr uint32_t kReachMaskTag = 72, kReachImageTag = 73;
constexpr uint8_t kVertexStage = 1;

// The registers a reach twin sets over its state's: its pixel program alone writes target 0, which nothing blends, tests or
// covers by alpha, through the conservative rasteriser. The pixel stage is on for a state of no fragment function too, whose
// draws reach as discarding ones do.
std::vector<std::pair<uint32_t, uint32_t>> reachRegisters(const PlacedProgram &fs)
{
    return {
        {kPixelStageSwitch, kPixelStageOn},
        {NVB197_SET_PIPELINE_PROGRAM(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL), fs.program},
        {NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL), fs.record->gprs},
        {NVB197_SET_CT_WRITE(0), 0x1111},
        {NVB197_SET_SINGLE_CT_WRITE_CONTROL, 1},
        {NVB197_SET_BLEND(0), 0},
        {NVB197_SET_ANTI_ALIAS_ALPHA_CONTROL, 0},
        {NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL, 0},
        {NVB197_SET_HYBRID_ANTI_ALIAS_CONTROL, sampleShadingControl(1)},
        {NVB197_SET_API_MANDATED_EARLY_Z, 0},
        {kIteratedBlendOptimization, kNoopNever},
        {NVB197_SET_CONSERVATIVE_RASTER, NVB197_SET_CONSERVATIVE_RASTER_ENABLE_TRUE},
    };
}

// Registers the reach's own words set back after its draws: the pixel stage is on in the channel's context and in every state
// that names it not (stage_switches.h), so its switch never goes back to a pack baseline
bool restoredAfterReach(uint32_t m)
{
    return m != kPixelStageSwitch && m != NVB197_SET_PIPELINE_PROGRAM(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL) &&
           m != NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL);
}

// Whether a state's draws rasterise: one Metal makes with rasterization off turns the rasteriser off in its methods and
// back on in its restore (stage_switches.h), read as push words: a header's method in bits 11:0, its data words after
// it but for an immediate one's (opcode 4), which carries its value in itself
bool rasterises(const Pipeline &pl)
{
    if (!pl.stagesOff)
        return true;
    for (size_t i = 0; i < pl.restore.size(); i++) {
        const uint32_t w = pl.restore[i];
        if (((w & 0xfff) << 2) == kRasterSwitch)
            return false;
        if ((w >> 29) != 4)
            i += (w >> 16) & 0x1fff;
    }
    return true;
}

}  // namespace

// A state's reach form: its vertex and tessellation programs with every binding they read (set 0's vertex-stage entries,
// argument tables, constant buffers and stream strides), the pack's reach program in place of its pixel stage's. Made once
// a state, at its first reached draw.
Pipeline *Device::reachTwin(Pipeline &pl, const char **why)
{
    if (Pipeline *made = __atomic_load_n(&pl.reachTwin, __ATOMIC_ACQUIRE))
        return made;
    if (pl.vertexStores) {
        *why = "a draw whose vertex program writes memory, which drawing its geometry again would write twice";
        return nullptr;
    }
    const Pipeline *reach = ownPipeline(kReachState);
    Pipeline *strideReach = nullptr;
    if (pl.strideTwin && !(strideReach = reachTwin(*pl.strideTwin, why)))
        return nullptr;
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (pl.reachTwin)
        return pl.reachTwin;
    PlacedProgram fs;
    if (!reach || !reach->rec || !placePackProgram(reach->rec->fs, fs)) {
        *why = "no reach state in the pack";
        return nullptr;
    }
    if (reachMethods_.empty()) {
        const PackRules rules = PackRules::of(pack_);
        std::vector<uint32_t> words(64), restore(64);
        Push w{words.data(), words.data() + words.size()}, r{restore.data(), restore.data() + restore.size()};
        for (const auto &[m, v] : reachRegisters(fs)) {
            w.m(m, v);
            if (rules.varying.count(m))
                continue;
            const auto base = rules.baseline.find(m);
            const auto context = rules.context.find(m);
            const uint32_t expected = base != rules.baseline.end()     ? base->second
                                      : context != rules.context.end() ? context->second
                                                                       : 0;
            if (v != expected && restoredAfterReach(m))
                r.m(m, expected);
        }
        reachMethods_.assign(words.data(), w.at);
        reachRestore_.assign(restore.data(), r.at);
    }
    PackPipeline &rec = reachRecords_.emplace_back(*pl.rec);
    std::vector<uint32_t> &far = reachFar_.emplace_back();
    if (pl.entries.far())
        far.assign(pl.entries.far(), pl.entries.far() + farWords(entryCount(rec)));
    for (uint32_t i = 0; i < pl.entries.size(); i++)
        if (PackEntry &e = entryAt(rec, far, i); e.stage != kVertexStage) {
            e.kind = EntrySamplerOnly;
            e.samplerKind = SamplerNone;
        }
    rec.fs = reach->rec->fs;
    rec.flags = 0;
    rec.halfBlendColorWord = 0;
    rec.storageMask = rec.storageWriteMask = rec.storageTests = rec.storageBlendMask = 0;
    rec.shaderTests = {};
    rec.variantCount[0] = rec.variantCount[1] = 0;
    setSpillEntry(rec, 1, 0);
    Pipeline &t = reachPipelines_.emplace_back();
    t.rec = &rec;
    t.entries = SetEntries(rec, far.empty() ? nullptr : far.data());
    t.vertexInstructions = pl.vertexInstructions;
    t.writesMemory[0] = pl.writesMemory[0];
    t.writesMemory[1] = false;
    t.methods.assign(pl.methodWords(), pl.methodWords() + pl.methodCount());
    t.methods.insert(t.methods.end(), reachMethods_.begin(), reachMethods_.end());
    t.reselectRoot = pl.reselectRoot;
    t.viewportClip = pl.viewportClip;
    t.namesViewportClip = pl.namesViewportClip;
    t.namesViewportClipRange = pl.namesViewportClipRange;
    memcpy(t.push, pl.push, sizeof t.push);
    t.pushMask = pl.pushMask;
    memcpy(t.streamStride, pl.streamStride, sizeof t.streamStride);
    for (const uint32_t k : {0u, 2u, 3u}) {
        t.setSlot[k] = pl.setSlot[k];
        t.heapSlot[k] = pl.heapSlot[k];
    }
    for (uint8_t u = 0; u < pl.uboCount; u++)
        if (pl.ubos[u].group != kSetGroups[1])
            t.ubos[t.uboCount++] = pl.ubos[u];
    t.ubosNamed = pl.ubosNamed;
    t.memoryEntries = pl.memoryEntries;
    t.strideTwin = strideReach;
    t.heapSet = pl.heapSet;
    t.heapMissing = pl.heapMissing;
    t.spill[0] = pl.spill[0];
    t.writeBits = 1;
    t.colorFormats[0] = kReachFormat;
    t.restore = pl.restore;
    t.restore.insert(t.restore.end(), reachRestore_.begin(), reachRestore_.end());
    t.namesAlphaControl = t.ownsAlphaControl = true;
    __atomic_store_n(&pl.reachTwin, &t, __ATOMIC_RELEASE);
    return &t;
}

void RenderEncoder::takeReachTwin()
{
    if (!pipeline_ || pipeline_ == reachTwin_)
        return;
    if (!rasterises(*pipeline_)) {  // its draws reach nothing, its vertex stores aside
        reachTwin_ = pipeline_ = nullptr;
        reachCover_ = false;
        return;
    }
    const char *why = "no reach state in the pack";
    reachCover_ = pipeline_->vertexStores;
    Pipeline *drawn = reachCover_ ? dev_->ownPipeline(kReachState) : pipeline_;
    reachTwin_ = pipeline_ = drawn ? dev_->reachTwin(*drawn, &why) : nullptr;
    if (!pipeline_ && !reachWhy_)
        reachWhy_ = why;
    static std::atomic<bool> logged{false};
    if (reachCover_ && !logged.exchange(true, std::memory_order_relaxed))
        os_log_error(
            gm20b::log(),
            "SwitchMetal: a rotation-4 load's draw has a vertex program that writes memory: it reaches every tile of "
            "its scissor within its viewport (once per process)");
}

void RenderEncoder::beginReach(uint32_t step, uint32_t width, uint32_t height)
{
    reachStep_ = step;
    reachW_ = width;
    reachH_ = height;
    setViewport(0, 0, width, height, 0, 1);
    setScissor(0, 0, width, height);
}

bool CommandBuffer::reach(const PassDesc &pass, uint32_t width, uint32_t height, DrawReplay &draws, PassReach &out,
                          const char **why)
{
    out = {};
    const Rect area = reachArea(pass, width, height);
    if ((!draws.any() && !runsWithoutDraws(pass)) || !area.x1 || !area.y1)
        return true;
    width = area.x1;
    height = area.y1;
    const bool every = reachesEveryTile(pass);
    const uint32_t step = draws.atFrameResolution() ? 1 : reachStep(width, height);
    const uint32_t tilesX = reachTiles(width), tilesY = reachTiles(height);
    const bool reduced = !every && step < kReachTile;
    Texture *mask = turnScratch(kReachFormat, tilesX, tilesY, kReachMaskTag);
    Texture *image = reduced ? turnScratch(kReachFormat, width / step, height / step, kReachImageTag) : mask;
    if (!mask || !image) {
        *why = "no memory for a pass's reach target";
        out.memory = true;
        return false;
    }
    PassDesc q;
    q.color[0].texture = image;
    q.color[0].load = ColorAttachment::Clear;
    q.count = 1;
    if (every) {
        const double one[4] = {1, 0, 0, 0};
        integerClearSlots(kReachFormat, one, q.color[0].clear);
    }
    RenderEncoder *e = render(q);
    if (!e) {
        *why = "a reach target the driver cannot render";
        return false;
    }
    if (!every) {
        e->beginReach(step, width, height);
        if (!draws.replay(*e, why))
            return false;
        if (e->reachRefusal()) {
            *why = e->reachRefusal();
            return false;
        }
    }
    e->end();
    if (reduced) {
        Pipeline *reduce = dev_->ownPipeline(kReachReduceState);
        q.color[0].texture = mask;
        if (!reduce || !(e = render(q))) {
            *why = "no reach reduction state in the pack, or no pass for it";
            return false;
        }
        const uint32_t extent[3] = {width / step, height / step, kReachTile / step};
        e->setPipeline(reduce);
        e->setTexture(0, image);
        e->setBytes(2, 0, extent, sizeof extent);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return false;
        e->end();
    }
    out = {mask, tilesX, tilesY};
    return true;
}

}  // namespace gm20b
