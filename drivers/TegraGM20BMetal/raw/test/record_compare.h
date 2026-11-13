// SPDX-License-Identifier: MIT
// Host tests: how a record the runtime made (raw/runtime/raw_record.h) differs from the pack's record of the same state, field by
// field; empty: none.
#pragma once
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../runtime/raw_pack.h"
#include "../runtime/raw_record.h"

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
inline bool inPackSamplers(const raw::Pack &pack, const raw::RuntimeRecord &r, raw::PackPipeline &out)
{
    out = r.pipeline;
    const uint32_t *headers = pack.section<uint32_t>(raw::PackSamplers);
    bool unknown = false;
    return raw::placeConstexprSamplers(
        out, r.samplers,
        [&](const std::array<uint32_t, 8> &words, uint32_t &index) {
            for (index = 0; index < pack.header()->samplers; index++)
                if (!memcmp(headers + 8 * index, words.data(), sizeof words))
                    return true;
            return false;
        },
        unknown);
}

inline std::string differences(const raw::Pack &pack, const raw::PackPipeline &pp, const raw::RuntimeRecord &r)
{
    std::string why;
    raw::PackPipeline rp;
    if (!inPackSamplers(pack, r, rp))
        why += " samplers";
    if (pack.string(pp.keyString) != r.keyString)
        why += " key-fields";
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + pp.blob;
    if (pp.blobDwords != r.blob.size() || memcmp(blob, r.blob.data(), r.blob.size() * 4))
        why += " blob" + blobDifferences(blob, pp.blobDwords, r.blob);
    if (pp.entries != rp.entries || memcmp(pp.entry, rp.entry, sizeof pp.entry)) {
        why += " entries[" + std::to_string(pp.entries) + "/" + std::to_string(rp.entries);
        for (uint32_t k = 0; k < 16; k++)
            if (memcmp(&pp.entry[k], &rp.entry[k], sizeof pp.entry[k])) {
                const raw::PackEntry &a = pp.entry[k], &b = rp.entry[k];
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
    if (memcmp(pp.spillEntry, rp.spillEntry, sizeof pp.spillEntry) ||
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
        rungs &= (pp.fetchRung[c] & raw::kFetchRungMask) == rp.fetchRung[c];
    if (pp.boundEntries != rp.boundEntries || pp.memoryEntries != rp.memoryEntries ||
        memcmp(pp.uniform, rp.uniform, sizeof pp.uniform) || !rungs)
        why += " reads";
    if (pp.variantCount[0] != rp.variantCount[0] || pp.variantCount[1] != rp.variantCount[1])
        why += " variants";
    for (int k = 0; k < 2; k++) {
        const raw::PackProgram &packed = pack.section<raw::PackProgram>(raw::PackPrograms)[k ? pp.fs : pp.vs];
        raw::PackProgram made;
        const std::vector<uint8_t> slot = raw::programSlot(r.programs[k], made);
        const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap) + packed.heapOffset;
        if (packed.bytes != made.bytes || memcmp(heap, slot.data(), slot.size()) ||
            packed.program - packed.heapOffset != made.program || packed.dataOffset != made.dataOffset ||
            packed.dataBytes != made.dataBytes || packed.cbufs != made.cbufs ||
            memcmp(packed.cbufType, made.cbufType, sizeof made.cbufType) || packed.gprs != made.gprs ||
            packed.boundTextures != made.boundTextures)
            why += k ? " pixel-program" : " vertex-program";
    }
    return why;
}

// How the split a record made with its split (Direct::make's `split`) differs from the pack's of the same state
inline std::string splitDifferences(const raw::Pack &pack, const raw::PackPipeline &pp, const raw::RuntimeRecord &r)
{
    const raw::PackSplit *packed = pack.split(pp.fs);
    if (!r.splitTried)
        return " split-untried";
    if (!packed || !r.split)
        return packed ? " split-missing" : r.split ? " split-extra" : "";
    std::string why;
    const raw::PackSplit &m = r.splitRecord;
    if (packed->thresholdPixels != m.thresholdPixels || packed->frontierBytes != m.frontierBytes ||
        packed->frontierCbuf != m.frontierCbuf || packed->uboCount != m.uboCount ||
        packed->shaderDataCbuf != m.shaderDataCbuf || packed->prologueCbufs != m.prologueCbufs ||
        packed->batchVersion != m.batchVersion || packed->batchThreads != m.batchThreads ||
        memcmp(packed->uboCbuf, m.uboCbuf, sizeof m.uboCbuf) || memcmp(packed->qmd, m.qmd, sizeof m.qmd) ||
        memcmp(&packed->cost, &m.cost, sizeof m.cost))
        why += " split-record";
    const raw::PackProgram *programs = pack.section<raw::PackProgram>(raw::PackPrograms);
    const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap);
    const raw::PackProgram &fs = programs[packed->fs];
    raw::PackProgram made;
    const std::vector<uint8_t> slot = raw::programSlot(r.splitPrograms[0], made);
    if (fs.bytes != made.bytes || memcmp(heap + fs.heapOffset, slot.data(), slot.size()) ||
        fs.program - fs.heapOffset != made.program || fs.cbufs != made.cbufs ||
        memcmp(fs.cbufType, made.cbufType, sizeof made.cbufType))
        why += " split-pixel-program";
    const raw::PackProgram &prologue = programs[packed->prologue];
    const raw::CapturedProgram &p = r.splitPrograms[1];
    const size_t code = p.image.size() > 80 ? p.image.size() - 80 : 0;
    if (!code || prologue.dataBytes < p.data.size() || memcmp(heap + prologue.program, p.image.data() + 80, code) ||
        memcmp(heap + prologue.heapOffset + prologue.dataOffset, p.data.data(), p.data.size()))
        why += " split-prologue";
    return why;
}

}  // namespace recordcompare
