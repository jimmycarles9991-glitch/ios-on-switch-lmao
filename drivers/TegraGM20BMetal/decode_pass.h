// SPDX-License-Identifier: MIT
// The decode of a decoded format's blocks (decoded.h) into its texture's stored image: one 3D-engine pass of the
// pack's decode state over a level's region, one texel a fragment, into the image's words viewed as integers.
#pragma once
#include <cstdint>
#include "decoded.h"

namespace gm20b {

class CommandBuffer;
class Device;
struct Buffer;
struct Texture;

// Where a region's blocks lie in a buffer: from `offset`, `rowBytes` a row of blocks apart (PVRTC: the whole level's
// blocks in rows, MTLBlitOptionRowLinearPVRTC's order)
struct DecodeSource {
    const Buffer *buffer = nullptr;
    uint64_t offset = 0;
    uint32_t rowBytes = 0;
};

// Texels [x, x + width) x [y, y + height) of level `level`, layer or 3D plane `layer`, of `image` (a decoded texture's
// stored image, its levels owner's) from `source` by `f`'s decode state; false: *why
bool decodePass(Device &d, CommandBuffer &rc, const Texture &image, const DecodedFormat &f, uint32_t level,
                uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const DecodeSource &source,
                const char **why);

}  // namespace gm20b
