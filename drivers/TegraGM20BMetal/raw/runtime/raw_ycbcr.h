// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures over IOSurfaces (Metal formats 500 to 548), one-plane 4:2:2 ones (501, 521), and the packed
// 10-bit planes' (588, 589, raw_packed_plane.h): which ones the raw path takes and how its conversion state (pack/ycbcr.metal) draws their image.
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
    // Metal reads and stores the top-left corner of the planes' rows: no taller than the surface, as wide as its rows hold
    uint32_t width = 0, height = 0;
    uint32_t surfaceWidth = 0, surfaceHeight = 0;  // IOSurfaceGetWidth/Height: plane 0's size
    uint64_t offset[2] = {};                       // each plane's first byte past the surface's base address
    uint32_t pitch[2] = {};
    uint64_t allocBytes = 0;
    uint64_t mappedBytes = 0;      // the allocation's whole pages, which the GPU maps (0: allocBytes)
    bool simple = false;           // 2D, one level, one sample, not rotated
    bool target = false;           // may be rendered into (ycbcrUsageRenders; raw_ycbcr_store.h)
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
    // a converted or sRGB format's under kIOSurfaceYCbCrMatrix P3_D65 or DCI_P3: the codes images (codes8, codes10, packed
    // codes10), whose header reads red Y, green Cb, blue Cr (ycbcrLumaFirst)
    YcbcrLumaFirst8 = 9,
    YcbcrLumaFirst10 = 10,
    YcbcrLumaFirst10Packed = 11,
};

inline bool ycbcrLumaFirst(uint8_t output)
{
    return output >= YcbcrLumaFirst8 && output <= YcbcrLumaFirst10Packed;
}

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
    uint64_t readable = 0;  // the bytes past the surface's base the planes' rows may reach
    // a one-plane 4:2:2 format's (501, 521): luma and lumaPitch its plane of Y0 Cb Y1 Cr texel pairs, which the copy engine
    // splits into pool images of the luma and chroma textures' sizes (BlitEncoder::splitPlane) for a 4:2:2 conversion
    bool onePlane = false;
    // the planes' textures' sizes: a texel a code (luma) or pair (chroma), or a word of three codes packed; chromaWidth 0:
    // no chroma plane read (a texture over plane 1: chroma codes 0)
    uint32_t lumaWidth = 0, chromaWidth = 0, chromaHeight = 0;
    uint32_t shape[kYcbcrShapeWords] = {};
    YcbcrStoreShape store;  // a target's store states' shape
};

// One of the two-plane YCbCr formats (Metal makes them over an IOSurface plane only)
bool isYcbcrFormat(uint32_t pixelFormat);

// Whether a texture of `usage` (MTLTextureUsage, Unknown every use) may be rendered into. Metal makes these textures of any
// usage; binding one to a shader argument with write access is its validation error ("Non-writeable texture format ... with
// write access enabled", validateComputeFunctionArguments), and an M4 Max without validation drops the writes.
inline bool ycbcrUsageRenders(uint64_t usage)
{
    constexpr uint64_t kRenderTarget = 4;
    return !usage || (usage & kRenderTarget);
}

// Whether Metal makes no texture of `usage` over a surface (MTLDevice.h newTextureWithDescriptor:iosurface:plane:, nullable): a
// format of chroma halved across or down, of an odd width or height, that the GPU may write (render target, shader write or
// Unknown usage). An M4 Max answers nil, and makes one used only to be read.
bool ycbcrMetalRefuses(uint32_t pixelFormat, uint32_t width, uint32_t height, uint64_t usage);

// Null when the raw path takes the texture (`out` its plan), else the rule that refused it.
const char *planYcbcr(const YcbcrFacts &f, YcbcrPlan &out);

}  // namespace raw
