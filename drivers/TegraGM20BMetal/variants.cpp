// SPDX-License-Identifier: MIT
// Program variants (pack.h PackVariant): a stage's programs compiled under assumptions about its draws. A draw's facts are
// worked out when its bindings change; the pipeline it draws with is the one whose programs are the first variants of each
// stage those facts hold for. Compatible pipelines are placed with their state, sharing their programs in the pack heap.
#include <algorithm>
#include "creation_cost.h"
#include "gpu.h"
#include "pack_window.h"
#include "record.h"
#include "set_reads.h"
#include "slm.h"
#include "variants.h"

namespace gm20b {
namespace {

constexpr uint32_t kUniformAlignment = 256;  // residual.h bufferBaseAlignment(false)

// The set-0 entries combination (v, f) of pack table `t`'s stage variants reads past its record's
// (PackVariant::memoryEntries): v or f equal to its stage's count is that stage's generic program, whose reads are the
// record's
uint32_t variantSetReads(const VariantTable &t, uint32_t v, uint32_t f)
{
    return (v < t.count[0] ? t.variants[0][v].memoryEntries : 0u) |
           (f < t.count[1] ? t.variants[1][f].memoryEntries : 0u);
}

void attachVariantSplit(Device &dev, const Pack &pack, Pipeline &made, const PackPipeline &pp, const PlacedProgram &vs,
                        uint32_t fsIndex, void (Device::*offer)(Pipeline &, SplitSource &&))
{
    if (const PackSplit *split = pixelStage(pp) ? pack.split(fsIndex) : nullptr; split && !made.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack.section<uint32_t>(PackBlob) + pp.blob;
        source.far = pack.far(pp);
        source.keyString = pack.string(pp.keyString);
        source.vs = vs;
        source.caTransform = made.caTransform;
        (dev.*offer)(made, std::move(source));
    }
}

// Each combination of `t`'s variants some draw's facts select, but the state's own: `make(vertex, fragment, its index in
// chosen)`, a variant's index or its stage's count for the stage's own program; false at the first `make` that fails
template <typename Make> bool eachCombination(const VariantTable &t, uint32_t chipFacts, Make make)
{
    for (uint32_t v = 0; v <= t.count[0]; v++)
        for (uint32_t f = 0; f <= t.count[1]; f++) {
            if (v == t.count[0] && f == t.count[1])
                continue;
            const uint32_t facts = chipFacts | (v < t.count[0] ? t.variants[0][v].assume : 0u) |
                                   (f < t.count[1] ? t.variants[1][f].assume : 0u);
            if (chooseVariant(t.variants[0], t.count[0], facts) != v ||
                chooseVariant(t.variants[1], t.count[1], facts) != f)
                continue;
            if (!make(v, f, v * (t.count[1] + 1u) + f))
                return false;
        }
    return true;
}

// Pack pipeline `pl`'s table of record `pp`'s variants, every choice `pl` until its combinations are placed; null: none
std::unique_ptr<VariantTable> packTable(const Pack &pack, Pipeline &pl, const PackPipeline &pp)
{
    uint32_t count[2] = {};
    const PackVariant *variants[2] = {pack.variants(pp, 0, count[0]), pack.variants(pp, 1, count[1])};
    if (!count[0] && !count[1])
        return nullptr;
    auto t = std::make_unique<VariantTable>();
    for (uint32_t s = 0; s < 2; s++) {
        t->variants[s] = variants[s];
        t->count[s] = uint8_t(count[s]);
    }
    pl.turnedVariant = count[1] && (variants[1][0].assume & AssumeTurned);
    t->chosen.assign((count[0] + 1) * (count[1] + 1), &pl);
    return t;
}

// lock_ held, or `pl` not yet published. Once only: a draw may hold the table it loaded for as long as it draws.
void publishVariants(Pipeline &pl, std::unique_ptr<VariantTable> t)
{
    if (pl.ownedVariantTable)
        return;
    pl.ownedVariantTable = std::move(t);
    __atomic_store_n(&pl.variantTable, pl.ownedVariantTable.get(), __ATOMIC_RELEASE);
}

}  // namespace

void RenderEncoder::setPipeline(Pipeline *p)
{
    alphaWanted_ = 0;
    if (pipeline_ == p)
        return;
    pipeline_ = p;
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
            slm = std::max(slm, programSlm(placed));
        }
    }
    return true;
}

Pipeline &Device::buildCombination(VariantTable &t, const Pipeline &pl, const PackPipeline &pp,
                                   const PlacedProgram (&placed)[2], uint32_t setReads, const uint32_t *blob,
                                   const char *keyString, const uint32_t *spills, const uint32_t *far,
                                   const PackSegment *prebuilt)
{
    Pipeline &made = *t.pipelines.emplace_back(std::make_unique<Pipeline>());
    SWITCH_METAL_CREATION_COUNT(variants, 1);
    build(made, pp, placed[0], placed[1], blob, keyString, pl.caTransform, spills, far, nullptr, prebuilt);
    made.memoryEntries |= setReads;
    made.restore = pl.restore;
    made.strideTwin = pl.strideTwin;
    return made;
}

// lock_ held: pack pipeline `pl` of record `pp`, just built, learns its stages' variants
void Device::offerVariants(Pipeline &pl, const PackPipeline &pp, uint32_t recIndex)
{
    std::unique_ptr<VariantTable> t = packTable(pack_, pl, pp);
    if (!t)
        return;
    eachCombination(*t, pack_.header()->chipFacts, [&](uint32_t v, uint32_t f, uint32_t combo) {
        t->chosen[combo] = placeVariant(
            pl, *t,
            {v < t->count[0] ? t->variants[0][v].program : pp.vs, f < t->count[1] ? t->variants[1][f].program : pp.fs},
            variantSetReads(*t, v, f), recIndex, combo);
        return true;
    });
    publishVariants(pl, std::move(t));
}

bool Device::offerVariantsLockFree(Pipeline &pl, const PackPipeline &pp, uint32_t recIndex)
{
    std::unique_ptr<VariantTable> t = packTable(pack_, pl, pp);
    if (!t)
        return true;
    const bool placedAll = eachCombination(*t, pack_.header()->chipFacts, [&](uint32_t v, uint32_t f, uint32_t combo) {
        const PackSegment *prebuilt = prebuiltVariantSegment(recIndex, combo);
        if (!prebuilt)
            return false;
        const std::array<uint32_t, 2> index = {v < t->count[0] ? t->variants[0][v].program : pp.vs,
                                               f < t->count[1] ? t->variants[1][f].program : pp.fs};
        PlacedProgram placed[2] = {{}, noPixelProgram()};
        if (!placePackWindowProgram(index[0], placed[0]) ||
            (pixelStage(pp) && !placePackWindowProgram(index[1], placed[1])))
            return false;
        if (!slmReady(std::max(programSlm(placed[0]), programSlm(placed[1]))))
            return false;
        Pipeline &made =
            buildCombination(*t, pl, pp, placed, variantSetReads(*t, v, f), pack_.section<uint32_t>(PackBlob) + pp.blob,
                             pack_.string(pp.keyString), pack_.spills(pp), pack_.far(pp), prebuilt);
        attachVariantSplit(*this, pack_, made, pp, placed[0], index[1], &Device::offerSplit);
        t->chosen[combo] = &made;
        return true;
    });
    if (!placedAll)
        return false;
    publishVariants(pl, std::move(t));
    return true;
}

// lock_ held: runtime record `r`'s program variants given to its state `c`, whose record is `r` or of its layout (its full
// form, Device::attachFullForm): each combination a draw's facts select built over c's registers and entries. Its turned
// variant is no choice of the table's (TurnedSource), and only a state of a vertex and a fragment stage is given any, as
// direct.cpp programVariants makes them.
void Device::offerCompletedVariants(Completed &c, const RuntimeRecord &r)
{
    if (c.pipeline.ownedVariantTable || !pixelStage(c.record) || tessellates(c.record))
        return;
    auto t = std::make_unique<VariantTable>();
    t->runtime = true;
    for (uint32_t i = 0; i < r.variants.size(); i++) {
        const RuntimeRecord::Variant &v = r.variants[i];
        std::vector<PackVariant> &stage = t->runtimeVariants[i < r.pipeline.variantCount[0] ? 0 : 1];
        if (!(v.assume & AssumeTurned) && v.program.image.size() >= sizeof CompletedProgram::header &&
            stage.size() < UINT8_MAX) {
            // CompletedProgram::header holds only the 80-byte SPH, so compute set reads from the variant image here
            uint32_t reads[entryMaskWords(kSetEntriesMax)] = {};
            programSetReads(v.program.image.data() + sizeof CompletedProgram::header,
                            v.program.image.size() - sizeof CompletedProgram::header, v.program.cbufTypes.data(),
                            uint32_t(std::min<size_t>(v.program.cbufTypes.size(), sizeof(PackProgram::cbufType))),
                            v.program.boundTextures, c.pipeline.entries, reads);
            stage.push_back({v.assume, i, reads[0]});
        }
    }
    if (t->runtimeVariants[0].empty() && t->runtimeVariants[1].empty())
        return;
    for (uint32_t s = 0; s < 2; s++) {
        t->variants[s] = t->runtimeVariants[s].data();
        t->count[s] = uint8_t(t->runtimeVariants[s].size());
    }
    t->chosen.assign((t->count[0] + 1) * (t->count[1] + 1), &c.pipeline);
    const uint32_t *spills =
        c.spill.size() == c.record.spillWords[0] + c.record.spillWords[1] ? c.spill.data() : nullptr;
    const uint32_t *far = c.far.empty() ? nullptr : c.far.data();
    eachCombination(*t, pack_.header()->chipFacts, [&](uint32_t v, uint32_t f, uint32_t combo) {
        const uint32_t choice[2] = {v, f};
        PlacedProgram placed[2] = {c.vs, c.fs};
        for (uint32_t s = 0; s < 2; s++) {
            if (choice[s] == t->count[s])
                continue;
            if (!placeCompleted(r.variants[t->variants[s][choice[s]].program].program, placed[s])) {
                stats.heapFull++;
                return true;
            }
        }
        if (!slmNeed(std::max(slmPerWarpOfHeader(placed[0].header), slmPerWarpOfHeader(placed[1].header)))) {
            stats.heapFull++;
            return true;
        }
        t->chosen[combo] = &buildCombination(*t, c.pipeline, c.record, placed, variantSetReads(*t, v, f), c.blob.data(),
                                             c.keyString.c_str(), spills, far, nullptr);
        return true;
    });
    publishVariants(c.pipeline, std::move(t));
}

Pipeline *Device::variant(Pipeline &pl, uint32_t facts)
{
    return variant(pl, __atomic_load_n(&pl.variantTable, __ATOMIC_ACQUIRE), facts);
}

Pipeline *Device::variant(Pipeline &pl, const VariantTable *t, uint32_t facts)
{
    if (!t || (t->runtime && (facts & AssumeTurned)))
        return &pl;
    facts |= pack_.header()->chipFacts;
    const uint32_t v = chooseVariant(t->variants[0], t->count[0], facts);
    const uint32_t f = chooseVariant(t->variants[1], t->count[1], facts);
    return t->chosen[v * (t->count[1] + 1u) + f];
}

Pipeline *Device::placeVariant(Pipeline &pl, VariantTable &t, const std::array<uint32_t, 2> &index, uint32_t setReads,
                               uint32_t recIndex, uint32_t comboIndex)
{
    const PackPipeline &pp = *pl.rec;
    PlacedProgram placed[2] = {{}, noPixelProgram()};
    bool placedBoth =
        placePackProgram(index[0], placed[0]) && (!pixelStage(pp) || placePackProgram(index[1], placed[1]));
    if (placedBoth && !slmNeed(std::max(programSlm(placed[0]), programSlm(placed[1])))) {
        stats.heapFull++;
        placedBoth = false;
    }
    if (!placedBoth)
        return &pl;
    Pipeline &made = buildCombination(t, pl, pp, placed, setReads, pack_.section<uint32_t>(PackBlob) + pp.blob,
                                      pack_.string(pp.keyString), pack_.spills(pp), pack_.far(pp),
                                      prebuiltVariantSegment(recIndex, comboIndex));
    attachVariantSplit(*this, pack_, made, pp, placed[0], index[1], &Device::offerSplit);
    return &made;
}

bool Device::placePackWindowProgram(uint32_t index, PlacedProgram &out)
{
    if (!packWindowProgram(index, out.program, out.data))
        return false;
    const PackProgram *programs = pack_.section<PackProgram>(PackPrograms);
    out.record = &programs[index];
    out.header = pack_.section<uint8_t>(PackHeap) + programs[index].program;
    out.farUbos = pack_.farUbos(programs[index]);
    SWITCH_METAL_DIAGNOSTIC(stats.packSlotsInBoundGrains.fetch_add(1, std::memory_order_relaxed));
    return true;
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

// The facts of the draw's bindings that `pl`'s variants (`variants`, as the draw loaded them) may assume (pack.h
// PackAssumption bits 0-15), worked out again only when a binding changed
uint32_t RenderEncoder::bindingFacts(const Pipeline &pl, const VariantTable *variants)
{
    if (!variants)
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

}  // namespace gm20b
