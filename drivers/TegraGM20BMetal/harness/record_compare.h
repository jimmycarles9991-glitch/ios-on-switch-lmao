// SPDX-License-Identifier: MIT
// Host tests: how a record the runtime made (record.h) differs from the pack's record of the same state, field by
// field; empty: none.
#pragma once
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../pack.h"
#include "../record.h"

namespace recordcompare {

// The registers whose values differ between two blobs of (method, value) pairs: "[method pack/made ...]"
inline std::string blobDifferences(const uint32_t *packed, uint32_t packedDwords, const std::vector<uint32_t> &made)
{
    std::map<uint32_t, std::pair<int64_t, int64_t>> regs;
    for (uint32_t k = 0; k + 1 < packedDwords; k += 2)
        regs[packed[k]] = {packed[k + 1], -1};
    for (size_t k = 0; k + 1 < made.size(); k += 2) {
        auto it = regs.try_emplace(made[k], std::pair<int64_t, int64_t>{-1, -1}).first;
        it->second.second = made[k + 1];
    }
    std::string out;
    char line[64];
    for (const auto &[m, v] : regs)
        if (v.first != v.second) {
            snprintf(line, sizeof line, " %04x %llx/%llx", m, (long long)v.first, (long long)v.second);
            out += line;
        }
    return "[" + out + " ]";
}

// The made record with its constexpr samplers numbered as the pack's; false: a header the pack lacks
inline bool inPackSamplers(const gm20b::Pack &pack, const gm20b::RuntimeRecord &r, gm20b::PackPipeline &out,
                           std::vector<uint32_t> &far)
{
    out = r.pipeline;
    far = r.far;
    const uint32_t *headers = pack.section<uint32_t>(gm20b::PackSamplers);
    bool unknown = false;
    return gm20b::placeConstexprSamplers(
        out, far, r.samplers,
        [&](const std::array<uint32_t, 8> &words, uint32_t &index) {
            for (index = 0; index < pack.header()->samplers; index++)
                if (!memcmp(headers + 8 * index, words.data(), sizeof words))
                    return true;
            return false;
        },
        unknown);
}

// A pack program's far table of uniform buffers (null: none) and a made program's name the same offsets
inline bool sameFarUbos(const uint16_t *packed, const gm20b::CapturedProgram &made)
{
    for (uint32_t c = 0; c < gm20b::kProgramCbufs; c++)
        if ((packed ? packed[c] : 0) != (c < made.farUbos.size() ? made.farUbos[c] : 0))
            return false;
    return true;
}

inline std::string differences(const gm20b::Pack &pack, const gm20b::PackPipeline &pp, const gm20b::RuntimeRecord &r)
{
    std::string why;
    gm20b::PackPipeline rp;
    std::vector<uint32_t> far;
    if (!inPackSamplers(pack, r, rp, far))
        why += " samplers";
    if (pack.string(pp.keyString) != r.keyString)
        why += " key-fields";
    // a pack record's blob holds its extras after its varying pairs, its restore pairs at restoreAt (directlink.py)
    std::vector<uint32_t> held = r.blob, restore;
    for (const auto &[m, v] : r.extras)
        held.insert(held.end(), {m, v});
    for (const auto &[m, v] : r.restore)
        restore.insert(restore.end(), {m, v});
    const uint32_t *blob = pack.section<uint32_t>(gm20b::PackBlob) + pp.blob;
    if (pp.blobDwords != held.size() || memcmp(blob, held.data(), held.size() * 4))
        why += " blob" + blobDifferences(blob, pp.blobDwords, held);
    if (pp.restoreDwords != restore.size() ||
        (!restore.empty() &&
         memcmp(pack.section<uint32_t>(gm20b::PackBlob) + pp.restoreAt, restore.data(), restore.size() * 4)))
        why += " restore";
    const uint32_t *packFar = pack.far(pp);
    const uint32_t farWords = gm20b::farWords(gm20b::entryCount(pp));
    if (far.size() != farWords || (farWords && (!packFar || memcmp(packFar, far.data(), 4 * farWords))))
        why += " far-entries";
    if (gm20b::entryCount(pp) != gm20b::entryCount(rp) || memcmp(pp.entry, rp.entry, sizeof pp.entry)) {
        why += " entries[" + std::to_string(gm20b::entryCount(pp)) + "/" + std::to_string(gm20b::entryCount(rp));
        for (uint32_t k = 0; k < 16; k++)
            if (memcmp(&pp.entry[k], &rp.entry[k], sizeof pp.entry[k])) {
                const gm20b::PackEntry &a = pp.entry[k], &b = rp.entry[k];
                char line[160];
                snprintf(line, sizeof line, " %u: %u.%u.%u.%u.%u.%u.%u/%u.%u.%u.%u.%u.%u.%u", k, a.offset, a.kind,
                         a.stage, a.slot, a.samplerKind, a.sampler, a.pad, b.offset, b.kind, b.stage, b.slot,
                         b.samplerKind, b.sampler, b.pad);
                why += line;
                break;
            }
        why += " ]";
    }
    if (pp.pushes != rp.pushes || pp.pushMask != rp.pushMask || memcmp(pp.pushWords, rp.pushWords, sizeof pp.pushWords))
        why += " push";
    const uint32_t spillWords = uint32_t(pp.spillWords[0]) + pp.spillWords[1];
    const uint32_t *spills = pack.spills(pp);
    if (memcmp(pp.spillEntry, rp.spillEntry, sizeof pp.spillEntry) || pp.spillEntryHigh != rp.spillEntryHigh ||
        memcmp(pp.spillWords, rp.spillWords, sizeof pp.spillWords) || r.spill.size() != spillWords ||
        (spillWords && (!spills || memcmp(spills, r.spill.data(), 4 * spillWords))))
        why += " spills";
    if (pp.vertexSlot != rp.vertexSlot || pp.vertexStride != rp.vertexStride)
        why += " vertex";
    if (pp.setBytes != rp.setBytes)
        why += " set-bytes";
    if (pp.flags != rp.flags)
        why += " flags";
    if (pp.keyHash != rp.keyHash)
        why += " key";
    if (!r.extras.empty())
        why += " extras";
    // the pack's candidate rungs are its generator's (directpack.mm markCandidates), which no runtime record has
    bool rungs = true;
    for (uint32_t c = 0; c < 8; c++)
        rungs &= (pp.fetchRung[c] & gm20b::kFetchRungMask) == rp.fetchRung[c];
    if (pp.boundEntries != rp.boundEntries || pp.memoryEntries != rp.memoryEntries ||
        memcmp(pp.uniform, rp.uniform, sizeof pp.uniform) || !rungs)
        why += " reads";
    if (pp.variantCount[0] != rp.variantCount[0] || pp.variantCount[1] != rp.variantCount[1])
        why += " variants";
    if (gm20b::pixelStage(pp) != gm20b::pixelStage(rp))
        why += " pixel-stage";
    const bool tess = gm20b::tessellates(pp);
    const uint32_t index[4] = {pp.vs, pp.fs, pp.tessPrograms[0], pp.tessPrograms[1]};
    const gm20b::CapturedProgram *program[4] = {&r.programs[0], &r.programs[1], &r.tessPrograms[0], &r.tessPrograms[1]};
    const char *name[4] = {" vertex-program", " pixel-program", " control-program", " evaluation-program"};
    const bool pixels = gm20b::pixelStage(pp) && gm20b::pixelStage(rp);
    for (int k = 0; k < (tess ? 4 : 2); k++) {
        if (k == 1 && !pixels)
            continue;
        const gm20b::PackProgram &packed = pack.section<gm20b::PackProgram>(gm20b::PackPrograms)[index[k]];
        gm20b::PackProgram made;
        const std::vector<uint8_t> slot = gm20b::programSlot(*program[k], made);
        const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap) + packed.heapOffset;
        if (packed.bytes != made.bytes || memcmp(heap, slot.data(), slot.size()) ||
            packed.program - packed.heapOffset != made.program || packed.dataOffset != made.dataOffset ||
            packed.dataBytes != made.dataBytes || packed.cbufs != made.cbufs ||
            memcmp(packed.cbufType, made.cbufType, sizeof made.cbufType) || packed.gprs != made.gprs ||
            packed.boundTextures != made.boundTextures || !sameFarUbos(pack.farUbos(packed), *program[k]))
            why += name[k];
    }
    return why;
}

// How the split a record made with its split (Direct::make's `split`) differs from the pack's of the same state.
// `splittable`: its state is one the direct compile splits (direct.h splitsFragment); any other has no split to make
inline std::string splitDifferences(const gm20b::Pack &pack, const gm20b::PackPipeline &pp,
                                    const gm20b::RuntimeRecord &r, bool splittable)
{
    const gm20b::PackSplit *packed = pack.split(pp.fs);
    if (!splittable)
        return packed ? " split-packed" : r.splitTried || r.split ? " split-extra" : "";
    if (!r.splitTried)
        return " split-untried";
    if (!packed || !r.split)
        return packed ? " split-missing" : r.split ? " split-extra" : "";
    std::string why;
    const gm20b::PackSplit &m = r.splitRecord;
    if (packed->thresholdPixels != m.thresholdPixels || packed->frontierBytes != m.frontierBytes ||
        packed->frontierCbuf != m.frontierCbuf || packed->uboCount != m.uboCount ||
        packed->shaderDataCbuf != m.shaderDataCbuf || packed->prologueCbufs != m.prologueCbufs ||
        packed->batchVersion != m.batchVersion || packed->batchThreads != m.batchThreads ||
        memcmp(packed->uboCbuf, m.uboCbuf, sizeof m.uboCbuf) || memcmp(packed->qmd, m.qmd, sizeof m.qmd) ||
        memcmp(&packed->cost, &m.cost, sizeof m.cost))
        why += " split-record";
    const gm20b::PackProgram *programs = pack.section<gm20b::PackProgram>(gm20b::PackPrograms);
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    const gm20b::PackProgram &fs = programs[packed->fs];
    gm20b::PackProgram made;
    const std::vector<uint8_t> slot = gm20b::programSlot(r.splitPrograms[0], made);
    if (fs.bytes != made.bytes || memcmp(heap + fs.heapOffset, slot.data(), slot.size()) ||
        fs.program - fs.heapOffset != made.program || fs.cbufs != made.cbufs ||
        memcmp(fs.cbufType, made.cbufType, sizeof made.cbufType))
        why += " split-pixel-program";
    const gm20b::PackProgram &prologue = programs[packed->prologue];
    const gm20b::CapturedProgram &p = r.splitPrograms[1];
    const size_t code = p.image.size() > 80 ? p.image.size() - 80 : 0;
    if (!code || prologue.dataBytes < p.data.size() || memcmp(heap + prologue.program, p.image.data() + 80, code) ||
        memcmp(heap + prologue.heapOffset + prologue.dataOffset, p.data.data(), p.data.size()))
        why += " split-prologue";
    return why;
}

}  // namespace recordcompare
