// SPDX-License-Identifier: MIT
// Program variants (raw_pack.h PackVariant): a stage's programs compiled under assumptions about its draws. A draw's facts are
// worked out when its bindings change; the pipeline it draws with is the one whose programs are the first variants of each
// stage those facts hold for. That pipeline is made the first time a draw selects it, its programs placed from the pack, never
// compiled.
#include "raw_lock.h"
#include <algorithm>
#include "raw_gpu.h"
#include "raw_set_reads.h"
#include "raw_slm.h"
#include "raw_variants.h"
#if SWITCH_RAW_DIAGNOSTICS
#include <os/log.h>
#include <time.h>
#include <mutex>
#include "raw_uniform_tuples.h"
#endif

namespace raw {
namespace {

constexpr uint32_t kUniformAlignment = 256;  // raw_residual.h bufferBaseAlignment(false)

}  // namespace

// lock_ held: pack pipeline `pl` of record `pp`, just built, learns its stages' variants
void Device::offerVariants(Pipeline &pl, const PackPipeline &pp)
{
    uint32_t count[2] = {};
    for (uint32_t s = 0; s < 2; s++)
        pl.variants[s] = pack_.variants(pp, s, count[s]);
    if (!count[0] && !count[1])
        return;
    pl.variantCount[0] = uint8_t(count[0]);
    pl.variantCount[1] = uint8_t(count[1]);
    pl.turnedVariant = count[1] && (pl.variants[1][0].assume & AssumeTurned);
    pl.chosen.assign((count[0] + 1) * (count[1] + 1), nullptr);
    pl.chosen.back() = &pl;
}

// The pipeline a draw of `pl` whose facts are `facts` draws with: `pl` itself, or the one made of the variants they select
Pipeline *Device::variant(Pipeline &pl, uint32_t facts)
{
    if (pl.chosen.empty())
        return &pl;
    facts |= pack_.header()->chipFacts;
    const uint32_t v = chooseVariant(pl.variants[0], pl.variantCount[0], facts);
    const uint32_t f = chooseVariant(pl.variants[1], pl.variantCount[1], facts);
    const size_t at = v * (pl.variantCount[1] + 1u) + f;
    if (Pipeline *made = __atomic_load_n(&pl.chosen[at], __ATOMIC_ACQUIRE))
        return made;
    std::lock_guard<raw::Mutex> l(lock_);
    if (pl.chosen[at])
        return pl.chosen[at];
    const PackPipeline &pp = *pl.rec;
    const uint32_t index[2] = {v < pl.variantCount[0] ? pl.variants[0][v].program : pp.vs,
                               f < pl.variantCount[1] ? pl.variants[1][f].program : pp.fs};
    PlacedProgram placed[2];
    bool placedBoth = placePackProgram(index[0], placed[0]) && placePackProgram(index[1], placed[1]);
    if (placedBoth && !slmNeed(std::max(slmPerWarpOfHeader(placed[0].header), slmPerWarpOfHeader(placed[1].header)))) {
        stats.heapFull++;
        placedBoth = false;
    }
    if (!placedBoth) {
        __atomic_store_n(&pl.chosen[at], &pl, __ATOMIC_RELEASE);
        return &pl;
    }
    Pipeline &made = variantPipelines_.emplace_back();
    build(made, pp, placed[0], placed[1], pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString),
          pl.caTransform, pack_.spills(pp));
    for (uint32_t s = 0; s < 2; s++)
        if (index[s] != (s ? pp.fs : pp.vs))
            made.memoryEntries |= placedSetReads(placed[s].header, *placed[s].record, pp);
    made.restore = pl.restore;
    if (const PackSplit *split = pack_.split(index[1]); split && !made.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack_.section<uint32_t>(PackBlob) + pp.blob;
        source.keyString = pack_.string(pp.keyString);
        source.vs = placed[0];
        source.caTransform = pl.caTransform;
        offerSplit(made, std::move(source));
    }
    __atomic_store_n(&pl.chosen[at], &made, __ATOMIC_RELEASE);
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
    uint32_t facts = 0;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        const PackUniformRange &u = r.uniform[k];
        if (u.entry >= r.entries)
            continue;
        const PackEntry &e = r.entry[u.entry];
        if (!(bufs_[e.stage][e.slot].va & (kUniformAlignment - 1)))
            facts |= AssumeUniformAligned << k;
    }
    for (uint32_t i = 0, k = 0; i < r.entries && k < kTextureFacts; i++) {
        const PackEntry &e = r.entry[i];
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

#if SWITCH_RAW_DIAGNOSTICS
// A draw of a generic merge state: its tuple of the uniform bytes its record reads, counted in its 5 s window
// (raw_uniform_tuples.h), each window logged as it closes
void RenderEncoder::logUniformTuple(const Pipeline &pl) const
{
    if (!pl.rec)
        return;
    const PackPipeline &r = *pl.rec;
    uint64_t tuple = 0;
    const bool mapped = uniformTuple(
        r,
        [&](uint8_t stage, uint8_t slot) {
            const Bound &b = bufs_[stage][slot];
            return BoundBytes{b.cpu, b.size};
        },
        tuple);
    static std::mutex lock;
    static UniformTuples tuples;
    std::vector<UniformTuples::Window> closed;
    {
        std::lock_guard<std::mutex> l(lock);
        tuples.note(r.keyHash, mapped, tuple, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000, closed);
    }
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    for (const UniformTuples::Window &w : closed)
        os_log(log,
               "raw: uniform tuples key=%016llx in 5 s: %llu draws, %zu tuples (drawn once %llu, 2-9 times %llu, "
               "10-99 times %llu, 100 or more %llu; the most %llu draws, tuple %016llx), %llu draws of tuples past "
               "the first %zu, %llu draws of a range the CPU does not map",
               (unsigned long long)w.keyHash, (unsigned long long)w.draws, w.tuples, (unsigned long long)w.drawnOnce,
               (unsigned long long)w.drawnUnder10, (unsigned long long)w.drawnUnder100, (unsigned long long)w.drawnMore,
               (unsigned long long)w.most, (unsigned long long)w.mostTuple, (unsigned long long)w.beyond,
               UniformTuples::kTuplesPerState, (unsigned long long)w.unmapped);
}
#endif

}  // namespace raw
