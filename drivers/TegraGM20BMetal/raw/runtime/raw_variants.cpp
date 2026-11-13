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
#include <mutex>
#include <unordered_map>
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
// For a pack pipeline whose fragment program leaves 20 warps per SM or fewer (more than 96 registers: Core Animation's
// generic Xghfc states, whose branches test uniform words), each distinct tuple of its uniform buffers' bytes, its bytes
// at its first draw and its count at the 10th, 100th...: what a variant specialised on uniform values would cover
void RenderEncoder::logUniformTuple(const Pipeline &pl) const
{
    constexpr uint8_t kLowOccupancyGprs = 96;
    constexpr size_t kTuplesPerKey = 64, kLogBytes = 1024, kLineBytes = 128;
    if (!pl.rec || dev_->pack().section<PackProgram>(PackPrograms)[pl.rec->fs].gprs <= kLowOccupancyGprs)
        return;
    const PackPipeline &r = *pl.rec;
    uint64_t tuple = 0xcbf29ce484222325ull;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        const PackUniformRange &u = r.uniform[k];
        if (u.entry >= r.entries)
            continue;
        const PackEntry &e = r.entry[u.entry];
        const Bound &b = bufs_[e.stage][e.slot];
        const uint8_t *bytes = b.cpu;
        for (uint32_t i = 0; bytes && i < std::min<size_t>(b.size, kLogBytes); i++)
            tuple = (tuple ^ bytes[i]) * 0x100000001b3ull;
        tuple = (tuple ^ k) * 0x100000001b3ull;
    }
    static std::mutex lock;
    static std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> draws;
    uint64_t n;
    {
        std::lock_guard<std::mutex> l(lock);
        auto &tuples = draws[r.keyHash];
        auto it = tuples.find(tuple);
        if (it == tuples.end()) {
            if (tuples.size() >= kTuplesPerKey)
                return;
            it = tuples.emplace(tuple, 0).first;
        }
        n = ++it->second;
    }
    uint64_t power = 1;
    while (power < n)
        power *= 10;
    if (power != n)
        return;
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    os_log(log, "raw: uniform tuple key=%016llx tuple=%016llx draws=%llu", (unsigned long long)r.keyHash,
           (unsigned long long)tuple, (unsigned long long)n);
    if (n != 1)
        return;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        const PackUniformRange &u = r.uniform[k];
        if (u.entry >= r.entries)
            continue;
        const PackEntry &e = r.entry[u.entry];
        const Bound &b = bufs_[e.stage][e.slot];
        const size_t size = b.cpu ? std::min<size_t>(b.size, kLogBytes) : 0;
        for (size_t at = 0; at < size; at += kLineBytes) {
            char hex[2 * kLineBytes + 1];
            const size_t line = std::min(kLineBytes, size - at);
            for (size_t i = 0; i < line; i++)
                snprintf(hex + 2 * i, 3, "%02x", b.cpu[at + i]);
            os_log(log, "raw: uniform tuple key=%016llx tuple=%016llx stage=%u slot=%u bank=%u at=%zu %{public}s",
                   (unsigned long long)r.keyHash, (unsigned long long)tuple, e.stage, e.slot, u.bank, at, hex);
        }
    }
}
#endif

}  // namespace raw
