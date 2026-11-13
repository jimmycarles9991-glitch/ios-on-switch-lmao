// SPDX-License-Identifier: MIT
// A stage-in kernel's push words (PackKernel::stageInRegionWord, stageInStride): m2v's StageInRegion field, the origin and
// size setStageInRegion: names, and each dynamic layout's StageInStride, its buffer's bind stride. A compute encoder's
// dispatch writes the encoder's (raw_compute.cpp); an indirect dispatch command, its own (raw_icb_compute.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include "raw_pack.h"

namespace raw {

constexpr uint32_t kNoBindStride = UINT32_MAX;

struct StageIn {
    std::array<uint32_t, 8> region{};  // origin xyz, 0, size xyz, 0
    std::array<uint32_t, 31> strides = unbound();

    static constexpr std::array<uint32_t, 31> unbound()
    {
        std::array<uint32_t, 31> s{};
        for (uint32_t &v : s)
            v = kNoBindStride;
        return s;
    }

    // setStageInRegion:'s origin and size, each component's low 32 bits
    void setRegion(const std::array<uint64_t, 6> &r)
    {
        region = {uint32_t(r[0]), uint32_t(r[1]), uint32_t(r[2]), 0, uint32_t(r[3]), uint32_t(r[4]), uint32_t(r[5]), 0};
    }
};

// Into the root table's push words `push` (0x700): a bind without a stride reads at 0, as Metal's does
inline void stageInWords(const PackKernel &r, const StageIn &s, uint32_t *push)
{
    if (r.stageInRegionWord && r.stageInRegionWord - 1u + s.region.size() <= 32)
        std::copy(s.region.begin(), s.region.end(), push + r.stageInRegionWord - 1);
    for (const auto &stride : r.stageInStride)
        if (stride[1] && stride[1] <= 32)
            push[stride[1] - 1] = stride[0] < 31 && s.strides[stride[0]] != kNoBindStride ? s.strides[stride[0]] : 0;
}

}  // namespace raw
