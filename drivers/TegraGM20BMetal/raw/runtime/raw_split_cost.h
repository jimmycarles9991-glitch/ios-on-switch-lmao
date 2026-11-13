// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace raw {

class SplitPassPlan {
    static constexpr uint64_t lanesPerCycle = 256, parallelWarpEstimate = 8, bytesPerCycle = 16;
    static constexpr uint64_t recordsPerLaunch = 1024, recordBytes = 272;

    struct Candidate {
        const void *key = nullptr;
        const PackSplit *split = nullptr;
        uint64_t draws = 0, pixels = 0, fixedCycles = 0, variableCycles = 0;
        uint64_t minimumPixels = UINT64_MAX;
        bool viable = false, admitted = false;
    };

    std::vector<Candidate> candidates_;
    std::vector<size_t> index_;
    mutable const void *lastKey_ = nullptr;
    mutable size_t lastIndex_ = SIZE_MAX;

    size_t slot(const void *key) const
    {
        const uint64_t hash = (uint64_t(reinterpret_cast<uintptr_t>(key)) >> 4) * 0x9e3779b97f4a7c15ull;
        size_t at = hash & (index_.size() - 1);
        while (index_[at] != SIZE_MAX && candidates_[index_[at]].key != key)
            at = (at + 1) & (index_.size() - 1);
        return at;
    }

    size_t find(const void *key) const
    {
        return index_.empty() ? SIZE_MAX : index_[slot(key)];
    }

    static uint64_t plus(uint64_t a, uint64_t b)
    {
        return b > UINT64_MAX - a ? UINT64_MAX : a + b;
    }

    static uint64_t times(uint64_t a, uint64_t b)
    {
        return a && b > UINT64_MAX / a ? UINT64_MAX : a * b;
    }

    static uint64_t ceilDiv(uint64_t value, uint64_t divisor)
    {
        return value / divisor + (value % divisor != 0);
    }

    static uint64_t savedWork(const PackSplit &split)
    {
        const auto &cost = split.cost;
        return cost.savedPixelCycles ? std::min<uint64_t>(cost.savedPixelInstructions,
                                                          uint64_t(cost.savedPixelCycles) * parallelWarpEstimate)
                                     : cost.savedPixelInstructions;
    }

    static uint64_t variableCycles(const PackSplit &split, uint64_t draws)
    {
        const uint64_t fullLaunches = draws / recordsPerLaunch, tail = draws % recordsPerLaunch;
        const uint64_t warpsPerGroup = ceilDiv(split.batchThreads, 32);
        const uint64_t fullWarps = times(ceilDiv(recordsPerLaunch, split.batchThreads), warpsPerGroup);
        const uint64_t tailWarps = times(ceilDiv(tail, split.batchThreads), warpsPerGroup);
        const uint64_t warps = plus(times(fullLaunches, fullWarps), tailWarps);
        const uint64_t instructions = times(warps, split.cost.prologueInstructions);
        const uint64_t waves = plus(times(fullLaunches, ceilDiv(fullWarps, parallelWarpEstimate)),
                                    ceilDiv(tailWarps, parallelWarpEstimate));
        const uint64_t latency = times(waves, split.cost.prologueWarpCycles);
        const uint64_t traffic = ceilDiv(times(draws, recordBytes + split.frontierBytes), bytesPerCycle);
        return plus(std::max(ceilDiv(instructions, parallelWarpEstimate), latency), traffic);
    }

    static uint64_t pixelsForCycles(const PackSplit &split, uint64_t cycles)
    {
        const uint64_t saved = savedWork(split);
        return saved && cycles <= UINT64_MAX / lanesPerCycle ? ceilDiv(cycles * lanesPerCycle, saved) : UINT64_MAX;
    }

public:
    void clear()
    {
        candidates_.clear();
        std::fill(index_.begin(), index_.end(), SIZE_MAX);
        lastKey_ = nullptr;
        lastIndex_ = SIZE_MAX;
    }

    void add(const void *key, const PackSplit &split, uint32_t draws, uint64_t pixelsPerDraw)
    {
        if (!key || !draws || !pixelsPerDraw || split.cost.version != 1 || !split.batchThreads)
            return;
        if (index_.empty() || (candidates_.size() + 1) * 2 > index_.size()) {
            index_.assign(std::max<size_t>(16, index_.size() * 2), SIZE_MAX);
            for (size_t i = 0; i < candidates_.size(); ++i)
                index_[slot(candidates_[i].key)] = i;
        }
        const size_t at = slot(key);
        if (index_[at] == SIZE_MAX) {
            index_[at] = candidates_.size();
            candidates_.push_back({key, &split});
        }
        auto &candidate = candidates_[index_[at]];
        candidate.draws = plus(candidate.draws, draws);
        candidate.pixels = plus(candidate.pixels, times(draws, pixelsPerDraw));
    }

    void finish()
    {
        lastKey_ = nullptr;
        lastIndex_ = SIZE_MAX;
        // Static instruction counts and clipped raster extents estimate work; launch/drain and
        // bandwidth constants are heuristics until device timing qualifies this compiler identity.
        uint64_t surplus = 0, drain = 0, viableDraws = 0;
        for (auto &candidate : candidates_) {
            const auto &split = *candidate.split;
            candidate.fixedCycles = times(ceilDiv(candidate.draws, recordsPerLaunch), split.cost.launchCycles);
            candidate.variableCycles = variableCycles(split, candidate.draws);
            const uint64_t cost = plus(candidate.fixedCycles, candidate.variableCycles);
            const uint64_t saved = times(candidate.pixels, savedWork(split)) / lanesPerCycle;
            candidate.viable = saved > cost;
            if (candidate.viable) {
                surplus = plus(surplus, saved - cost);
                drain = std::max<uint64_t>(drain, split.cost.drainCycles);
                viableDraws = plus(viableDraws, candidate.draws);
            }
        }
        for (auto &candidate : candidates_) {
            candidate.admitted = candidate.viable && surplus > drain;
            if (!candidate.admitted)
                continue;
            const auto &split = *candidate.split;
            const uint64_t amortized =
                plus(ceilDiv(plus(candidate.fixedCycles, candidate.variableCycles), candidate.draws),
                     ceilDiv(drain, viableDraws));
            const uint64_t marginal = plus(ceilDiv(split.cost.prologueWarpCycles, 32),
                                           ceilDiv(recordBytes + split.frontierBytes, bytesPerCycle));
            candidate.minimumPixels = pixelsForCycles(split, std::max(amortized, marginal));
        }
    }

    bool accepts(const void *key, const PackSplit &split, uint64_t pixels) const
    {
        if (split.cost.version > 1)
            return false;
        if (split.cost.version != 1 || !split.batchThreads)
            return pixels >= split.thresholdPixels;
        if (key != lastKey_) {
            lastKey_ = key;
            lastIndex_ = find(key);
        }
        if (lastIndex_ != SIZE_MAX) {
            const auto &candidate = candidates_[lastIndex_];
            return candidate.admitted && pixels >= candidate.minimumPixels;
        }
        const uint64_t cycles = plus(plus(split.cost.launchCycles, split.cost.drainCycles), variableCycles(split, 1));
        return pixels >= pixelsForCycles(split, cycles);
    }
};

}  // namespace raw
