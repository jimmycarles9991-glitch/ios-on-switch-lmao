// SPDX-License-Identifier: MIT
// Device::addRecord (raw_gpu.h): a state the runtime made a record of (raw_record.h), drawn from then on as the pack's states are.
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"
#include "raw_dynamic_push.h"
#include "raw_record.h"
#include "raw_slm.h"
#include "raw_storage.h"

namespace raw {

// lock_ held: a program's slot laid out as the pack lays one (programSlot), once per program and constant buffer layout (one code
// can read its uniform buffers at another state's set-0 offsets), placed as the pack's programs are (heapPlace). The next
// submission's prologue invalidates the shader and constant caches before any draw reads it.
bool Device::placeCompleted(const CapturedProgram &p, PlacedProgram &out)
{
    const std::string key = p.id + std::string(p.cbufTypes.begin(), p.cbufTypes.end());
    if (const auto it = completedPrograms_.find(key); it != completedPrograms_.end()) {
        out = it->second.placed;
        return true;
    }
    if (p.image.size() < sizeof CompletedProgram::header || !slmNeed(slmPerWarpOfHeader(p.image.data())))
        return false;
    PackProgram info;
    const std::vector<uint8_t> slot = programSlot(p, info);
    uint64_t at = 0;
    if (!heapPlace(slot.data(), uint32_t(slot.size()), at))
        return false;
    info.heapOffset = uint32_t(at);
    CompletedProgram &placed = completedPrograms_[key];
    placed.info = info;
    memcpy(placed.header, p.image.data(), sizeof placed.header);
    placed.placed = {&placed.info, uint32_t(at) + info.program, info.dataBytes ? heap_.va + at + info.dataOffset : 0,
                     placed.header};
    RAW_DIAGNOSTIC(stats.programsPlaced++);
    out = placed.placed;
    return true;
}

bool Device::constexprSampler(const std::array<uint32_t, 8> &words, uint32_t &index)
{
    const uint32_t *pack = pack_.section<uint32_t>(PackSamplers);
    for (uint32_t i = 0; i < pack_.header()->samplers; i++)
        if (!memcmp(pack + 8 * i, words.data(), sizeof words)) {
            index = i;
            return true;
        }
    auto known = completedSamplers_.find(words);
    if (known == completedSamplers_.end()) {
        uint32_t tsc = 0;
        if (constexprCount_ == kConstexprSamplers || !takeTsc(tsc))
            return false;
        memcpy(tsc_.cpu + 32 * tsc, words.data(), 32);
        constexprTsc_[constexprCount_] = tsc;
        known = completedSamplers_.emplace(words, uint8_t(constexprCount_++)).first;
    }
    index = known->second;
    return true;
}

// lock_ held: a runtime record's split offered to its state (RuntimeRecord::split; the prologue's image an 80-byte header of
// zeros before its code), its programs laid out as heap slots for the draw that first takes it
void Device::offerCompletedSplit(Completed &c, const RuntimeRecord &r, const PlacedProgram &vs, bool caTransform)
{
    const CapturedProgram &prologue = r.splitPrograms[1];
    if (prologue.image.size() <= 80 || r.splitPrograms[0].image.size() < sizeof CompletedProgram::header)
        return;
    SplitSource source;
    source.rec = r.splitRecord;
    source.fsSlot = programSlot(r.splitPrograms[0], source.fsInfo);
    source.prologueSlot.assign(prologue.image.begin() + 80, prologue.image.end());
    source.prologueSlot.resize((source.prologueSlot.size() + 0xFF) & ~size_t(0xFF));
    source.prologueDataAt = uint32_t(source.prologueSlot.size());
    source.prologueDataBytes = uint32_t(prologue.data.size());
    source.prologueSlot.insert(source.prologueSlot.end(), prologue.data.begin(), prologue.data.end());
    source.pp = &c.record;
    source.blob = c.blob.data();
    source.keyString = c.keyString.c_str();
    source.vs = vs;
    source.caTransform = caTransform;
    offerSplit(c.pipeline, std::move(source));
}

Pipeline *Device::addRecord(const RuntimeRecord &r, bool caTransform, std::string &why, bool *retry)
{
    if (retry)
        *retry = false;
    const bool tess = tessellates(r.pipeline);
    for (const CapturedProgram *program : {&r.programs[0], &r.programs[1], &r.tessPrograms[0], &r.tessPrograms[1]}) {
        if (!tess && (program == &r.tessPrograms[0] || program == &r.tessPrograms[1]))
            continue;
        if (program->image.size() < sizeof CompletedProgram::header) {
            why = "missing shader program header";
            return nullptr;
        }
    }
    if (!usableDynamicPush(r.pipeline, r.keyString.c_str())) {
        why = "missing or invalid dynamic blend push metadata";
        return nullptr;
    }
    Pipeline *strideTwin = nullptr;
    if (r.strideTwin && !(strideTwin = addRecord(*r.strideTwin, caTransform, why, retry))) {
        why = "its stride twin: " + why;
        return nullptr;
    }
    std::lock_guard<std::mutex> l(lock_);
    const uint64_t key = r.pipeline.keyHash;
    if (const auto it = completed_.find(key); it != completed_.end())
        return it->second;
    PlacedProgram vs, fs, tessPlaced[2];
    if (!placeCompleted(r.programs[0], vs) || !placeCompleted(r.programs[1], fs) ||
        (tess &&
         (!placeCompleted(r.tessPrograms[0], tessPlaced[0]) || !placeCompleted(r.tessPrograms[1], tessPlaced[1])))) {
        stats.heapFull++;
        why = "the shader heap is full";
        if (retry)
            *retry = true;
        return nullptr;
    }
    Completed c;
    c.record = r.pipeline;
    c.keyString = r.keyString;
    c.blob = r.blob;
    c.spill = r.spill;
    for (const auto &[m, v] : r.extras) {
        c.blob.push_back(m);
        c.blob.push_back(v);
    }
    c.record.blobDwords = uint32_t(c.blob.size());
    if (!usableStorageMetadata(c.record, c.blob.data(), c.record.blobDwords)) {
        why = "missing or invalid ordered storage attachment metadata";
        return nullptr;
    }
    bool unknownSampler = false;
    if (!placeConstexprSamplers(
            c.record, r.samplers,
            [&](const std::array<uint32_t, 8> &words, uint32_t &index) {
                return constexprSampler(words, index);
            },
            unknownSampler)) {
        why = unknownSampler ? "a constexpr sampler the record holds no header of" : "no room for its sampler headers";
        if (retry)
            *retry = !unknownSampler;
        return nullptr;
    }
    Completed &stored = completedStore_.emplace_back(std::move(c));
    build(stored.pipeline, stored.record, vs, fs, stored.blob.data(), stored.keyString.c_str(), caTransform,
          stored.spill.size() == stored.record.spillWords[0] + stored.record.spillWords[1] ? stored.spill.data()
                                                                                           : nullptr,
          tess ? tessPlaced : nullptr);
    std::vector<uint32_t> words(2 * r.restore.size() + 2);
    Push w{words.data(), words.data() + words.size()};
    for (const auto &[m, v] : r.restore)
        w.m(m, v);
    stored.pipeline.restore.assign(words.data(), w.at);
    stored.pipeline.amplificationLayerWord = r.amplificationLayerWord;
    stored.pipeline.amplificationViewportWord = r.amplificationViewportWord;
    stored.pipeline.strideTwin = strideTwin;
    if (r.split && !c.record.storageMask)
        offerCompletedSplit(stored, r, vs, caTransform);
    for (uint32_t i = r.pipeline.variantCount[0]; i < r.variants.size(); i++) {
        if (!(r.variants[i].assume & AssumeTurned))
            continue;
        PlacedProgram turned;
        if (!placeCompleted(r.variants[i].program, turned)) {
            stored.pipeline.turnRefused = true;
            break;
        }
        build(stored.turned, stored.record, vs, turned, stored.blob.data(), stored.keyString.c_str(), caTransform,
              stored.spill.size() == stored.record.spillWords[0] + stored.record.spillWords[1] ? stored.spill.data()
                                                                                               : nullptr,
              tess ? tessPlaced : nullptr);
        stored.turned.restore = stored.pipeline.restore;
        stored.turned.amplificationLayerWord = r.amplificationLayerWord;
        stored.turned.amplificationViewportWord = r.amplificationViewportWord;
        stored.turned.strideTwin = strideTwin;
        stored.pipeline.turnedTwin = &stored.turned;
        stored.pipeline.turnedVariant = true;
        break;
    }
    completed_[key] = &stored.pipeline;
    return &stored.pipeline;
}

}  // namespace raw
