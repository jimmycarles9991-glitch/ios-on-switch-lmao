// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures over IOSurfaces (Metal formats 500 to 548), one-plane ones (4:2:2 501, 521, 562; 4:4:4 504,
// 524), and the packed 10-bit planes' (588, 589, packed_plane.h): which ones the driver takes and how its conversion
// state (pack/ycbcr.metal) draws their image.
// The rules are m2v's (metal2vulkan-runtime ycbcr.rs `surface_planes`, metal2vulkan bind/ycbcr_convert.rs), whose texels
// are an Apple M4's measured reads.
#pragma once

#include <cstdint>

#include "ycbcr_store.h"

namespace gm20b {

// The descriptor's and the surface's facts the rules read.
struct YcbcrFacts {
    uint32_t pixelFormat = 0;  // MTLPixelFormat
    uint32_t fourcc = 0;       // IOSurfaceGetPixelFormat
    // the plane the texture is made over and the surface's IOSurfaceGetPlaneCount; offset and pitch hold planes 0 and 1. A
    // two-plane format reads its plane's codes as luma and the next plane's as chroma, codes 0 where there is none (the last
    // plane, or a surface of one or none), so the facade gives a texture over plane k of more than two planes k and k + 1 as
    // planes 0 and 1 of two, or plane k alone as the one plane of one (TegraGM20BMetalSurfaceTextures.mm ycbcrPlan)
    uint32_t plane = 0;
    uint32_t planes = 0;
    // Metal reads and stores the top-left corner of the planes' rows: no taller than the surface, as wide as its rows hold
    uint32_t width = 0, height = 0;
    uint32_t surfaceWidth = 0, surfaceHeight = 0;  // IOSurfaceGetWidth/Height: plane 0's size
    uint64_t offset[2] = {};                       // each plane's first byte past the surface's base address
    uint32_t pitch[2] = {};
    uint64_t allocBytes = 0;
    uint64_t mappedBytes = 0;      // the allocation's whole pages, which the GPU maps (0: allocBytes)
    bool simple = false;           // 2D (or a texture buffer), one sample, not rotated
    uint32_t levels = 1;           // mipmapLevelCount
    bool buffer = false;           // a texture buffer: an M4 Max reads one as a 2D texture's row 0 (m2v-textures13
                                   // probes/ytbuf/ytread.txt)
    bool target = false;           // may be rendered into (ycbcr_store.h)
    const char *matrix = nullptr;  // kIOSurfaceYCbCrMatrix, null without one
    bool sparseTail = false;  // the kernel maps VA sparse (kernel.h Kernel::sparse): rows may run past the last page
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
    // R10Unorm_PACKED's and RG10Unorm_PACKED's (packed_plane.h): RGB10A2Unorm of a texel's codes, blue 0 (and green)
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
    bool target = false;  // passes render into it (TegraGM20BMetalYcbcrTargets.mm)
    // Texture::addressMap: 1 the wrapping modes clamp to the edge (4:2:0, 4:2:2), 2 clamps to zero and to the border do
    // (codes), 3 both, 0 neither (a 4:4:4 converted texture)
    uint8_t addressMap = 0;
    uint32_t image = 0;                         // MTLPixelFormat of the image the conversion draws
    uint32_t lumaFormat = 0, chromaFormat = 0;  // R8Unorm or R16Unorm, RG8Unorm or RG16Unorm; R32Uint packed
    uint64_t luma = 0, chroma = 0;              // offsets past the surface's base address
    uint32_t lumaPitch = 0, chromaPitch = 0;
    // a two-plane format's planes' rows as a GPU store lands them where not at their pitch (Texture::storePitch: rows under
    // 16 bytes, which an M4 Max reads 1 MiB apart and stores 16 apart: m2v-textures12 probes/f2/targets-full.txt), else 0
    uint32_t lumaStorePitch = 0, chromaStorePitch = 0;
    // a target whose two planes' rows, as its stores land them, reach one another's bytes: each plane rendered into a pool
    // image of its own, the span of both drawn from them by the pack's meet state (ycbcr_meet.h)
    bool meet = false;
    uint64_t readable = 0;  // the bytes past the surface's base the planes' rows may reach
    // the rows run past the allocation's last page, to `readable`: read through a window of their own whose tail reads 0
    // (Device::openSparseWindow)
    bool window = false;
    // a one-plane 4:2:2 format's (501, 521, 562): luma and lumaPitch its plane of texel pairs, Y0 Cb Y1 Cr or, pairsReversed,
    // Cb Y0 Cr Y1 (562), which the copy engine splits into pool images of the luma and chroma textures' sizes
    // (BlitEncoder::splitPlane) for a 4:2:2 conversion
    bool onePlane = false;
    bool pairsReversed = false;
    // a one-plane 4:4:4 format's (504, 524): a word a texel, Y, Cb and Cr ten bits each from bit 0, which luma reads as
    // RGB10A2Unorm's red and chroma, the same plane, as its green and blue (a header swizzle)
    bool chromaGreenBlue = false;
    // the planes' textures' sizes: a texel a code (luma) or pair (chroma), or a word of three codes packed; chromaWidth 0:
    // no chroma plane read (a texture over plane 1: chroma codes 0)
    uint32_t lumaWidth = 0, chromaWidth = 0, chromaHeight = 0;
    // the image's levels: several only for R10Unorm_PACKED and RG10Unorm_PACKED, every level of which reads level 0
    // (packed_plane.h, Texture::levelZero)
    uint32_t levels = 1;
    // a texture buffer's image: its one row, linear, under a texel buffer's header (TEXHEAD_1D addresses linear memory
    // alone: clb097tex.h), which its conversion draws into as a row target
    bool buffer = false;
    uint32_t shape[kYcbcrShapeWords] = {};
    YcbcrStoreShape store;  // a target's store states' shape
};

// One of the two-plane YCbCr formats (Metal makes them over an IOSurface plane only)
// The bytes between a plane's rows as Metal's GPU reads them: its bytesPerRow in 16-byte units less one, kept to 16 bits.
// An M4 Max reads YCbCr planes of rows 16 or more at their rows rounded down to 16, of rows under 16 1 MiB apart, of rows
// 1 MiB + 16 and 1 MiB + 32 at 16 and 32, of rows 2 MiB + 48 at 48 (two-plane and one-plane formats, plane by plane)
constexpr uint32_t ycbcrPlaneStride(uint32_t bytesPerRow)
{
    return 16u * ((((bytesPerRow >> 4) - 1u) & 0xffffu) + 1u);
}

// The bytes between a plane's rows as Metal's GPU stores them: rows under 16 bytes 16 apart, others rounded down to 16 (an
// M4 Max's passes and kernels into RGB10A8_2P_XR10 planes: m2v-textures11 probes/h1/rows550.txt)
constexpr uint32_t planeStoreStride(uint32_t bytesPerRow)
{
    return bytesPerRow < 16 ? 16u : bytesPerRow & ~15u;
}

bool isYcbcrFormat(uint32_t pixelFormat);

// A plan's refusal of planes whose rows run past the allocation's last page on a kernel that maps no VA sparse (an M4 Max
// reads 0 there): the import's step, not a form (TegraGM20BMetalSurfaceTextures.mm gm20b:tex.surf.imp)
inline constexpr char kRowsPastWithoutSparse[] = "rows past the allocation's last page (an M4 Max reads 0 there)";

// A YCbCr or packed 10-bit texture buffer of `texels` as an M4 Max reads one: texel i at its plane's (i % 16384,
// i / 16384), 20000 texels of 500, 503, 540, 588 and 589 alike (the wide texel-buffer probe, macOS 27.2.1). Its conversion
// draws that 2D texture's extent, rows 16384 wide, which a GM20B colour target holds and its buffer header reads back to
// back
constexpr uint32_t kYcbcrBufferRow = 16384;

constexpr void ycbcrBufferExtent(uint32_t texels, uint32_t &width, uint32_t &height)
{
    width = texels < kYcbcrBufferRow ? texels : kYcbcrBufferRow;
    height = (texels + kYcbcrBufferRow - 1) / kYcbcrBufferRow;
}

// Whether `pixelFormat` is a two-plane YCbCr format, and the bytes of each texel Metal's CPU copies of a texture of it move:
// its luma code's alone (1, or 2 for unpacked 10-bit codes) from its plane at the plane's own offset and rows, never its
// chroma; 0 for a packed 10-bit format, of which they move nothing (an M4 Max, validation on and off, rows under 16 bytes
// and over: m2v-textures13 probes/c4misc/RESULT.txt P1, P2)
bool ycbcrCpuLuma(uint32_t pixelFormat, uint32_t &bytes);

// Whether Metal makes no texture of `usage` over a surface (MTLDevice.h newTextureWithDescriptor:iosurface:plane:, nullable): a
// format of chroma halved across or down, of an odd width or height, that the GPU may write (render target, shader write or
// Unknown usage). An M4 Max answers nil, and makes one used only to be read.
bool ycbcrMetalRefuses(uint32_t pixelFormat, uint32_t width, uint32_t height, uint64_t usage);

// Null when the driver takes the texture (`out` its plan), else the rule that refused it.
const char *planYcbcr(const YcbcrFacts &f, YcbcrPlan &out);

}  // namespace gm20b
