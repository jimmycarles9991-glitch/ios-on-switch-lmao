// SPDX-License-Identifier: MIT
// See raw_reach.h.
#include "raw_reach.h"

#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include <os/log.h>

#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "raw_area_load.h"
#include "raw_blend_kill.h"
#include "raw_clear.h"
#include "raw_gpu.h"
#include "raw_record.h"
#include "raw_sample_shading.h"

namespace raw {
namespace {

// The reach masks' and reach images' scratch backings (raw_turned_pass.cpp's start at 48)
constexpr uint32_t kReachMaskTag = 72, kReachImageTag = 73;

// The registers a reach twin sets over its state's: its pixel program alone writes target 0, which nothing blends, tests or
// covers by alpha, through the conservative rasteriser
std::vector<std::pair<uint32_t, uint32_t>> reachRegisters(const PlacedProgram &fs)
{
    return {
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

}  // namespace

Pipeline *Device::reachTwin(Pipeline &pl, const char **why)
{
    if (Pipeline *made = __atomic_load_n(&pl.reachTwin, __ATOMIC_ACQUIRE))
        return made;
    if (pl.vertexStores) {
        *why = "a draw whose vertex program writes memory, which drawing its geometry again would write twice";
        return nullptr;
    }
    const Pipeline *reach = pipeline(Pack::fnv(kReachState, strlen(kReachState)));
    std::lock_guard<raw::Mutex> l(lock_);
    if (pl.reachTwin)
        return pl.reachTwin;
    PlacedProgram fs;
    if (!reach || !reach->rec || !placePackProgram(reach->rec->fs, fs)) {
        *why = "no reach state in the pack, or no heap room for its program";
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
            if (v != expected && m != NVB197_SET_PIPELINE_PROGRAM(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL) &&
                m != NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL))
                r.m(m, expected);
        }
        reachMethods_.assign(words.data(), w.at);
        reachRestore_.assign(restore.data(), r.at);
    }
    PackPipeline &rec = reachRecords_.emplace_back(*pl.rec);
    for (uint32_t i = 0; i < rec.entries; i++)
        if (PackEntry &e = rec.entry[i]; e.kind != EntryUbo || e.stage != 1) {
            e.kind = EntrySamplerOnly;
            e.samplerKind = SamplerNone;
        }
    rec.fs = reach->rec->fs;
    rec.flags = 0;
    rec.blendColorWord = rec.halfBlendColorWord = 0;
    rec.storageMask = rec.storageWriteMask = rec.storageTests = rec.storageBlendMask = 0;
    rec.shaderTests = {};
    rec.variantCount[0] = rec.variantCount[1] = 0;
    Pipeline &t = reachPipelines_.emplace_back();
    t.rec = &rec;
    t.vertexInstructions = pl.vertexInstructions;
    t.methods = pl.methods;
    t.methods.insert(t.methods.end(), reachMethods_.begin(), reachMethods_.end());
    memcpy(t.push, pl.push, sizeof t.push);
    t.pushMask = pl.pushMask;
    t.setSlot[0] = pl.setSlot[0];
    for (uint8_t u = 0; u < pl.uboCount; u++)
        if (!pl.ubos[u].group)
            t.ubos[t.uboCount++] = pl.ubos[u];
    t.ubosNamed = pl.ubosNamed;
    t.writeBits = 1;
    t.colorFormats[0] = kReachFormat;
    t.restore = pl.restore;
    t.restore.insert(t.restore.end(), reachRestore_.begin(), reachRestore_.end());
    __atomic_store_n(&pl.reachTwin, &t, __ATOMIC_RELEASE);
    return &t;
}

void RenderEncoder::takeReachTwin()
{
    if (!pipeline_ || pipeline_ == reachTwin_)
        return;
    const char *why = "no reach state in the pack";
    reachCover_ = pipeline_->vertexStores;
    Pipeline *drawn = reachCover_ ? dev_->pipeline(Pack::fnv(kReachState, strlen(kReachState))) : pipeline_;
    reachTwin_ = pipeline_ = drawn ? dev_->reachTwin(*drawn, &why) : nullptr;
    if (!pipeline_ && !reachWhy_)
        reachWhy_ = why;
    static std::atomic<bool> logged{false};
    if (reachCover_ && !logged.exchange(true))
        os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                     "raw: a rotation-4 load's draw has a vertex program that writes memory: it reaches every tile of "
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
    if (!draws.any() && !runsWithoutDraws(pass))
        return true;
    const bool every = reachesEveryTile(pass);
    const uint32_t step = draws.atFrameResolution() ? 1 : reachStep(width, height);
    const uint32_t tilesX = reachTiles(width), tilesY = reachTiles(height);
    const bool reduced = !every && step < kReachTile;
    Texture *mask = turnScratch(kReachFormat, tilesX, tilesY, kReachMaskTag);
    Texture *image = reduced ? turnScratch(kReachFormat, width / step, height / step, kReachImageTag) : mask;
    if (!mask || !image) {
        *why = "no scratch for a pass's reach";
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
        *why = "a reach target the raw path cannot render";
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
        Pipeline *reduce = dev_->pipeline(Pack::fnv(kReachReduceState, strlen(kReachReduceState)));
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

}  // namespace raw
