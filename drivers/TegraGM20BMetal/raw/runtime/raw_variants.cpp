// SPDX-License-Identifier: MIT
// Program variants (raw_pack.h PackVariant): a stage's programs compiled under assumptions about its draws. A draw's facts are
// worked out when its bindings change; the pipeline it draws with is the one whose programs are the first variants of each
// stage those facts hold for. Compatible pipelines are placed with their state, sharing their programs in the pack heap.
#include <algorithm>
#include "raw_gpu.h"
#include "raw_set_reads.h"
#include "raw_slm.h"
#include "raw_variants.h"
#include "raw_pack_selectors.h"
#include "drivers/TegraGM20BMetal/precompile/ca-selectors/raw_ca_selector_layout.h"

namespace raw {
namespace {

constexpr uint32_t kUniformAlignment = 256;  // raw_residual.h bufferBaseAlignment(false)

}  // namespace

void RenderEncoder::setPipeline(Pipeline *p)
{
    alphaWanted_ = 0;
    if (pipeline_ == p)
        return;
    pipeline_ = p;
    if (kCaSelectorConfigs && selectorId_ == UINT16_MAX && p && p->selectorLayout()) {
        const Bound &bound = bufs_[2][kCaSelectorSlot];
        selectorId_ = bound.buffer ? 0 : caSelectorId(bound.cpu, bound.size);
    }
}

bool Device::placeRequiredVariants(const PackPipeline &pp, uint64_t &slm)
{
    for (uint32_t stage = 0; stage < 2; stage++) {
        uint32_t count = 0;
        const PackVariant *variants = pack_.variants(pp, stage, count);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t facts = variants[i].assume | pack_.header()->chipFacts;
            if (!(variants[i].assume & AssumeTurned) || chooseVariant(variants, count, facts) != i)
                continue;
            PlacedProgram placed;
            if (!placePackProgram(variants[i].program, placed))
                return false;
            slm = std::max(slm, slmPerWarpOfHeader(placed.header));
        }
    }
    return true;
}

// lock_ held: pack pipeline `pl` of record `pp`, just built, learns its stages' variants
void Device::offerVariants(Pipeline &pl, const PackPipeline &pp)
{
    uint32_t count[2] = {};
    for (uint32_t s = 0; s < 2; s++)
        pl.variants[s] = pack_.variants(pp, s, count[s]);
    const PackSelectorTable *selectors = pack_.selectors(pp);
    if (!count[0] && !count[1] && !selectors)
        return;
    pl.variantCount[0] = uint8_t(count[0]);
    pl.variantCount[1] = uint8_t(count[1]);
    pl.turnedVariant = count[1] && (pl.variants[1][0].assume & AssumeTurned);
    const uint32_t stride = (count[0] + 1) * (count[1] + 1);
    pl.selectorCount = selectors ? kCaSelectorConfigs : 0;
    pl.selectorAssume = selectors ? selectors->assume : 0;
    pl.chosen.assign(stride * (pl.selectorCount + 1u), &pl);
    for (uint32_t v = 0; v <= count[0]; v++)
        for (uint32_t f = 0; f <= count[1]; f++) {
            if (v == count[0] && f == count[1])
                continue;
            const uint32_t facts = pack_.header()->chipFacts | (v < count[0] ? pl.variants[0][v].assume : 0u) |
                                   (f < count[1] ? pl.variants[1][f].assume : 0u);
            if (chooseVariant(pl.variants[0], count[0], facts) != v ||
                chooseVariant(pl.variants[1], count[1], facts) != f)
                continue;
            pl.chosen[v * (count[1] + 1u) + f] = placeVariant(pl, {v < count[0] ? pl.variants[0][v].program : pp.vs,
                                                                   f < count[1] ? pl.variants[1][f].program : pp.fs});
        }
    for (uint32_t id = 1; id <= pl.selectorCount; id++)
        std::copy_n(pl.chosen.begin(), stride, pl.chosen.begin() + id * stride);
    if (!selectors)
        return;
    const PackSelectorConfig *configs = selectorConfigs(*selectors);
    for (uint32_t c = 0; c < selectors->configs; c++)
        for (uint32_t v = 0; v <= count[0]; v++) {
            const uint32_t facts = pack_.header()->chipFacts | (v < count[0] ? pl.variants[0][v].assume : 0u);
            if (chooseVariant(pl.variants[0], count[0], facts) != v)
                continue;
            Pipeline *made = placeVariant(pl, {v < count[0] ? pl.variants[0][v].program : pp.vs, configs[c].program});
            if (made == &pl)
                continue;
            made->selectorVariant = true;
            for (uint32_t f = 0; f <= count[1]; f++)
                pl.chosen[configs[c].id * stride + v * (count[1] + 1u) + f] = made;
        }
}

// The pipeline a draw of `pl` whose facts are `facts` draws with: `pl` itself, or the one made of the variants they select
Pipeline *Device::variant(Pipeline &pl, uint32_t facts, uint16_t selector)
{
    if (pl.chosen.empty())
        return &pl;
    facts |= pack_.header()->chipFacts;
    const uint32_t v = chooseVariant(pl.variants[0], pl.variantCount[0], facts);
    const uint32_t f = chooseVariant(pl.variants[1], pl.variantCount[1], facts);
    const uint32_t config = pl.selectorCount && !(facts & AssumeTurned) && !(pl.selectorAssume & ~facts) ? selector : 0;
    const size_t at = (config * (pl.variantCount[0] + 1u) + v) * (pl.variantCount[1] + 1u) + f;
    return pl.chosen[at];
}

Pipeline *Device::placeVariant(Pipeline &pl, const std::array<uint32_t, 2> &index)
{
    const PackPipeline &pp = *pl.rec;
    PlacedProgram placed[2] = {{}, noPixelProgram()};
    bool placedBoth =
        placePackProgram(index[0], placed[0]) && (!pixelStage(pp) || placePackProgram(index[1], placed[1]));
    if (placedBoth && !slmNeed(std::max(slmPerWarpOfHeader(placed[0].header), slmPerWarpOfHeader(placed[1].header)))) {
        stats.heapFull++;
        placedBoth = false;
    }
    if (!placedBoth)
        return &pl;
    Pipeline &made = variantPipelines_.emplace_back();
    build(made, pp, placed[0], placed[1], pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString),
          pl.caTransform, pack_.spills(pp), pack_.far(pp));
    for (uint32_t s = 0; s < 2; s++)
        if (index[s] != (s ? pp.fs : pp.vs))
            made.memoryEntries |= placedSetReads(placed[s].header, *placed[s].record, made.entries);
    made.restore = pl.restore;
    made.strideTwin = pl.strideTwin;
    if (const PackSplit *split = pixelStage(pp) ? pack_.split(index[1]) : nullptr; split && !made.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack_.section<uint32_t>(PackBlob) + pp.blob;
        source.far = pack_.far(pp);
        source.keyString = pack_.string(pp.keyString);
        source.vs = placed[0];
        source.caTransform = pl.caTransform;
        offerSplit(made, std::move(source));
    }
    return &made;
}

// lock_ held: pack program `index` placed in the heap; false: the heap is full
bool Device::placePackProgram(uint32_t index, PlacedProgram &out)
{
    if (!place(index, out.program, out.data)) {
        stats.heapFull++;
        return false;
    }
    const PackProgram *programs = pack_.section<PackProgram>(PackPrograms);
    out.record = &programs[index];
    out.header = pack_.section<uint8_t>(PackHeap) + programs[index].program;
    out.farUbos = pack_.farUbos(programs[index]);
    return true;
}

// The facts of the draw's bindings that `pl`'s variants may assume (raw_pack.h PackAssumption bits 0-15), worked out again only
// when a binding changed
uint32_t RenderEncoder::bindingFacts(const Pipeline &pl)
{
    if (pl.chosen.empty())
        return 0;
    if (factsPipeline_ == &pl && factsRevision_ == bindingRevision_)
        return facts_;
    const PackPipeline &r = *pl.rec;
    const SetEntries &entries = pl.entries;
    uint32_t facts = 0;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        const PackUniformRange &u = r.uniform[k];
        if (u.entry >= entries.size())
            continue;
        const PackEntry &e = entries[u.entry];
        if (!(bufs_[e.stage][e.slot].va & (kUniformAlignment - 1)))
            facts |= AssumeUniformAligned << k;
    }
    for (uint32_t i = 0, k = 0; i < entries.size() && k < kTextureFacts; i++) {
        const PackEntry &e = entries[i];
        if (e.kind != EntryTexture)
            continue;
        const Texture *t = boundTexture(e);
        const Texture *sampled = t && t->standIn ? t->standIn : t && t->sampled ? t->sampled : t;
        if (sampled && fixedPointTexels(sampled->format))
            facts |= AssumeTextureUnorm << k;
        k++;
    }
    factsPipeline_ = &pl;
    factsRevision_ = bindingRevision_;
    facts_ = facts;
    return facts;
}

}  // namespace raw
