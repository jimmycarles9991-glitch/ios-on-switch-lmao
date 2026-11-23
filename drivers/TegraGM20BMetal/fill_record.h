// SPDX-License-Identifier: MIT
// A texture fill as its blit encoder records it (OpFillTexture: c the call's level | its slice << 32, x its first extra
// word): what it writes into each image, decided at record (TegraGM20BMetalBlitFill.mm), which the copy engine or the 2D engine
// then writes (TegraGM20BMetalEncodeBlit.mm)
#pragma once
#include <cstdint>
#include <cstring>

namespace gm20b {

struct FillRecord {
    // the units it writes: a compressed format's blocks, else texels; a 3D level's planes z0 .. z1, one image elsewhere
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, z0 = 0, z1 = 1;
    uint32_t layer = 0;      // the layer of an array or cube it writes
    uint8_t texel[16] = {};  // one unit's bytes as the fill leaves them
    uint8_t bytes = 0;       // the unit's bytes
    uint8_t words = 0;       // the unit's 4-byte words the fill writes (bit k: word k); 0: every byte
};

// Its words follow the call's own (the region's origin and size, the colour's four floats' bits), which a replay reads
constexpr uint32_t kFillCallWords = 8, kFillRecordWords = 6;

inline void packFill(const FillRecord &f, uint64_t out[kFillRecordWords])
{
    out[0] = f.x0 | uint64_t(f.y0) << 32;
    out[1] = f.x1 | uint64_t(f.y1) << 32;
    out[2] = f.z0 | uint64_t(f.z1) << 32;
    memcpy(&out[3], f.texel, sizeof f.texel);
    out[5] = f.bytes | uint64_t(f.words) << 8 | uint64_t(f.layer) << 32;
}

inline FillRecord unpackFill(const uint64_t in[kFillRecordWords])
{
    FillRecord f;
    f.x0 = uint32_t(in[0]), f.y0 = uint32_t(in[0] >> 32);
    f.x1 = uint32_t(in[1]), f.y1 = uint32_t(in[1] >> 32);
    f.z0 = uint32_t(in[2]), f.z1 = uint32_t(in[2] >> 32);
    memcpy(f.texel, &in[3], sizeof f.texel);
    f.bytes = uint8_t(in[5]);
    f.words = uint8_t(in[5] >> 8);
    f.layer = uint32_t(in[5] >> 32);
    return f;
}

}  // namespace gm20b
