// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BABI.h"
#include "pack.h"
#include "program_pages.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace gm20b {

inline constexpr uint32_t kPackWindowMaxCpuRanges = 8;
inline constexpr uint64_t kPackWindowMaxCpuWireBytes = 64ULL << 20;

static_assert(kPackWindowMaxCpuRanges <= TegraGM20BMaxPackCpuRanges, "pack CPU ranges fit ABI struct");

inline bool packWindowEligible(const PackHeader &h, uint64_t windowBytes)
{
    const uint64_t heapOff = h.section[PackHeap].offset;
    const uint64_t heapBytes = h.section[PackHeap].bytes;
    const uint64_t tier0Bytes = h.tier0[PackHeap];
    return h.version == kPackVersion && heapBytes != 0 && tier0Bytes != 0 && (heapOff % kPackTierAlign) == 0 &&
           (tier0Bytes % kPackTierAlign) == 0 && tier0Bytes <= heapBytes && heapBytes <= windowBytes;
}

inline bool packWindowMatchesReply(const PackHeader &h, const TegraGM20BAbiPackImageMapReply &reply,
                                   uint64_t expectedVa, uint64_t windowBytes)
{
    return reply.status == TegraGM20BPackStatusReady && reply.gpu_va == expectedVa && reply.stack == h.stack &&
           reply.tier0_heap_bytes != 0 && reply.tier0_heap_bytes == h.tier0[PackHeap] &&
           reply.heap_bytes == h.section[PackHeap].bytes && reply.tier0_heap_bytes <= windowBytes &&
           (reply.tier0_heap_bytes % kProgramGrain) == 0;
}

inline bool packWindowContainsProgram(const PackProgram &p, uint64_t packWindowBytes)
{
    return packWindowBytes != 0 && p.bytes != 0 && uint64_t(p.heapOffset) <= packWindowBytes &&
           uint64_t(p.bytes) + kProgramPrefetchBytes <= packWindowBytes - p.heapOffset;
}

inline bool packWindowContainsSegment(const PackSegment &s, uint64_t packWindowBytes)
{
    return packWindowBytes != 0 && s.words != 0 && (s.heapOffset & 15u) == 0 &&
           uint64_t(s.heapOffset) <= packWindowBytes && uint64_t(s.words) * 4 <= packWindowBytes - s.heapOffset;
}

inline uint32_t packWindowCollectCpuRanges(const uint8_t *packBase, const PackHeader &h, uint64_t vmPageBytes,
                                           TegraGM20BAbiPackCpuRange *out, uint32_t maxRanges = kPackWindowMaxCpuRanges,
                                           uint64_t maxBytes = kPackWindowMaxCpuWireBytes)
{
    if (!packBase || !out || !maxRanges || !maxBytes || !vmPageBytes || (vmPageBytes & (vmPageBytes - 1)) != 0)
        return 0;
    const uint32_t cap = std::min<uint32_t>(maxRanges, TegraGM20BMaxPackCpuRanges);
    const uint64_t pageMask = vmPageBytes - 1;
    uint64_t fileBytes = sizeof(PackHeader);
    for (const PackSection &s : h.section)
        if (s.offset <= UINT64_MAX - s.bytes)
            fileBytes = std::max(fileBytes, s.offset + s.bytes);
    const uint64_t mappedEnd = (fileBytes + pageMask) & ~pageMask;
    const uintptr_t base = uintptr_t(packBase);
    if ((base & pageMask) != 0)
        return 0;

    static constexpr PackSectionId kPriority[] = {
        PackFastKeys, PackPipelines, PackPrograms, PackSegments, PackVariants, PackBlob,
        PackStrings,  PackSplits,    PackIterated, PackInit,     PackBaseline, PackSamplers,
    };

    std::vector<std::pair<uint64_t, uint64_t>> wanted;
    for (PackSectionId id : kPriority) {
        // every creation the pack lacks searches the fast keys of both tiers before it is known to miss
        const uint64_t clamped = id == PackFastKeys ? h.section[id].bytes : std::min(h.tier0[id], h.section[id].bytes);
        if (clamped && h.section[id].offset <= UINT64_MAX - clamped)
            wanted.emplace_back(h.section[id].offset, clamped);
        PackSegmentTable table = {};
        if (id == PackSegments && h.section[id].bytes >= sizeof table &&
            h.section[id].offset <= fileBytes - sizeof table) {
            memcpy(&table, packBase + h.section[id].offset, sizeof table);
            const uint64_t heapEnd = h.section[PackHeap].offset + h.tier0[PackHeap];
            const uint64_t at = h.section[PackHeap].offset + table.wordsOffset;
            if (table.wordsBytes && at + table.wordsBytes <= heapEnd)
                wanted.emplace_back(at, table.wordsBytes);
        }
    }

    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    intervals.reserve(cap + 1);

    for (const auto &[from, bytesWanted] : wanted) {
        const uint64_t start = from & ~pageMask;
        uint64_t end = (from + bytesWanted + pageMask) & ~pageMask;
        if (end > mappedEnd)
            end = mappedEnd;
        if (end <= start)
            continue;

        std::vector<std::pair<uint64_t, uint64_t>> merged = intervals;
        merged.emplace_back(start, end);
        std::sort(merged.begin(), merged.end());
        std::vector<std::pair<uint64_t, uint64_t>> coalesced;
        coalesced.reserve(merged.size());
        uint64_t bytes = 0;
        for (const auto &[lo, hi] : merged) {
            if (!coalesced.empty() && lo <= coalesced.back().second) {
                if (hi > coalesced.back().second) {
                    bytes += hi - coalesced.back().second;
                    coalesced.back().second = hi;
                }
            } else {
                coalesced.emplace_back(lo, hi);
                bytes += hi - lo;
            }
        }
        if (coalesced.size() > cap || bytes > maxBytes)
            continue;
        intervals = std::move(coalesced);
    }

    const uint32_t count = uint32_t(intervals.size());
    for (uint32_t i = 0; i < count; ++i)
        out[i] = {uint64_t(base) + intervals[i].first, intervals[i].second - intervals[i].first};
    return count;
}

}  // namespace gm20b
