// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace raw {

// A float colour format's component size where its bits can be NaN or infinity (Metal's R16, RG16, RGBA16, R32, RG32 and
// RGBA32 Float), else 0. A non-finite texel a glass shader samples makes its whole band (0,0,0): the backdrop pyramid's
// blur and adaptive tint spread it and the blend writes NaN as 0 (vp: QuartzCore on the Mac fed a NaN pyramid).
inline uint32_t floatComponentBytes(uint32_t format)
{
    switch (format) {
    case 25:
    case 65:
    case 115:
        return 2;
    case 55:
    case 105:
    case 125:
        return 4;
    default:
        return 0;
    }
}

inline bool nonFiniteComponent(const uint8_t *at, uint32_t bytes)
{
    if (bytes == 2) {
        uint16_t h;
        memcpy(&h, at, 2);
        return (h & 0x7c00) == 0x7c00;
    }
    uint32_t f;
    memcpy(&f, at, 4);
    return (f & 0x7f800000) == 0x7f800000;
}

// Non-finite components among `bytes` of memory in one cache line out of every `stride` bytes (64-byte lines)
template <class Line>
uint32_t sampledNonFinite(const uint8_t *memory, uint64_t bytes, uint32_t componentBytes, uint64_t stride, Line &&line)
{
    uint32_t found = 0;
    for (uint64_t at = 0; componentBytes && at + 64 <= bytes; at += stride) {
        line(memory + at);
        for (uint32_t c = 0; c < 64; c += componentBytes)
            found += nonFiniteComponent(memory + at + c, componentBytes);
    }
    return found;
}

// A glass state's uniform bytes: their 4-byte-aligned words that are non-finite floats, their 2-byte-aligned halves that are
// non-finite (a float's low half can be one by chance), and the offset of the first non-finite word, else of the first half
struct NonFiniteUniforms {
    uint32_t words = 0, halves = 0;
    uint64_t first = UINT64_MAX;
};

inline NonFiniteUniforms nonFiniteUniforms(const uint8_t *bytes, uint64_t length)
{
    NonFiniteUniforms found;
    uint64_t firstHalf = UINT64_MAX;
    for (uint64_t at = 0; at + 2 <= length; at += 2) {
        if (nonFiniteComponent(bytes + at, 2)) {
            found.halves++;
            firstHalf = std::min(firstHalf, at);
        }
        if (at % 4 == 0 && at + 4 <= length && nonFiniteComponent(bytes + at, 4)) {
            found.words++;
            found.first = std::min(found.first, at);
        }
    }
    if (!found.words)
        found.first = firstHalf;
    return found;
}

}  // namespace raw
