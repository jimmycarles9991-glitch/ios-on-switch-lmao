// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace raw {

struct FusionAttachment {
    uintptr_t identity = 0;
    uint64_t level = 0;
    bool native = false, loads = false, stores = false, resolves = false;
    uint64_t slice = 0;  // its slice (a 3D texture: its depth plane)
};

struct FusionPass {
    std::array<FusionAttachment, 10> attachments{};
    uint64_t width = 0, height = 0;
    uint32_t layers = 1;
    bool bodySafe = false;
};

inline bool canFusePasses(const FusionPass &first, const FusionPass &next)
{
    if (!first.bodySafe || !next.bodySafe || first.width != next.width || first.height != next.height ||
        first.layers != next.layers)
        return false;
    bool target = false;
    for (std::size_t i = 0; i < first.attachments.size(); ++i) {
        const auto &a = first.attachments[i], &b = next.attachments[i];
        if (a.identity != b.identity || a.level != b.level || a.slice != b.slice)
            return false;
        if (!a.identity)
            continue;
        target = true;
        if (!a.native || !b.native || !a.stores || !b.loads || a.resolves || b.resolves)
            return false;
    }
    return target;
}

inline bool fusionRangesOverlap(uint64_t a, uint64_t bytesA, uint64_t b, uint64_t bytesB)
{
    return a && b && bytesA && bytesB && (a <= b ? b - a < bytesA : a - b < bytesB);
}

}  // namespace raw
