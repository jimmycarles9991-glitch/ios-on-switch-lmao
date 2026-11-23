// SPDX-License-Identifier: MIT
// The copy engine's remap words and the bytes a run of pitch rows takes, shared by the blit encoder's units (blit.cpp,
// blit_texels.cpp, blit_convert.cpp)
#pragma once
#include <cstdint>
#include "gm20b_gpu_internal.h"

namespace gm20b {
namespace {
// SET_REMAP_COMPONENTS of one-, two- or four-byte components (`size`): DST_X..W each SRC_X..W (0..3) or NO_WRITE (6), the
// source's and destination's component counts (clb0b5)
constexpr uint32_t kRemapNoWrite = NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE;

constexpr uint32_t remapOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

inline uint64_t rowStorageBytes(uint32_t pitch, uint32_t width, uint32_t height, uint32_t element)
{
    if (!width || !height || !element)
        return 0;
    const uint64_t rows = uint64_t(pitch) * (height - 1), last = uint64_t(width) * element;
    return last <= UINT64_MAX - rows ? rows + last : 0;
}
}  // namespace
}  // namespace gm20b
