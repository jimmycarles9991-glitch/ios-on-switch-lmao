// SPDX-License-Identifier: MIT
// nvdec's view of compressed targets (raw/runtime/raw_comp.h) over the mock GPU: a target is compressed when the mock's
// model of the kext (mock_comp.h) has its VA bound with a compressible kind and the 3D engine's compression enable for it
// is on. A clear of such a target that covers it whole, with every channel, to a value of the boot ZBC table writes no
// texels (counted with the bytes it skips); other clears of it are counted as misses. Decoder::vaKind is the mock's
// (pack/cap.cpp); without it (the runtime's capture) nothing is counted.
#pragma once
#include <cstdint>

namespace nvdec {

struct CompCounts {
    uint64_t draws = 0, clears = 0, zbc = 0, zbcBytes = 0;

    bool any() const
    {
        return draws || clears;
    }
};

inline bool compressibleKind(uint32_t k)
{
    return k == 0x51 || k == 0xce || k == 0xd8 || k == 0xdb || k == 0xe6 || k == 0xe9 || k == 0xf4 || k == 0xf5;
}

inline uint32_t compBytesPerTexel(uint32_t kind)
{
    switch (kind) {
    case 0xd8:
    case 0xdb:
    case 0x51:
        return 4;
    case 0xe6:
    case 0xe9:
    case 0xce:
        return 8;
    default:
        return 16;
    }
}

// The boot table's values for a colour target of NV format `fmt` (raw_comp.cpp zbcBootColor's, by the 3D engine's formats)
inline bool zbcBootColorNv(uint32_t fmt, const uint32_t w[4])
{
    const uint32_t one = 0x3f800000;
    if (!(w[0] | w[1] | w[2] | w[3]))
        return true;
    const bool white = w[0] == one && w[1] == one && w[2] == one && w[3] == one;
    const bool black = !(w[0] | w[1] | w[2]) && w[3] == one;
    if (fmt == 0xcf || fmt == 0xd0 || fmt == 0xd5 || fmt == 0xd6)
        return white || black;
    return fmt == 0xc0 && white;
}

inline bool zbcBootDepthBits(uint32_t bits)
{
    return bits == 0 || bits == 0x3f800000;
}

}  // namespace nvdec
