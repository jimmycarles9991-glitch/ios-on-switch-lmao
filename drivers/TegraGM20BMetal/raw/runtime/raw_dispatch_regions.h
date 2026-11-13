// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>

namespace raw {
struct ComputeRegion {
    std::array<uint32_t, 3> local{}, groups{}, base{};
};

struct ComputeDispatch {
    enum Reason {
        Ready,
        Edge,
        Range
    } reason = Ready;

    std::array<uint64_t, 3> groups{};
    std::array<uint32_t, 3> grid{}, nominal{};
    std::array<ComputeRegion, 8> regions{};
    uint8_t count = 0, axis = 0;
    bool multi = false;
};

inline ComputeDispatch computeDispatch(std::array<uint64_t, 3> grid, const std::array<uint32_t, 3> &threads,
                                       bool perThread, uint32_t maxThreads = 1024)
{
    ComputeDispatch out;
    out.nominal = threads;
    uint64_t localThreads = 1;
    std::array<uint32_t, 3> full{}, edge{};
    for (uint8_t i = 0; i < 3; ++i) {
        if (!threads[i] || threads[i] > 1024) {
            out.reason = ComputeDispatch::Edge;
            out.axis = i;
            return out;
        }
        localThreads *= threads[i];
        if (localThreads > 1024 || localThreads > maxThreads) {
            out.reason = ComputeDispatch::Edge;
            out.axis = i;
            return out;
        }
        if (grid[i] > UINT32_MAX) {
            out.reason = ComputeDispatch::Range;
            return out;
        }
        const uint64_t total = perThread ? grid[i] : grid[i] * uint64_t(threads[i]);
        if (total > UINT32_MAX) {
            out.reason = ComputeDispatch::Range;
            return out;
        }
        out.grid[i] = uint32_t(total);
        full[i] = uint32_t(total / threads[i]);
        edge[i] = uint32_t(total % threads[i]);
        out.groups[i] = full[i] + (edge[i] != 0);
        out.multi |= edge[i] != 0;
        if (out.groups[i] > (i ? 0xffffu : 0x7fffffffu))
            out.reason = ComputeDispatch::Range;
    }
    if (out.reason != ComputeDispatch::Ready || !out.grid[0] || !out.grid[1] || !out.grid[2])
        return out;
    for (uint8_t mask = 0; mask < 8; ++mask) {
        ComputeRegion region;
        bool live = true;
        for (uint8_t axis = 0; axis < 3; ++axis) {
            const bool tail = mask & (1u << axis);
            region.local[axis] = tail ? edge[axis] : threads[axis];
            region.groups[axis] = tail ? 1 : full[axis];
            region.base[axis] = tail ? full[axis] : 0;
            live &= region.local[axis] && region.groups[axis];
        }
        if (live)
            out.regions[out.count++] = region;
    }
    return out;
}

inline bool computeRegionWords(const std::array<uint32_t, 3> &base, const std::array<uint32_t, 3> &grid,
                               const std::array<uint32_t, 3> &nominal, uint32_t out[8])
{
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!nominal[i] || nominal[i] > 1024)
            return false;
        out[i] = base[i];
        out[4 + i] = grid[i];
        packed |= (nominal[i] - 1) << (10 * i);
    }
    out[3] = packed;
    out[7] = 0;
    return true;
}
}  // namespace raw
