// SPDX-License-Identifier: MIT
// Device::addRecord (raw_gpu.h): a state the runtime made a record of (raw_record.h), drawn from then on as the pack's states are.
#include <cstring>
#include "raw_gpu.h"
#include "raw_record.h"

namespace raw {

// lock_ held: a program's slot laid out as the pack lays one (programSlot), once per program, placed as the pack's programs are
// (heapPlace). The next submission's prologue invalidates the shader and constant caches before any draw reads it.
bool Device::placeCompleted(const CapturedProgram &p, PlacedProgram &out)
{
    if (const auto it = completedPrograms_.find(p.id); it != completedPrograms_.end()) {
        out = it->second.placed;
        return true;
    }
    if (p.image.size() < sizeof CompletedProgram::header)
        return false;
    PackProgram info;
    const std::vector<uint8_t> slot = programSlot(p, info);
    uint64_t at = 0;
    if (!heapPlace(slot.data(), uint32_t(slot.size()), at))
        return false;
    info.heapOffset = uint32_t(at);
    CompletedProgram &placed = completedPrograms_[p.id];
    placed.info = info;
    memcpy(placed.header, p.image.data(), sizeof placed.header);
    placed.placed = {&placed.info, uint32_t(at) + info.program, info.dataBytes ? heap_.va + at + info.dataOffset : 0,
                     placed.header};
    stats.programsPlaced++;
    out = placed.placed;
    return true;
}

Pipeline *Device::addRecord(const RuntimeRecord &r, bool caTransform, std::string &why)
{
    std::lock_guard<std::mutex> l(lock_);
    const uint64_t key = r.pipeline.keyHash;
    if (const auto it = completed_.find(key); it != completed_.end())
        return it->second;
    PlacedProgram vs, fs;
    if (!placeCompleted(r.programs[0], vs) || !placeCompleted(r.programs[1], fs)) {
        stats.heapFull++;
        why = "the shader heap is full";
        return nullptr;
    }
    Completed c;
    c.record = r.pipeline;
    c.keyString = r.keyString;
    c.blob = r.blob;
    for (const auto &[m, v] : r.extras) {
        c.blob.push_back(m);
        c.blob.push_back(v);
    }
    c.record.blobDwords = uint32_t(c.blob.size());
    // its constexpr samplers past the pack's own, numbered among the device's
    const uint32_t packSamplers = pack_.header()->samplers;
    for (uint32_t k = 0; k < c.record.entries; k++) {
        PackEntry &e = c.record.entry[k];
        if (e.kind == EntryUbo || e.samplerKind != SamplerConstexpr || e.sampler < packSamplers)
            continue;
        const std::array<uint32_t, 8> &words = r.samplers.at(e.sampler - packSamplers);
        auto known = completedSamplers_.find(words);
        if (known == completedSamplers_.end()) {
            uint32_t tsc = 0;
            if (constexprCount_ == kConstexprSamplers || !takeTsc(tsc)) {
                why = "no room for its sampler headers";
                return nullptr;
            }
            memcpy(tsc_.cpu + 32 * tsc, words.data(), 32);
            constexprTsc_[constexprCount_] = tsc;
            known = completedSamplers_.emplace(words, uint8_t(constexprCount_++)).first;
        }
        e.sampler = known->second;
    }
    Completed &stored = completedStore_.emplace_back(std::move(c));
    build(stored.pipeline, stored.record, vs, fs, stored.blob.data(), stored.keyString.c_str(), caTransform);
    std::vector<uint32_t> words(2 * r.restore.size() + 2);
    Push w{words.data(), words.data() + words.size()};
    for (const auto &[m, v] : r.restore)
        w.m(m, v);
    stored.pipeline.restore.assign(words.data(), w.at);
    completed_[key] = &stored.pipeline;
    return &stored.pipeline;
}

}  // namespace raw
