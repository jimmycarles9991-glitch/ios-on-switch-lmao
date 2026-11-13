// SPDX-License-Identifier: MIT
// What a new texture holds before anything writes it, as Apple's GPUs answer (an M4 Max under macOS 27.0.1, read back by
// blits: out/agent-scratch/x1-fresh/mac/fresh.mm; m2v's EVs ZIN-zp1-m4, ZIN-zf1-m4 and ZIN-zf2-m4 alike). A Private texture
// Metal keeps losslessly compressed reads one fixed texel in each level whose shorter side is 16 texels or more and zero in the
// levels below; every other Private texture reads zero. Clients that are correct on Apple's GPUs never read those texels
// before writing them, so a read that does is the runtime's bug: the pattern makes it visible (opaque magenta in an 8-bit
// colour format, NaN in a float one) where stale memory would hide it.
#pragma once
#include <cstdint>
#include <cstring>

namespace raw {

constexpr uint64_t kUsageShaderWrite = 0x2, kUsagePixelFormatView = 0x10, kUsageShaderAtomic = 0x20;
constexpr uint32_t kCompressedSide = 16;

struct FreshTexel {
    uint8_t bytes[16] = {};
    uint8_t size = 0;
};

// Each storage field all ones but the second from its lowest bit, zero (a float field's ones are a NaN); false for a format
// whose new texture reads zero however it is made (block-compressed, two-plane) or that was not measured
inline bool freshPatternTexel(uint32_t mtlPixelFormat, FreshTexel &out)
{
    static constexpr struct {
        uint32_t format;
        uint8_t size;
        uint8_t bytes[16];
    } kPatterns[] = {
        {1, 1, {0xff}},                                        // A8Unorm
        {10, 1, {0xff}},                                       // R8Unorm
        {11, 1, {0xff}},                                       // R8Unorm_sRGB
        {12, 1, {0xff}},                                       // R8Snorm
        {13, 1, {0xff}},                                       // R8Uint
        {14, 1, {0xff}},                                       // R8Sint
        {20, 2, {0xff, 0xff}},                                 // R16Unorm
        {22, 2, {0xff, 0xff}},                                 // R16Snorm
        {23, 2, {0xff, 0xff}},                                 // R16Uint
        {24, 2, {0xff, 0xff}},                                 // R16Sint
        {25, 2, {0xff, 0xff}},                                 // R16Float
        {30, 2, {0xff, 0x00}},                                 // RG8Unorm
        {31, 2, {0xff, 0x00}},                                 // RG8Unorm_sRGB
        {32, 2, {0xff, 0x00}},                                 // RG8Snorm
        {33, 2, {0xff, 0x00}},                                 // RG8Uint
        {34, 2, {0xff, 0x00}},                                 // RG8Sint
        {40, 2, {0x1f, 0xf8}},                                 // B5G6R5Unorm
        {41, 2, {0xc1, 0xff}},                                 // A1BGR5Unorm
        {42, 2, {0x0f, 0xff}},                                 // ABGR4Unorm
        {43, 2, {0x1f, 0xfc}},                                 // BGR5A1Unorm
        {53, 4, {0xff, 0xff, 0xff, 0xff}},                     // R32Uint
        {54, 4, {0xff, 0xff, 0xff, 0xff}},                     // R32Sint
        {55, 4, {0xff, 0xff, 0xff, 0xff}},                     // R32Float
        {60, 4, {0xff, 0xff, 0x00, 0x00}},                     // RG16Unorm
        {62, 4, {0xff, 0xff, 0x00, 0x00}},                     // RG16Snorm
        {63, 4, {0xff, 0xff, 0x00, 0x00}},                     // RG16Uint
        {64, 4, {0xff, 0xff, 0x00, 0x00}},                     // RG16Sint
        {65, 4, {0xff, 0xff, 0x00, 0x00}},                     // RG16Float
        {70, 4, {0xff, 0x00, 0xff, 0xff}},                     // RGBA8Unorm
        {71, 4, {0xff, 0x00, 0xff, 0xff}},                     // RGBA8Unorm_sRGB
        {72, 4, {0xff, 0x00, 0xff, 0xff}},                     // RGBA8Snorm
        {73, 4, {0xff, 0x00, 0xff, 0xff}},                     // RGBA8Uint
        {74, 4, {0xff, 0x00, 0xff, 0xff}},                     // RGBA8Sint
        {80, 4, {0xff, 0x00, 0xff, 0xff}},                     // BGRA8Unorm
        {81, 4, {0xff, 0x00, 0xff, 0xff}},                     // BGRA8Unorm_sRGB
        {90, 4, {0xff, 0x03, 0xf0, 0xff}},                     // RGB10A2Unorm
        {91, 4, {0xff, 0x03, 0xf0, 0xff}},                     // RGB10A2Uint
        {92, 4, {0xe0, 0x07, 0x00, 0xfc}},                     // RG11B10Float
        {93, 4, {0xff, 0x01, 0xfc, 0xff}},                     // RGB9E5Float
        {94, 4, {0xff, 0x03, 0xf0, 0xff}},                     // BGR10A2Unorm
        {103, 8, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}},        // RG32Uint
        {104, 8, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}},        // RG32Sint
        {105, 8, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}},        // RG32Float
        {110, 8, {0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff}},  // RGBA16Unorm
        {112, 8, {0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff}},  // RGBA16Snorm
        {113, 8, {0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff}},  // RGBA16Uint
        {114, 8, {0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff}},  // RGBA16Sint
        {115, 8, {0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff}},  // RGBA16Float
        {123, 16, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},  // RGBA32Uint
        {124, 16, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},  // RGBA32Sint
        {125, 16, {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},  // RGBA32Float
        {552, 8, {0xc0, 0xff, 0, 0, 0xc0, 0xff, 0x80, 0xdf}},                                             // BGRA10_XR
        {554, 4, {0xff, 0x03, 0xf0, 0xff}},                                                               // BGR10_XR
    };

    for (const auto &p : kPatterns)
        if (p.format == mtlPixelFormat) {
            out.size = p.size;
            memcpy(out.bytes, p.bytes, sizeof out.bytes);
            return true;
        }
    return false;
}

// Whether Metal keeps a colour texture losslessly compressed: allowGPUOptimizedContents, a level 0 whose shorter side is 16
// texels or more, and none of ShaderWrite, PixelFormatView and ShaderAtomic usage (MTLTextureUsageUnknown, 0, is every usage)
inline bool metalCompresses(uint64_t usage, bool allowGPUOptimizedContents, uint32_t width, uint32_t height)
{
    const uint64_t plain = kUsageShaderWrite | kUsagePixelFormatView | kUsageShaderAtomic;
    const uint32_t shorter = width < height ? width : height;
    return allowGPUOptimizedContents && shorter >= kCompressedSide && usage && !(usage & plain);
}

// The levels 0..n that read the pattern: those whose shorter side is 16 or more, of a texture Metal compresses
inline uint32_t patternLevels(uint64_t usage, bool allowGPUOptimizedContents, uint32_t width, uint32_t height,
                              uint32_t levels)
{
    if (!metalCompresses(usage, allowGPUOptimizedContents, width, height))
        return 0;
    uint32_t n = 0;
    while (n < levels) {
        const uint32_t w = width >> n ? width >> n : 1, h = height >> n ? height >> n : 1;
        if ((w < h ? w : h) < kCompressedSide)
            break;
        n++;
    }
    return n;
}

// The copy engine's fill of one texel value (clb0b5 SET_REMAP_CONST_A/B and SET_REMAP_COMPONENTS): each destination component
// a constant, `componentBytes` bytes each, `components` a texel; no source is read
struct CopyFill {
    uint32_t constA = 0, constB = 0;
    uint32_t remap = 0;
    uint8_t componentBytes = 0, components = 0;
};

constexpr uint32_t kRemapConstA = 4, kRemapConstB = 5;

inline CopyFill copyFillRemap(uint32_t a, uint32_t b, const uint8_t dst[4], uint8_t componentBytes, uint8_t components)
{
    CopyFill f;
    f.constA = a;
    f.constB = b;
    f.componentBytes = componentBytes;
    f.components = components;
    for (uint32_t i = 0; i < 4; i++)
        f.remap |= uint32_t(dst[i]) << (4 * i);
    f.remap |= uint32_t(componentBytes - 1) << 16 | uint32_t(components - 1) << 20 | uint32_t(components - 1) << 24;
    return f;
}

// A texel of `size` bytes (1, 2, 4, 8 or 16) as the copy engine's constants: at most two distinct 32-bit words; false where
// it holds more
inline bool copyFillOf(const uint8_t *texel, uint8_t size, CopyFill &out)
{
    const uint8_t aOnly[4] = {kRemapConstA, kRemapConstA, kRemapConstA, kRemapConstA};
    if (size == 1 || size == 2 || size == 4) {
        uint32_t v = 0;
        memcpy(&v, texel, size);
        out = copyFillRemap(v, 0, aOnly, size, 1);
        return true;
    }
    if (size != 8 && size != 16)
        return false;
    uint32_t words[4] = {};
    memcpy(words, texel, size);
    const uint8_t count = size / 4;
    uint32_t a = words[0], b = words[0];
    uint8_t dst[4] = {kRemapConstA, kRemapConstA, kRemapConstA, kRemapConstA};
    for (uint8_t i = 1; i < count; i++) {
        if (words[i] == a)
            continue;
        if (b != a && words[i] != b)
            return false;
        b = words[i];
        dst[i] = kRemapConstB;
    }
    out = copyFillRemap(a, b, dst, 4, count);
    return true;
}

// Whether the first pass that touches a new texture writes every texel of it before anything reads it: a stored attachment of
// level 0 of a one-level, one-layer texture, cleared over a render area Metal's clear reaches past to whole 32-pixel tiles (a
// pass without a draw call clears the whole level), and not read by the same pass
inline bool firstPassWritesAll(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, bool clearLoad,
                               bool stored, bool readInPass, uint32_t areaWidth, uint32_t areaHeight)
{
    constexpr uint32_t kTile = 32;
    const auto reaches = [](uint32_t area, uint32_t side) {
        return area >= side || (area + kTile - 1) / kTile * kTile >= side;
    };
    return levels == 1 && layers == 1 && clearLoad && stored && !readInPass && reaches(areaWidth, width) &&
           reaches(areaHeight, height);
}

}  // namespace raw
