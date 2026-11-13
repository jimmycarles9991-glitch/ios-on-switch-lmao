// SPDX-License-Identifier: MIT
// SET_ITERATED_BLEND_OPTIMIZATION NOOP_SOURCE_RGBA_0000 (clb197.h): the ROP drops a pixel whose colour output is
// (0, 0, 0, 0) instead of blending it. A state turns it on only where that output leaves its target as it was under the
// state's own ROP blend (its blob's registers, not the Metal descriptor: m2v may blend in the program instead), for every
// value the target can hold:
//  - one target written (SET_CT_WRITE), target 0; the probe drew one target, and a pixel kill decided on one output would
//    drop the writes of the others;
//  - its ROP blends (SET_BLEND), colour and alpha alike: op ADD or REVERSE_SUBTRACT (0 + D and D - 0 are D; SUBTRACT
//    gives -D, MIN and MAX ignore the coefficients); destination coefficient ONE, ONE_MINUS_SRC_COLOR or
//    ONE_MINUS_SRC_ALPHA (each 1 with that source); source coefficient one that is finite with that source, so 0 times it
//    is 0: ZERO, ONE, SRC_*, ONE_MINUS_SRC_*, and on a normalized target, whose texels are finite, DST_*,
//    ONE_MINUS_DST_* and SRC_ALPHA_SATURATE too. Never the blend constant (any float the encoder sets) or source 1 (a
//    second output the kill does not look at);
//  - no depth or stencil attachment (whether a dropped pixel still writes depth is unmeasured), no alpha to coverage or
//    alpha to one (the source the ROP sees is not the program's), no target the program writes as a storage image.
// HW Test gpu.x1.blend_kills (probe d13, PROBELOG): source-over of a zero source onto RGBA16F holding -0, NaN, infinities
// and denormals and onto BGRA8 changed no bit with it on.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace raw {

constexpr uint32_t kIteratedBlendOptimization = 0x02f4;  // NVB197_SET_ITERATED_BLEND_OPTIMIZATION
constexpr uint32_t kNoopNever = 0, kNoopSourceRgba0000 = 1;

// MTLPixelFormats whose texels are finite: unorm, snorm and sRGB colour formats
inline bool normalizedFormat(uint32_t mtl)
{
    switch (mtl) {
    case 1:    // A8Unorm
    case 10:   // R8Unorm
    case 11:   // R8Unorm_sRGB
    case 12:   // R8Snorm
    case 20:   // R16Unorm
    case 22:   // R16Snorm
    case 30:   // RG8Unorm
    case 31:   // RG8Unorm_sRGB
    case 32:   // RG8Snorm
    case 40:   // B5G6R5Unorm
    case 41:   // A1BGR5Unorm
    case 42:   // ABGR4Unorm
    case 43:   // BGR5A1Unorm
    case 60:   // RG16Unorm
    case 62:   // RG16Snorm
    case 70:   // RGBA8Unorm
    case 71:   // RGBA8Unorm_sRGB
    case 72:   // RGBA8Snorm
    case 80:   // BGRA8Unorm
    case 81:   // BGRA8Unorm_sRGB
    case 90:   // RGB10A2Unorm
    case 94:   // BGR10A2Unorm
    case 110:  // RGBA16Unorm
    case 112:  // RGBA16Snorm
        return true;
    default:
        return false;
    }
}

inline bool noopBlendOp(uint32_t op)
{
    return op == 0x8006 || op == 0x800b;  // OGL_FUNC_ADD, OGL_FUNC_REVERSE_SUBTRACT
}

inline bool noopDestCoeff(uint32_t c)
{
    return c == 0x4001 || c == 0x4301 || c == 0x4303;  // ONE, ONE_MINUS_SRC_COLOR, ONE_MINUS_SRC_ALPHA
}

inline bool noopSourceCoeff(uint32_t c, bool normalized)
{
    if (c == 0x4000 || c == 0x4001 || (c >= 0x4300 && c <= 0x4303))  // ZERO, ONE, SRC_COLOR .. ONE_MINUS_SRC_ALPHA
        return true;
    return normalized && c >= 0x4304 && c <= 0x4308;  // DST_ALPHA .. ONE_MINUS_DST_COLOR, SRC_ALPHA_SATURATE
}

// The colour format of target `j` in a state key ("... c<j>=<MTLPixelFormat>:..."), 0 without one
inline uint32_t keyColorFormat(const char *keyString, uint32_t j)
{
    char field[8];
    snprintf(field, sizeof field, " c%u=", j);
    const char *at = keyString ? strstr(keyString, field) : nullptr;
    unsigned format = 0;
    return at && sscanf(at + strlen(field), "%u", &format) == 1 ? format : 0;
}

// SET_ITERATED_BLEND_OPTIMIZATION's value for a state: its blob's (method, value) pairs, its targets' SET_CT_WRITE values
// after them, its key and the targets its program writes as storage images
inline uint32_t iteratedBlendNoop(const uint32_t *blob, uint32_t dwords, const uint32_t writes[8],
                                  const char *keyString, uint8_t storageBits)
{
    for (uint32_t j = 1; j < 8; j++)
        if (writes[j] & 0x1111)
            return kNoopNever;
    if (!(writes[0] & 0x1111) || (storageBits & 1) || !keyString || !strstr(keyString, " ds=0/0 "))
        return kNoopNever;
    bool blends = false;
    uint32_t perTarget[6] = {};
    bool seen[6] = {};
    for (uint32_t k = 0; k + 1 < dwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        if (m == 0x153c && (v & 0x11))  // SET_ANTI_ALIAS_ALPHA_CONTROL: ALPHA_TO_COVERAGE, ALPHA_TO_ONE
            return kNoopNever;
        if (m == 0x1360)  // SET_BLEND(0)
            blends = v & 1;
        if (m >= 0x1e04 && m <= 0x1e18) {  // SET_BLEND_PER_TARGET_COLOR_OP(0) .. ALPHA_DEST_COEFF(0)
            perTarget[(m - 0x1e04) / 4] = v;
            seen[(m - 0x1e04) / 4] = true;
        }
    }
    for (bool s : seen)
        if (!s)
            return kNoopNever;
    const uint32_t format = keyColorFormat(keyString, 0);
    if (format >= 550 && format <= 555)  // extended range: m2v blends those in the program
        return kNoopNever;
    const bool normalized = normalizedFormat(format);
    for (uint32_t half = 0; half < 2; half++) {
        const uint32_t *t = perTarget + 3 * half;  // op, source coefficient, destination coefficient
        if (!noopBlendOp(t[0]) || !noopSourceCoeff(t[1], normalized) || !noopDestCoeff(t[2]))
            return kNoopNever;
    }
    return blends ? kNoopSourceRgba0000 : kNoopNever;
}

}  // namespace raw
