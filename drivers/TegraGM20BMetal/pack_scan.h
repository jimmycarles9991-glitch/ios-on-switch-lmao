// SPDX-License-Identifier: MIT
// What every pack the generator writes holds to, which the runtime reads unchecked (Pack::load reads its header alone, a
// record is drawn as the pack holds it): the generator's tests scan the built pack (test/directcheck_test.py) and the
// packs host tests write with this.
#pragma once
#include "guest_bases.h"
#include "dynamic_push.h"
#include "pack.h"
#include "program_facts.h"
#include "program_pages.h"
#include "program_stores.h"
#include "set_entries.h"
#include "slm.h"
#include "storage.h"

#include <cstdio>
#include <string>
#include <vector>

namespace gm20b {

// Why the runtime could not draw record `pp` of `pack` as the pack holds it; nullptr: it can
inline const char *packRecordRefusal(const Pack &pack, const PackPipeline &pp)
{
    if (const char *why = entriesRefusal(pp, pack.far(pp), pack.header()->samplers))
        return why;
    if (!usableDynamicPush(pp, pack.string(pp.keyString)))
        return "its dynamic push words are not this runtime's";
    if (!usableStorageMetadata(pp, pack.section<uint32_t>(PackBlob) + pp.blob, pp.blobDwords, pack.far(pp)))
        return "its storage metadata is not this runtime's";
    if (!usableGuestBases(pp))
        return "its buffers' Metal address words are not this runtime's";
    return nullptr;
}

// Each way `pack` is not what the runtime reads: its pipelines out of keyHash order within a tier, a record's far block or
// a program's far table past the blob section, a record of more entries than any holds, a record refused
// (packRecordRefusal), a fast key out of order within its tier or not naming the first of its state's records and every
// one of them, or naming another tier's; a program whose facts are not its code's and its header's, one in tier 0's
// part of the heap above the tier-0 local memory ceiling, or one a launch-set record draws outside it; a variant whose
// set reads are not its code's; a segment outside the segments' words block; a stride twin past the pipelines or of a
// later tier than its record
inline std::vector<std::string> packScan(const Pack &pack)
{
    std::vector<std::string> out;
    const auto say = [&out](const char *what, uint64_t which, const char *why) {
        char line[96];
        snprintf(line, sizeof line, "%s %016llx: ", what, (unsigned long long)which);
        out.push_back(line + std::string(why));
    };
    const PackHeader &h = *pack.header();
    const PackPipeline *p = pack.section<PackPipeline>(PackPipelines);
    const uint64_t blobWords = pack.bytes(PackBlob) / sizeof(uint32_t);
    const uint64_t launchRecords = pack.tier0Entries(PackPipelines, sizeof(PackPipeline));
    const uint64_t launchPrograms = pack.tier0Entries(PackPrograms, sizeof(PackProgram));
    if (h.section[PackHeap].offset % kPackTierAlign || h.tier0[PackHeap] % kPackTierAlign)
        say("heap", h.tier0[PackHeap], "its tier 0 off the tier boundary");
    const PackProgram *programs = pack.section<PackProgram>(PackPrograms);
    const uint8_t *heap = pack.section<uint8_t>(PackHeap);
    std::vector<bool> scanned(h.programs);
    const auto drawn = [&](uint32_t index, bool launch) {
        constexpr uint32_t kCodeInSlot = 0x80;
        if (index == kNoProgram)
            return;
        if (index >= h.programs) {
            say("program", index, "past the programs");
            return;
        }
        const PackProgram &program = programs[index];
        if (launch && (index >= launchPrograms ||
                       uint64_t(program.heapOffset) + program.bytes + kProgramPrefetchBytes > h.tier0[PackHeap]))
            say("program", index, "drawn by the launch set, outside tier 0");
        if (scanned[index])
            return;
        scanned[index] = true;
        const uint32_t end = program.dataBytes ? program.dataOffset : program.bytes;
        if (uint64_t(program.heapOffset) + end > h.section[PackHeap].bytes) {
            say("program", index, "its code past the heap section");
            return;
        }
        const bool writes =
            end > kCodeInSlot && programWritesMemory(heap + program.heapOffset + kCodeInSlot, end - kCodeInSlot);
        if (writes != bool(program.facts & kProgramWritesMemory))
            say("program", index, "its facts are not its code's");
        if (uint64_t(program.program) + kProgramHeaderBytes > h.section[PackHeap].bytes) {
            say("program", index, "its header past the heap section");
            return;
        }
        const uint8_t *header = heap + program.program;
        if ((program.facts & kHeaderFacts) != headerFacts(header))
            say("program", index, "its facts are not its header's");
        if (uint64_t(program.heapOffset) + program.bytes + kProgramPrefetchBytes <= h.tier0[PackHeap] &&
            slmPerWarpOfHeader(header) > h.tier0SlmPerWarp)
            say("program", index, "in tier 0 above the tier-0 local memory ceiling");
    };
    for (uint32_t i = 0; i < h.pipelines; i++) {
        if (i && i != launchRecords && p[i].keyHash < p[i - 1].keyHash)
            say("pipeline", p[i].keyHash, "out of keyHash order");
        const bool launch = i < launchRecords;
        drawn(p[i].vs, launch);
        drawn(p[i].fs, launch);
        if (p[i].fs != kNoProgram) {
            if (const PackSplit *s = pack.split(p[i].fs)) {
                drawn(s->fs, launch);
                if (s->prologue >= h.programs)
                    say("program", s->prologue, "past the programs");
                else if (launch &&
                         (s->prologue >= launchPrograms || uint64_t(programs[s->prologue].heapOffset) +
                                                                   programs[s->prologue].bytes + kProgramPrefetchBytes >
                                                               h.tier0[PackHeap]))
                    say("program", s->prologue, "drawn by the launch set, outside tier 0");
            }
        }
        if (tessellates(p[i])) {
            drawn(p[i].tessPrograms[0], launch);
            drawn(p[i].tessPrograms[1], launch);
        }
        for (uint32_t stage = 0; stage < 2; stage++) {
            uint32_t count = 0;
            const PackVariant *variants = pack.variants(p[i], stage, count);
            for (uint32_t v = 0; v < count; v++) {
                drawn(variants[v].program, launch);
                const PackProgram *program =
                    variants[v].program < h.programs ? &programs[variants[v].program] : nullptr;
                if (program && uint64_t(program->program) + kProgramHeaderBytes <= h.section[PackHeap].bytes &&
                    variants[v].memoryEntries !=
                        placedSetReads(heap + program->program, *program, SetEntries(p[i], pack.far(p[i]))))
                    say("pipeline", p[i].keyHash, "a variant's set reads not its code's");
            }
        }
        if (p[i].strideTwin && (p[i].strideTwin > h.pipelines || p[i].strideTwin - 1 == i ||
                                (launch && p[i].strideTwin - 1 >= launchRecords)))
            say("pipeline", p[i].keyHash, "its stride twin past the pipelines or after its tier");
        const uint32_t count = entryCount(p[i]);
        if (count > kSetEntriesMax)
            say("pipeline", p[i].keyHash, "of more set-0 entries than a record holds");
        else if (count > kPackEntries && uint64_t(farEntriesAt(p[i])) + farWords(count) > blobWords)
            say("pipeline", p[i].keyHash, "its far entries past the blob section");
        else if (const char *why = packRecordRefusal(pack, p[i]))
            say("pipeline", p[i].keyHash, why);
    }
    for (uint32_t i = 0; i < h.programs; i++)
        if (programFarAt(programs[i]) && programFarAt(programs[i]) - 1 + kProgramCbufs / 2 > blobWords)
            say("program", i, "its far uniform buffers past the blob section");
    const PackFastKey *fast = pack.section<PackFastKey>(PackFastKeys);
    const uint64_t launchKeys = pack.tier0Entries(PackFastKeys, sizeof(PackFastKey));
    for (uint64_t i = 0; i < pack.bytes(PackFastKeys) / sizeof(PackFastKey); i++) {
        const PackFastKey &k = fast[i];
        if (i && i != launchKeys && k.fast <= fast[i - 1].fast)
            say("fast key", k.fast, "out of order");
        if (!k.records || uint64_t(k.first) + k.records > h.pipelines) {
            say("fast key", k.fast, "its records past the pipelines");
            continue;
        }
        if ((i < launchKeys) != (k.first < launchRecords) ||
            (k.first < launchRecords) != (k.first + k.records <= launchRecords))
            say("fast key", k.fast, "its records of another tier than its own");
        const uint64_t state = p[k.first].keyHash;
        const bool whole = (!k.first || p[k.first - 1].keyHash != state) &&
                           (uint64_t(k.first) + k.records == h.pipelines || p[k.first + k.records].keyHash != state);
        bool one = true;
        for (uint32_t r = 1; r < k.records; r++)
            one &= p[k.first + r].keyHash == state;
        if (!whole || !one)
            say("fast key", k.fast, "not its state's records alone and all of them");
    }
    if (const uint64_t segBytes = pack.bytes(PackSegments)) {
        if (segBytes < sizeof(PackSegmentTable)) {
            say("segments", segBytes, "shorter than table header");
        } else {
            PackSegmentTable tbl = {};
            memcpy(&tbl, pack.section<uint8_t>(PackSegments), sizeof tbl);
            const uint64_t need = sizeof(PackSegmentTable) +
                                  uint64_t(tbl.pipelineCount) * (sizeof(PackSegment) + sizeof(uint32_t)) +
                                  uint64_t(tbl.variantComboCount) * sizeof(PackSegment);
            if (need != segBytes || tbl.pipelineCount != launchRecords) {
                say("segments", tbl.pipelineCount, "table size or count does not match tier 0");
            } else {
                const uint64_t tier0Heap = h.tier0[PackHeap];
                const uint64_t wordsEnd = uint64_t(tbl.wordsOffset) + tbl.wordsBytes;
                if (wordsEnd > tier0Heap)
                    say("segments", tbl.wordsOffset, "their words block outside tier-0 heap");
                const auto checkSeg = [&](const PackSegment &seg, uint64_t id) {
                    if (!seg.words)
                        return;
                    if ((seg.heapOffset & 15u) != 0 || uint64_t(seg.heapOffset) > tier0Heap ||
                        uint64_t(seg.words) * 4 > tier0Heap - seg.heapOffset)
                        say("segment", id, "unaligned or outside tier-0 heap");
                    else if (seg.heapOffset < tbl.wordsOffset || seg.heapOffset + uint64_t(seg.words) * 4 > wordsEnd)
                        say("segment", id, "outside the segments' words block");
                };
                uint64_t maxSlm = 0;
                const auto noteSlm = [&](uint32_t progIdx) {
                    if (progIdx < h.programs && uint64_t(programs[progIdx].program) + 16 <= h.section[PackHeap].bytes)
                        maxSlm = std::max(maxSlm, slmPerWarpOfHeader(heap + programs[progIdx].program));
                };
                for (uint32_t i = 0; i < tbl.pipelineCount; i++) {
                    if (const PackSegment *s = pack.pipelineSegment(i))
                        checkSeg(*s, i);
                    noteSlm(p[i].vs);
                    if (pixelStage(p[i]))
                        noteSlm(p[i].fs);
                    if (tessellates(p[i])) {
                        noteSlm(p[i].tessPrograms[0]);
                        noteSlm(p[i].tessPrograms[1]);
                    }
                    uint32_t c0 = 0, c1 = 0;
                    const PackVariant *v0 = pack.variants(p[i], 0, c0);
                    const PackVariant *v1 = pack.variants(p[i], 1, c1);
                    for (uint32_t k = 0; k < c0; k++)
                        noteSlm(v0[k].program);
                    for (uint32_t k = 0; k < c1; k++)
                        noteSlm(v1[k].program);
                    const uint32_t combos = (c0 || c1) ? (c0 + 1u) * (c1 + 1u) - 1u : 0u;
                    for (uint32_t slot = 0; slot < combos; slot++)
                        if (const PackSegment *vs = pack.variantSegment(i, slot))
                            checkSeg(*vs, (uint64_t(i) << 32) | slot);
                }
                if (uint64_t(h.tier0SlmPerWarp) < maxSlm)
                    say("slm", h.tier0SlmPerWarp, "below tier-0 graphics programs' requirement");
            }
        }
    }
    return out;
}

}  // namespace gm20b
