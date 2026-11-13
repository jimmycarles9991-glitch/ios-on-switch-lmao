// SPDX-License-Identifier: MIT
// A shared texture's image in its IOSurface (RawMetalSharedTextures.mm), as AGX keeps one: the surface holds the texture
// in the device's own layout of its descriptor (AGX's AGXTextureLayout of it, kIOSurfaceAllocSize its size), and every
// process that opens the surface lays the descriptor out again, from the creation properties. Here that layout is the one
// the raw path gives a texture of the descriptor in memory of its own (Device::createMipTexture, createArray, createVolume,
// createStoredMultisample, createLine, createBlockTextureOver: NIL's images), so the surface's bytes are the image the
// texture header, the copy engine and the render targets address. A one-level 2D one (Plain) is a surface of rows
// instead, which the kernel lays out at its first import (raw_shared_texture.h surfaceShape).
#pragma once
#include <algorithm>
#include <cstdint>
#include "../fx/fx_image.h"

namespace raw {

// MTLTextureType's values
enum TextureType : uint32_t {
    kType1D = 0,
    kType1DArray = 1,
    kType2D = 2,
    kType2DArray = 3,
    kType2DMultisample = 4,
    kTypeCube = 5,
    kTypeCubeArray = 6,
    kType3D = 7,
};

// The layers (slices) of a texture of MTLTextureType `type`: a cube's six faces, a cube array's six a cube, a 1D or 2D
// array's arrayLength; one for any other type (a 3D texture's planes are its depth)
constexpr uint32_t textureLayers(uint32_t type, uint32_t arrayLength)
{
    const uint32_t length = arrayLength ? arrayLength : 1;
    switch (type) {
    case kTypeCube:
        return 6;
    case kTypeCubeArray:
        return 6 * length;
    case kType1DArray:
    case kType2DArray:
        return length;
    default:
        return 1;
    }
}

struct SurfaceImage {
    enum Kind : uint8_t {
        Plain,
        Chain,   // a mipmapped 2D texture
        Layers,  // a 1D or 2D array, a cube or a cube array, any levels
        Volume,  // a 3D texture, any levels
        Samples,
        Line,    // a 1D texture
        Blocks,  // a block-compressed 2D texture or 2D array, any levels
    };

    Kind kind = Plain;
    uint8_t type = fx::kTexTwoD;  // its texture header's
    fx::MipChain layout;
};

struct SurfaceImageFacts {
    uint32_t type = kType2D, width = 0, height = 0, depth = 1, levels = 1, samples = 1, arrayLength = 1;
    uint32_t bpp =
        0;  // a texel's bytes, a block's for a block-compressed format; 0: a format the raw path has no header of
    uint8_t blockW = 1, blockH = 1;
    bool target = false;    // a colour format the raw path renders
    bool extended = false;  // extended range, which only a one-level 2D texture keeps
    bool rendered = false;  // a render target (or of Unknown usage): a 3D one is laid out one plane a block, as m2v's
};

// The image of a shared texture of `d`; null when there is one, else what the raw path keeps no texture of `d` in memory
inline const char *surfaceImage(const SurfaceImageFacts &d, SurfaceImage &out)
{
    out = {};
    if (!d.bpp)
        return "a format the raw path has no texture header for";
    const uint32_t levels = std::max(d.levels, 1u), samples = std::max(d.samples, 1u);
    const uint32_t arrayLength = std::max(d.arrayLength, 1u), depth = std::max(d.depth, 1u);
    if (d.blockW > 1 || d.blockH > 1) {
        if ((d.type != kType2D && d.type != kType2DArray) || samples > 1 || depth > 1)
            return "a block-compressed texture other than 2D or a 2D array";
        out.kind = SurfaceImage::Blocks;
        out.type = uint8_t(d.type == kType2DArray ? fx::kTexTwoDArray : fx::kTexTwoD);
        return fx::blockChainLayout(d.width, d.height, levels, d.type == kType2DArray ? arrayLength : 1, d.blockW,
                                    d.blockH, d.bpp, out.layout)
                   ? nullptr
                   : "an extent or level count NIL lays out no image of";
    }
    if (d.type == kType2D && levels == 1 && samples == 1)
        return nullptr;
    if (d.extended)
        return "an extended-range format, which only a one-level 2D texture keeps";
    if (levels > 1 && d.type != kType2D && !d.target)
        return "a mipmapped array, cube or 3D texture of a format the raw path does not render";
    bool laid = false;
    switch (d.type) {
    case kType2D:
        if (samples > 1)
            return "a 2D texture of more than one sample";
        if (!d.target)
            return "a mipmapped format the raw path does not render";
        out.kind = SurfaceImage::Chain;
        laid = fx::mipChainLayout(d.width, d.height, levels, d.bpp, out.layout);
        break;
    case kType1D:
        out.kind = SurfaceImage::Line;
        out.type = fx::kTexOneD;
        laid = d.height <= 1 && fx::mipChainLayout(d.width, 1, 1, d.bpp, out.layout);
        break;
    case kType1DArray:
    case kType2DArray:
    case kTypeCube:
    case kTypeCubeArray: {
        const bool cube = d.type == kTypeCube || d.type == kTypeCubeArray;
        if ((cube && d.width != d.height) || (d.type == kTypeCube && arrayLength > 1) ||
            (d.type == kType1DArray && d.height > 1))
            return "an array or cube of a shape Metal makes none of";
        out.kind = SurfaceImage::Layers;
        out.type = uint8_t(d.type == kType1DArray   ? fx::kTexOneDArray
                           : d.type == kType2DArray ? fx::kTexTwoDArray
                           : d.type == kTypeCube    ? fx::kTexCube
                                                    : fx::kTexCubeArray);
        laid = samples == 1 && fx::arrayLayout(d.width, std::max(d.height, 1u), textureLayers(d.type, arrayLength),
                                               levels, d.bpp, out.layout);
        break;
    }
    case kType3D:
        if (d.rendered && !d.target)
            return "a 3D render target of a format the raw path does not render";
        out.kind = SurfaceImage::Volume;
        out.type = fx::kTexThreeD;
        laid = samples == 1 && fx::volumeLayout(d.width, d.height, depth, levels, d.bpp, d.rendered, out.layout);
        break;
    case kType2DMultisample:
        if (!d.target || (samples != 2 && samples != 4))
            return "a multisampled texture the raw path does not render";
        out.kind = SurfaceImage::Samples;
        laid = fx::msLayout(d.width, d.height, samples, d.bpp, out.layout);
        break;
    default:
        return "a texture type the raw path keeps in no memory of its own";
    }
    return laid ? nullptr : "an extent NIL lays out no image of";
}

}  // namespace raw
