// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace raw {

constexpr uint32_t kTransientColorTag = 9;

inline std::array<uint32_t, 8> transientColorTags(uint8_t scratch)
{
    std::array<uint32_t, 8> tags{};
    uint32_t next = kTransientColorTag;
    for (uint32_t slot = 0; slot < tags.size(); ++slot)
        if (scratch & (1u << slot))
            tags[slot] = next++;
    return tags;
}

template <class Texture> bool fusionTargetEligible(const Texture &texture, uint64_t level)
{
    return texture.va && !texture.memoryless && !texture.sampled && !texture.standIn && !texture.planarStandIn &&
           !texture.converted() && !level;
}

template <class Texture> uint64_t fusionImageBytes(const Texture &texture)
{
    const auto multiply = [](uint64_t a, uint64_t b) {
        return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
    };
    const uint64_t rows = multiply(uint64_t(texture.pitch) * texture.height, texture.depth);
    const uint64_t layers = multiply(std::max(rows, texture.layerBytes), texture.layers);
    const uint64_t chain = texture.chain ? texture.chain->layout.bytes : 0;
    return std::max({texture.span.bytes, layers, chain});
}

// A CE shadow cannot use a compressed binding left by a prior memoryless pass.
inline bool backingCompressionMatches(bool compressed, const void *memory, const void *latest,
                                      bool compressionRequested)
{
    return compressionRequested ? compressed && memory == latest : !compressed;
}

template <class Clears>
bool attachmentStartsCleared(uint32_t target, uint32_t width, uint32_t height, const Clears &clears)
{
    return std::any_of(clears.begin(), clears.end(), [&](const auto &clear) {
        return clear.target == target && !clear.x0 && !clear.y0 && clear.x1 >= width && clear.y1 >= height;
    });
}

}  // namespace raw
