// SPDX-License-Identifier: MIT
// What an idle arena kept non-volatile gives back of its pages (raw_arena.cpp trimArenas): the ranges no resource is
// placed in, in whole GMMU big pages. Its contents stay, as a NonVolatile heap promises: what lies in those ranges belongs
// to no resource (a resource placed there next starts undefined, as Metal's does). The rest is imported again in parts, at
// the same VAs, and a placement that reaches a hole binds that hole's pages again before it returns. A placement heap's
// offsets are its caller's, so it has no hole.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "raw_heap.h"

namespace raw {

struct ArenaRange {
    uint64_t offset, bytes;

    uint64_t end() const
    {
        return offset + bytes;
    }

    bool operator==(const ArenaRange &o) const
    {
        return offset == o.offset && bytes == o.bytes;
    }
};

// The free ranges of `place` within an arena of `arenaBytes` (the heap's size rounded up to `granule`, the part past it
// free too), each shrunk to the whole granules inside it
inline std::vector<ArenaRange> arenaHoles(const HeapPlacement &place, uint64_t arenaBytes, uint64_t granule)
{
    std::vector<ArenaRange> holes;
    if (place.explicitPlacement())
        return holes;
    place.forEachFree([&](uint64_t offset, uint64_t bytes) {
        const uint64_t end = offset + bytes >= place.bytes() ? arenaBytes : offset + bytes;
        const uint64_t first = (offset + granule - 1) / granule * granule, last = end / granule * granule;
        if (last > first)
            holes.push_back({first, last - first});
    });
    return holes;
}

// [0, arenaBytes) less the (sorted, disjoint) `holes`: the parts an arena keeps bound
inline std::vector<ArenaRange> arenaBacked(const std::vector<ArenaRange> &holes, uint64_t arenaBytes)
{
    std::vector<ArenaRange> backed;
    uint64_t at = 0;
    for (const ArenaRange &h : holes) {
        if (h.offset > at)
            backed.push_back({at, h.offset - at});
        at = h.end();
    }
    if (at < arenaBytes)
        backed.push_back({at, arenaBytes - at});
    return backed;
}

// The granules of `holes` a placement of [offset, offset + bytes) touches, taken out of `holes`: they are bound again
// before the placement is used
inline std::vector<ArenaRange> takeHoles(std::vector<ArenaRange> &holes, uint64_t offset, uint64_t bytes,
                                         uint64_t granule)
{
    std::vector<ArenaRange> taken, kept;
    const uint64_t first = offset / granule * granule, last = (offset + bytes + granule - 1) / granule * granule;
    for (const ArenaRange &h : holes) {
        const uint64_t from = std::max(h.offset, first), to = std::min(h.end(), last);
        if (from >= to) {
            kept.push_back(h);
            continue;
        }
        taken.push_back({from, to - from});
        if (h.offset < from)
            kept.push_back({h.offset, from - h.offset});
        if (to < h.end())
            kept.push_back({to, h.end() - to});
    }
    holes = std::move(kept);
    return taken;
}

inline uint64_t rangeBytes(const std::vector<ArenaRange> &ranges)
{
    uint64_t bytes = 0;
    for (const ArenaRange &r : ranges)
        bytes += r.bytes;
    return bytes;
}

}  // namespace raw
