// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures over IOSurfaces (Metal formats 500 to 548), and the packed 10-bit planes' (588, 589,
// raw_packed_plane.h): which ones the raw path takes and how its conversion state (pack/ycbcr.metal) draws their image.
// The rules are m2v's (metal2vulkan-runtime ycbcr.rs `surface_planes`, metal2vulkan bind/ycbcr_convert.rs), whose texels
// are an Apple M4's measured reads.
#pragma once

#include <cstdint>

#include "raw_ycbcr_store.h"

namespace raw {

// The descriptor's and the surface's facts the rules read.
struct YcbcrFacts {
    uint32_t pixelFormat = 0;  // MTLPixelFormat
    uint32_t fourcc = 0;       // IOSurfaceGetPixelFormat
    uint32_t plane = 0;        // the plane the texture is made over
    uint32_t planes = 0;       // IOSurfaceGetPlaneCount
    uint32_t width = 0, height = 0;
    uint32_t surfaceWidth = 0, surfaceHeight = 0;  // IOSurfaceGetWidth/Height: plane 0's size
    uint64_t offset[2] = {};                       // each plane's first byte past the surface's base address
    uint32_t pitch[2] = {};
    uint64_t allocBytes = 0;
    bool simple = false;  // 2D, one level, one sample, not rotated, used to be sampled, viewed or rendered alone
    bool target = false;  // used as a render target too (raw_ycbcr_store.h)
    const char *matrix = nullptr;  // kIOSurfaceYCbCrMatrix, null without one
};

// Width, height, full range, red's, green's and blue's (kb, kr), the codes' read scale and dropped low bits, the shift that
// makes them 10-bit codes, the largest code, the halved axes (bit 0 across, bit 1 down) and whether a chroma plane is read.
constexpr int kYcbcrShapeWords = 15;

enum YcbcrOutput : uint8_t {
    YcbcrConverted = 1,  // RGBA16Float of v / 2048
    YcbcrSrgb = 2,       // RGBA32Float of step / 8192
    YcbcrCodes8 = 3,     // RGBA8Unorm: red Cr, green Y, blue Cb
    YcbcrCodes10 = 4,    // RGB10A2Unorm
    // the same of packed planes (R32Uint, three codes a word)
    YcbcrConvertedPacked = 5,
    YcbcrSrgbPacked = 6,
    YcbcrCodes10Packed = 7,
    // R10Unorm_PACKED's and RG10Unorm_PACKED's (raw_packed_plane.h): RGB10A2Unorm of a texel's codes, blue 0 (and green)
    YcbcrPackedPlane = 8,
};

struct YcbcrPlan {
    YcbcrOutput output = YcbcrConverted;
    bool nativePlanes = false;
    bool target = false;  // passes render into it (RawMetalYcbcrTargets.mm)
    // Texture::addressMap: 1 the wrapping modes clamp to the edge (4:2:0, 4:2:2), 2 clamps to zero and to the border do
    // (codes), 3 both, 0 neither (a 4:4:4 converted texture)
    uint8_t addressMap = 0;
    uint32_t image = 0;                         // MTLPixelFormat of the image the conversion draws
    uint32_t lumaFormat = 0, chromaFormat = 0;  // R8Unorm or R16Unorm, RG8Unorm or RG16Unorm; R32Uint packed
    uint64_t luma = 0, chroma = 0;              // offsets past the surface's base address
    uint32_t lumaPitch = 0, chromaPitch = 0;
    // the planes' textures' sizes: a texel a code (luma) or pair (chroma), or a word of three codes packed; chromaWidth 0:
    // no chroma plane read (a texture over plane 1: chroma codes 0)
    uint32_t lumaWidth = 0, chromaWidth = 0, chromaHeight = 0;
    uint32_t shape[kYcbcrShapeWords] = {};
    YcbcrStoreShape store;  // a target's store states' shape
};

// One of the two-plane YCbCr formats (Metal makes them over an IOSurface plane only)
bool isYcbcrFormat(uint32_t pixelFormat);

// Null when the raw path takes the texture (`out` its plan), else the rule that refused it.
const char *planYcbcr(const YcbcrFacts &f, YcbcrPlan &out);

}  // namespace raw
