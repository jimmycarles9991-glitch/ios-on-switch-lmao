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

inline std::string differences(const raw::Pack &pack, const raw::PackPipeline &pp, const raw::RuntimeRecord &r)
{
    std::string why;
    const raw::PackPipeline &rp = r.pipeline;
    if (pack.string(pp.keyString) != r.keyString)
        why += " key-fields";
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + pp.blob;
    if (pp.blobDwords != r.blob.size() || memcmp(blob, r.blob.data(), r.blob.size() * 4))
        why += " blob" + blobDifferences(blob, pp.blobDwords, r.blob);
    if (pp.entries != rp.entries || memcmp(pp.entry, rp.entry, sizeof pp.entry))
        why += " entries";
    if (pp.pushes != rp.pushes || pp.pushMask != rp.pushMask || memcmp(pp.pushWords, rp.pushWords, sizeof pp.pushWords))
        why += " push";
    if (pp.vertexSlot != rp.vertexSlot || pp.vertexStride != rp.vertexStride)
        why += " vertex";
    if (pp.setBytes != rp.setBytes)
        why += " set-bytes";
    if (pp.flags != rp.flags)
        why += " flags";
    if (pp.keyHash != rp.keyHash)
        why += " key";
    if (!r.extras.empty() || !r.samplers.empty())
        why += " extras";
    for (int k = 0; k < 2; k++) {
        const raw::PackProgram &packed = pack.section<raw::PackProgram>(raw::PackPrograms)[k ? pp.fs : pp.vs];
        raw::PackProgram made;
        const std::vector<uint8_t> slot = raw::programSlot(r.programs[k], made);
        const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap) + packed.heapOffset;
        if (packed.bytes != made.bytes || memcmp(heap, slot.data(), slot.size()) ||
            packed.program - packed.heapOffset != made.program || packed.dataOffset != made.dataOffset ||
            packed.dataBytes != made.dataBytes || packed.cbufs != made.cbufs ||
            memcmp(packed.cbufType, made.cbufType, sizeof made.cbufType))
            why += k ? " pixel-program" : " vertex-program";
    }
    return why;
}

}  // namespace recordcompare
