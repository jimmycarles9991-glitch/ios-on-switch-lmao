// SPDX-License-Identifier: MIT
// computeframe's dispatch as both its modes bind it (computeframe.mm over the frontend, computeframe_raw.cpp over the raw
// compute encoder): every buffer slot 0..kSlots-1 tagged bytes (pack/probe.mm's scheme), textures of a pattern
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace computeframe {

constexpr uint32_t kSlots = 8, kTextures = 8, kTagBytes = 4096;
constexpr uint32_t kRgba8Unorm = 70;

inline std::vector<uint32_t> tag(uint32_t slot)
{
    std::vector<uint32_t> t(kTagBytes / 4);
    t[0] = 0xC0DE0000u | slot;
    for (uint32_t k = 1; k < t.size(); k++)
        t[k] = 0xA0000000u | slot << 16 | k;
    return t;
}

// Texture slot t: (16 + t) x (8 + t) RGBA8 texels of a pattern; sampler slot s: max LOD clamp 1 + s / 2
inline uint32_t texel(uint32_t t, uint32_t x, uint32_t y)
{
    return 0x01000000u * t ^ x * 0x00010307u ^ y * 0x00070103u;
}

inline std::vector<uint32_t> texels(uint32_t t)
{
    std::vector<uint32_t> v((16 + t) * (8 + t));
    for (uint32_t y = 0; y < 8 + t; y++)
        for (uint32_t x = 0; x < 16 + t; x++)
            v[y * (16 + t) + x] = texel(t, x, y);
    return v;
}

// FRAME_BIG_BUFFER=<bytes>: slot kBigSlot bound to a buffer of that many bytes instead of its tag (a storage range past
// a constant buffer's 64 KiB)
constexpr uint32_t kBigSlot = 1;

// FRAME_DISPATCHES=<n>: the dispatch n times in one serial encoder (the order between them)
inline uint32_t dispatches()
{
    const char *n = getenv("FRAME_DISPATCHES");
    return n ? std::max(1u, uint32_t(atoi(n))) : 1;
}

// FRAME_OFF16=<slot>:<offset>: that slot's tagged bytes from a zeroed buffer at an offset off 16 bytes (Core Animation's
// luma sums), so the descriptor starts below the tag and the residual is the slot's SlotBindOffset push word
inline bool off16(uint32_t &slot, uint32_t &offset)
{
    const char *text = getenv("FRAME_OFF16");
    return text && sscanf(text, "%u:%u", &slot, &offset) == 2 && slot < kSlots;
}

// FRAME_TEXTURES=<n>: textures (and samplers, unless FRAME_NO_SAMPLERS) bound at slots 0..n-1 only, as a kernel's caller binds
// them (the facade looks its kernel up by what is bound)
inline uint32_t boundTextures()
{
    const char *n = getenv("FRAME_TEXTURES");
    return n ? std::min(uint32_t(atoi(n)), kTextures) : kTextures;
}

inline uint32_t bigBuffer()
{
    const char *bytes = getenv("FRAME_BIG_BUFFER");
    return bytes ? uint32_t(strtoul(bytes, nullptr, 0)) : 0;
}

inline void fillBig(uint8_t *p, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
        p[i] = uint8_t(i * 7 + (i >> 9));
}

inline bool size3(const char *text, uint32_t v[3])
{
    return sscanf(text, "%u,%u,%u", &v[0], &v[1], &v[2]) == 3;
}

}  // namespace computeframe

// computeframe's raw and pack modes (computeframe_raw.cpp): argv as computeframe's main takes it
int computeframeRaw(char **argv);
