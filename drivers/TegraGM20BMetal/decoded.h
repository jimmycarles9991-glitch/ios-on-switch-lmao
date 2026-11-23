// SPDX-License-Identifier: MIT
// The formats GM20B's texture unit decodes none of that the identities make: PVRTC (160-167), Metal's ASTC footprints
// of 4x2 and 8x4 texels (185, 191, 203, 209) and ASTC HDR (221-236). A texture of one holds its texels decoded, in a
// stored format the GPU samples exactly as an M4 Max samples the asked one (pack/decode.metal,
// harness/decodeoracle.mm): its blocks are decoded on the GPU as they are uploaded (a blit's
// copyFromBuffer:toTexture:, replaceRegion through a blit of its own), and no copy of the blocks is kept.
#pragma once
#include <cstdint>
#include <cstring>

namespace gm20b {

// decode.metal's state, and astc_texel.h's form for ASTC
enum class DecodeState : uint8_t {
    Pvrtc,
    AstcUnorm8,
    AstcUnorm16,
    AstcHalf
};

struct DecodedFormat {
    uint32_t format, stored;
    uint8_t blockW, blockH, blockBytes;
    DecodeState state;
    bool opaque;  // PVRTC's RGB formats, which read alpha one
    // A never-written texel as the stored format holds it: the decode of a block of zeros, which Apple reads from a new
    // texture (PVRTC black, ASTC's error colour: magenta, an HDR NaN)
    uint32_t fresh[4];
};

constexpr uint32_t kRgba8Unorm = 70, kRgba8UnormSrgb = 71, kRgba16Unorm = 110, kRgba16Float = 115;
constexpr uint32_t kMagenta8 = 0xffff00ffu, kNanHalves = 0xffffffffu;

// clang-format off
inline constexpr DecodedFormat kDecodedFormats[] = {
    {160, kRgba8Unorm, 8, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {161, kRgba8UnormSrgb, 8, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {162, kRgba8Unorm, 4, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {163, kRgba8UnormSrgb, 4, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {164, kRgba8Unorm, 8, 4, 8, DecodeState::Pvrtc, false, {0}},
    {165, kRgba8UnormSrgb, 8, 4, 8, DecodeState::Pvrtc, false, {0}},
    {166, kRgba8Unorm, 4, 4, 8, DecodeState::Pvrtc, false, {0}},
    {167, kRgba8UnormSrgb, 4, 4, 8, DecodeState::Pvrtc, false, {0}},
    {185, kRgba8UnormSrgb, 4, 2, 16, DecodeState::AstcUnorm8, false, {kMagenta8}},
    {191, kRgba8UnormSrgb, 8, 4, 16, DecodeState::AstcUnorm8, false, {kMagenta8}},
    {203, kRgba16Unorm, 4, 2, 16, DecodeState::AstcUnorm16, false, {0x0000ffffu, 0xffffffffu}},
    {209, kRgba16Unorm, 8, 4, 16, DecodeState::AstcUnorm16, false, {0x0000ffffu, 0xffffffffu}},
    {221, kRgba16Float, 4, 2, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {222, kRgba16Float, 4, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {223, kRgba16Float, 5, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {224, kRgba16Float, 5, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {225, kRgba16Float, 6, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {226, kRgba16Float, 6, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {227, kRgba16Float, 8, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {228, kRgba16Float, 8, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {229, kRgba16Float, 8, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {230, kRgba16Float, 8, 8, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {231, kRgba16Float, 10, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {232, kRgba16Float, 10, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {233, kRgba16Float, 10, 8, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {234, kRgba16Float, 10, 10, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {235, kRgba16Float, 12, 10, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {236, kRgba16Float, 12, 12, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
};
// clang-format on

constexpr const DecodedFormat *decodedFormat(uint32_t format)
{
    for (const DecodedFormat &f : kDecodedFormats)
        if (f.format == format)
            return &f;
    return nullptr;
}

// The stored format's words as the integers a decode state writes them in: R32Uint, RGBA32Uint, RGBA16Uint
constexpr uint32_t decodeTargetFormat(DecodeState s)
{
    return s == DecodeState::AstcUnorm16 || s == DecodeState::AstcHalf ? 113 : 53;
}

constexpr const char *decodeStateName(DecodeState s)
{
    switch (s) {
    case DecodeState::Pvrtc:
        return "raw.decode.decode_pvrtc_frag";
    case DecodeState::AstcUnorm8:
        return "raw.decode.decode_astc_unorm8_frag";
    case DecodeState::AstcUnorm16:
        return "raw.decode.decode_astc_unorm16_frag";
    case DecodeState::AstcHalf:
        return "raw.decode.decode_astc_half_frag";
    }
    return nullptr;
}

// Whether a view of a texture of `source` as `view` reads the image the texture holds: the format itself, or PVRTC's
// sRGB and linear forms of one block size and alpha, whose codes are the same. Any other view Metal makes of a decoded
// format (ASTC's sRGB and linear forms, LDR and HDR of one footprint) decodes the same blocks into other values.
constexpr bool decodedViewShared(uint32_t source, uint32_t view)
{
    const DecodedFormat *s = decodedFormat(source), *v = decodedFormat(view);
    if (!s && !v)
        return true;
    if (!s || !v)
        return false;
    return source == view || (s->state == DecodeState::Pvrtc && (source ^ 1) == view);
}

// A format's ASTC footprint in Metal's order (4x2, 4x4, 5x4 .. 12x12) whatever its form (sRGB, LDR, HDR); -1 for
// any other format
constexpr int astcFootprint(uint32_t format)
{
    return format >= 185 && format <= 200   ? int(format - 185)
           : format >= 203 && format <= 218 ? int(format - 203)
           : format >= 221 && format <= 236 ? int(format - 221)
                                            : -1;
}

// Whether a texture of `format` keeps a copy of its blocks beside its decode: an ASTC one, which Metal lets a view read
// as another form of its footprint, whose decode of the same blocks differs (an M4 Max makes sRGB/linear views of every
// texture and LDR/HDR ones of every texture too, with validation's warning where it lacks PixelFormatView usage). Its
// readbacks give them back, as Apple's do. PVRTC's two forms read the same codes: it keeps none.
constexpr bool keepsBlocks(uint32_t format)
{
    const DecodedFormat *f = decodedFormat(format);
    return f && f->state != DecodeState::Pvrtc;
}

// Whether a view as `view` of a texture of `source` is a form of its own: the source keeps its blocks, and the view is
// another ASTC form of its footprint (a decoded one, or one GM20B decodes, held as those blocks), so it holds an image
// of its own decode, made from them
constexpr bool decodedOwnForm(uint32_t source, uint32_t view)
{
    return keepsBlocks(source) && view != source && astcFootprint(view) >= 0 &&
           astcFootprint(view) == astcFootprint(source);
}

// decode.metal's buffer 0: its DecodeStep
struct DecodeStep {
    uint32_t x, y, blockW, blockH, blocksX, blocksY, rowBytes, form, first;
};

// A level's blocks along a side of `texels`: at least one
constexpr uint32_t decodedBlocks(uint32_t texels, uint32_t block)
{
    return texels ? (texels + block - 1) / block : 1;
}

// Where a level's blocks lie in a texture's kept copy of them: the levels one after another, each its planes (layers,
// or a 3D level's depth planes) one after another, each its rows of blocks
struct KeptLevel {
    uint64_t offset, planeBytes;
    uint32_t rowBytes, planes;
};

inline KeptLevel keptLevel(const DecodedFormat &f, uint32_t width, uint32_t height, uint32_t depth, uint32_t layers,
                           uint32_t level)
{
    KeptLevel k = {};
    for (uint32_t l = 0; l <= level; l++) {
        k.offset += k.planeBytes * k.planes;
        const uint32_t w = width >> l ? width >> l : 1, h = height >> l ? height >> l : 1,
                       d = depth >> l ? depth >> l : 1;
        k.rowBytes = decodedBlocks(w, f.blockW) * f.blockBytes;
        k.planeBytes = uint64_t(k.rowBytes) * decodedBlocks(h, f.blockH);
        k.planes = depth > 1 ? d : layers;
    }
    return k;
}

inline uint64_t keptBytes(const DecodedFormat &f, uint32_t width, uint32_t height, uint32_t depth, uint32_t layers,
                          uint32_t levels)
{
    const KeptLevel k = keptLevel(f, width, height, depth, layers, levels ? levels - 1 : 0);
    return k.offset + k.planeBytes * k.planes;
}

// The step of a region whose first texel is (x, y) in a level of width x height, its blocks `rowBytes` apart from byte
// `first` of the bound buffer
inline DecodeStep decodeStep(const DecodedFormat &f, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                             uint32_t rowBytes, uint32_t first)
{
    // astc_texel.h kAstcUnorm8, kAstcUnorm12 (the state stores its 16-bit code), kAstcHalf; PVRTC: 1 for a format
    // without alpha
    const uint32_t form = f.state == DecodeState::Pvrtc         ? (f.opaque ? 1u : 0u)
                          : f.state == DecodeState::AstcUnorm8  ? 0u
                          : f.state == DecodeState::AstcUnorm16 ? 1u
                                                                : 2u;
    return {x,        y,    f.blockW, f.blockH, decodedBlocks(width, f.blockW), decodedBlocks(height, f.blockH),
            rowBytes, form, first};
}

// Block (x, y) of a PVRTC level's replaceRegion bytes: Morton order, y in the even bits, the longer side's extra bits
// on top (as an M4 Max reads them: test/decode_vectors.h)
constexpr uint32_t pvrtcMortonBlock(uint32_t x, uint32_t y, uint32_t blocksX, uint32_t blocksY)
{
    uint32_t xLog2 = 0, yLog2 = 0;
    while ((1u << xLog2) < blocksX)
        xLog2++;
    while ((1u << yLog2) < blocksY)
        yLog2++;
    const uint32_t shared = xLog2 < yLog2 ? xLog2 : yLog2;
    uint32_t index = 0;
    for (uint32_t i = 0; i < shared; i++)
        index |= ((y >> i) & 1) << (2 * i) | ((x >> i) & 1) << (2 * i + 1);
    return index | (xLog2 > yLog2 ? x >> shared : y >> shared) << (2 * shared);
}

// A PVRTC level's replaceRegion bytes (Morton order) into rows, a blit's MTLBlitOptionRowLinearPVRTC order
inline void pvrtcRows(const uint8_t *morton, uint32_t blocksX, uint32_t blocksY, uint8_t *rows)
{
    for (uint32_t y = 0; y < blocksY; y++)
        for (uint32_t x = 0; x < blocksX; x++)
            memcpy(rows + 8 * (uint64_t(y) * blocksX + x),
                   morton + 8 * uint64_t(pvrtcMortonBlock(x, y, blocksX, blocksY)), 8);
}

}  // namespace gm20b
