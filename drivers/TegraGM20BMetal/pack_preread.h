// SPDX-License-Identifier: MIT
// The pack pages placing a record outside tier 0 reads (Device::placeRecord under Device::lock_): its tier-1 table
// entries and blob words, which no process keeps resident, and the 64 KiB grains of its programs, which the program
// heap's import wires. Each is a card read where the page is not in the page cache. Touched on the creating thread before
// the lock, they are page-cache hits under it, and no encoder waits behind the read.
#pragma once
#include <cstdint>
#include <cstring>
#include "pack.h"
#include "program_pages.h"
#include "set_entries.h"

namespace gm20b {

// Touches each `page` of the pack's [offset, offset + bytes) (file offsets, clipped to the file)
inline void prereadPackBytes(const Pack &pack, uint64_t offset, uint64_t bytes, uint64_t page)
{
    const uint64_t size = pack.size();
    if (!bytes || offset >= size)
        return;
    const uint64_t end = std::min(size, offset + bytes);
    const volatile uint8_t *base = reinterpret_cast<const uint8_t *>(pack.header());
    for (uint64_t at = offset / page * page; at < end; at += page)
        static_cast<void>(base[std::max(at, offset)]);
}

inline void prereadPackPointer(const Pack &pack, const void *at, uint64_t bytes, uint64_t page)
{
    if (!at)
        return;
    const auto *base = reinterpret_cast<const uint8_t *>(pack.header());
    prereadPackBytes(pack, uint64_t(static_cast<const uint8_t *>(at) - base), bytes, page);
}

// Program `index`'s record, its far table of uniform buffers and the grains of its code (with the instruction
// prefetch's reach past its end, which the import binds too)
inline void prereadPackProgram(const Pack &pack, uint32_t index, uint64_t page)
{
    if (index >= pack.header()->programs)
        return;
    const PackProgram &p = pack.section<PackProgram>(PackPrograms)[index];
    prereadPackPointer(pack, &p, sizeof p, page);
    prereadPackPointer(pack, pack.farUbos(p), kProgramCbufs * sizeof(uint16_t), page);
    const GrainSpan grains =
        programGrains(pack.header()->section[PackHeap].offset + p.heapOffset, p.bytes + kProgramPrefetchBytes);
    prereadPackBytes(pack, grains.first, grains.end - grains.first, page);
}

// What Device::placeRecord reads of `pp` itself: its programs, its variants and theirs, its split's, its registers, far
// block, argument tables, restore pairs, iterated blend and key
inline void prereadPackRecordAlone(const Pack &pack, const PackPipeline &pp, uint64_t page)
{
    prereadPackPointer(pack, &pp, sizeof pp, page);
    prereadPackProgram(pack, pp.vs, page);
    if (pixelStage(pp))
        prereadPackProgram(pack, pp.fs, page);
    if (tessellates(pp)) {
        prereadPackProgram(pack, pp.tessPrograms[0], page);
        prereadPackProgram(pack, pp.tessPrograms[1], page);
    }
    for (uint32_t stage = 0; stage < 2; stage++) {
        uint32_t count = 0;
        const PackVariant *variants = pack.variants(pp, stage, count);
        prereadPackPointer(pack, variants, uint64_t(count) * sizeof(PackVariant), page);
        for (uint32_t i = 0; variants && i < count; i++)
            prereadPackProgram(pack, variants[i].program, page);
    }
    if (const PackSplit *split = pixelStage(pp) ? pack.split(pp.fs) : nullptr) {
        prereadPackProgram(pack, split->fs, page);
        prereadPackProgram(pack, split->prologue, page);
    }
    const uint32_t *blob = pack.section<uint32_t>(PackBlob);
    prereadPackPointer(pack, blob + pp.blob, uint64_t(pp.blobDwords) * 4, page);
    prereadPackPointer(pack, blob + pp.restoreAt, uint64_t(pp.restoreDwords) * 4, page);
    prereadPackPointer(pack, pack.far(pp), uint64_t(farWords(entryCount(pp))) * 4, page);
    prereadPackPointer(pack, pack.spills(pp), (uint64_t(pp.spillWords[0]) + pp.spillWords[1]) * 4, page);
    prereadPackPointer(pack, pack.iterated(pp), sizeof(PackIteratedBlend), page);
    const char *key = pack.string(pp.keyString);
    prereadPackPointer(pack, key, strlen(key) + 1, page);
}

// The same of `pp` and of its stride twin (a record whose own twin is none: record.h strideTwinKey)
inline void prereadPackRecord(const Pack &pack, const PackPipeline &pp, uint64_t page)
{
    prereadPackRecordAlone(pack, pp, page);
    if (pp.strideTwin && pp.strideTwin <= pack.header()->pipelines)
        prereadPackRecordAlone(pack, pack.section<PackPipeline>(PackPipelines)[pp.strideTwin - 1], page);
}

}  // namespace gm20b
