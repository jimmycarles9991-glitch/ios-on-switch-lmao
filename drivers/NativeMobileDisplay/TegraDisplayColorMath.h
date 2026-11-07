// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display's colour state as IOMobileFramebuffer receives it, turned into what DC A's colour management unit (CMU)
// holds: LUT1 (8-bit sRGB to 12-bit linear, 256 entries), a 3x3 CSC in linear light, LUT2 (12-bit linear in 960 buckets to
// 8-bit sRGB). One LUT1 and one LUT2 serve all three channels, and the CSC has no offsets. Integer only: the kernel has
// no floating point. C++, header only, for the driver and its host test.
#pragma once
#include <stdint.h>
#include "TegraDisplayColorTables.h"

namespace switch_color {

// IOMobileFramebuffer's matrices: S31.32 (QuartzCore converts with fcvtzs #32), rows are output channels.
using Q32 = int64_t;
constexpr Q32 kOne = Q32(1) << 32;

struct Matrix {
    Q32 m[3][3];
};

constexpr Matrix kIdentity = {{{kOne, 0, 0}, {0, kOne, 0}, {0, 0, kOne}}};

// Rec. 709 luma in linear light, the three weights summing to exactly one.
constexpr Q32 kLumaR = 913110047, kLumaG = 3071760630, kLumaB = 310096619;
constexpr Matrix kGrayscale = {{{kLumaR, kLumaG, kLumaB}, {kLumaR, kLumaG, kLumaB}, {kLumaR, kLumaG, kLumaB}}};

// DisplayColorRemapMode as QuartzCore sets it: grayscale ? (inverted ? 4 : 3) : inverted. AppleCLCD takes 0-4; 2 is
// never sent and is kept as a mode with no effect.
constexpr uint32_t kRemapNormal = 0, kRemapInverted = 1, kRemapGrayscale = 3, kRemapGrayscaleInverted = 4,
                   kRemapModes = 5;

constexpr uint32_t kUnity16 = 1u << 16;
constexpr unsigned kLut1Entries = 256, kLut2Entries = 960, kCscWords = 9;
constexpr uint16_t kCscOne = 0x100;

// The gamma table of selectors 17/27: three channels of 257 entries over [0, 256], 10-bit values.
constexpr unsigned kGammaPoints = 257, kGammaWords = 3 * kGammaPoints;
constexpr uint32_t kGammaMax = 1023;

constexpr uint32_t gammaIdentity(unsigned point)
{
    return point * 4 > kGammaMax ? kGammaMax : point * 4;
}

struct Inputs {
    Matrix harmony = kIdentity;              // SetMatrix, location 9: True Tone and Night Shift
    Matrix accessibility = kIdentity;        // SwapSetColorMatrix, location 2: colour filters
    uint32_t correction = kUnity16;          // SetBrightnessCorrection, 16.16
    uint32_t accessibilityScale = kUnity16;  // SetParameter 3, 16.16: Reduce White Point
    uint32_t remapMode = kRemapNormal;
    uint32_t contrast = kUnity16;  // SetContrast, 16.16; 0 is neutral
    bool blank = false;            // a blank swap: the head shows black whatever the windows scan
    uint8_t gamma[256];            // the gamma table at each 8-bit output
};

// What changes the picture, for the log line.
enum : uint32_t {
    kReasonMatrix = 1u << 0,
    kReasonScale = 1u << 1,
    kReasonRemap = 1u << 2,
    kReasonContrast = 1u << 3,
    kReasonGamma = 1u << 4,
    kReasonBlank = 1u << 5,
};

struct Tables {
    uint16_t lut1[kLut1Entries];
    uint16_t csc[kCscWords];  // KRR KGR KBR KRG KGG KBG KRB KGB KBB: K<input><output>, rows are outputs
    uint8_t lut2[kLut2Entries];
    uint32_t reasons;
    bool enabled;
};

inline void identityGamma(uint8_t (&gamma)[256])
{
    for (unsigned i = 0; i < 256; ++i)
        gamma[i] = uint8_t(i);
}

inline bool equal(const Matrix &a, const Matrix &b)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (a.m[r][c] != b.m[r][c])
                return false;
    return true;
}

inline Q32 clampQ32(__int128 value)
{
    const __int128 limit = (__int128(1) << 63) - 1;
    return Q32(value > limit ? limit : value < -limit ? -limit : value);
}

inline Matrix multiply(const Matrix &outer, const Matrix &inner)
{
    Matrix out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            __int128 sum = 0;
            for (int k = 0; k < 3; ++k)
                sum += __int128(outer.m[r][k]) * inner.m[k][c];
            out.m[r][c] = clampQ32(sum >> 32);
        }
    return out;
}

inline Matrix scaled(const Matrix &in, Q32 factor)
{
    Matrix out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.m[r][c] = clampQ32((__int128(in.m[r][c]) * factor) >> 32);
    return out;
}

// Q32 to the CMU CSC's ten-bit two's complement with eight fraction bits (L4T panel-a-1200-800-8-0.c: 0x138 is 1.22,
// 0x3ba is -0.27), rounded half away from zero and clamped to [-2, 2).
inline int32_t cscValue(Q32 value)
{
    constexpr Q32 half = Q32(1) << 23;
    const Q32 magnitude = value < 0 ? -value : value;
    const Q32 rounded = magnitude > (Q32(512) << 24) ? 512 : (magnitude + half) >> 24;
    const int32_t signedValue = int32_t(value < 0 ? -rounded : rounded);
    return signedValue > 511 ? 511 : signedValue < -512 ? -512 : signedValue;
}

// One CSC row; when rounding each coefficient would change the row's sum, the coefficient nearest a rounding boundary
// takes the difference, so a row summing to one keeps white white.
inline void cscRow(const Q32 (&row)[3], uint16_t *words)
{
    int32_t value[3];
    int32_t total = 0;
    for (int c = 0; c < 3; ++c) {
        value[c] = cscValue(row[c]);
        total += value[c];
    }
    const int32_t wanted = cscValue(row[0] + row[1] + row[2]);
    if (wanted != total) {
        const int32_t step = wanted > total ? 1 : -1;
        int best = 0;
        Q32 bestError = 0;
        for (int c = 0; c < 3; ++c) {
            const Q32 error = (row[c] - (Q32(value[c]) << 24)) * step;
            if (c == 0 || error > bestError) {
                best = c;
                bestError = error;
            }
        }
        const int32_t adjusted = value[best] + (wanted - total);
        if (adjusted >= -512 && adjusted <= 511)
            value[best] = adjusted;
    }
    for (int c = 0; c < 3; ++c)
        words[c] = uint16_t(value[c] & 0x3ff);
}

// An IEEE 754 single (SetContrast's argument) as 16.16; false for a negative, infinite or NaN value.
inline bool floatToUnity16(uint32_t bits, uint32_t *out)
{
    const uint32_t exponent = bits >> 23 & 0xff, mantissa = bits & 0x7fffff;
    if (exponent == 0xff || (bits >> 31 && (exponent || mantissa)))
        return false;
    if (!exponent) {
        *out = 0;
        return true;
    }
    const uint64_t significand = mantissa | 1u << 23;
    const int shift = int(exponent) - 134;
    if (shift >= 0)
        *out =
            shift >= 9 ? UINT32_MAX : uint32_t(significand << shift > UINT32_MAX ? UINT32_MAX : significand << shift);
    else
        *out = -shift >= 40 ? 0 : uint32_t((significand + (uint64_t(1) << (-shift - 1))) >> -shift);
    return true;
}

// The gamma-domain stages after LUT2's curve: contrast about mid-grey, then the inversion, then the gamma table.
inline uint8_t outputStage(uint32_t value, const Inputs &in, bool inverted)
{
    if (in.contrast && in.contrast != kUnity16) {
        const int64_t scaledValue = (int64_t(2 * value) - 255) * in.contrast + int64_t(255) * kUnity16;
        const int64_t result = scaledValue <= 0 ? 0 : (scaledValue + kUnity16) / (2 * kUnity16);
        value = result > 255 ? 255 : uint32_t(result);
    }
    if (inverted)
        value = 255 - value;
    return in.gamma[value];
}

inline bool gammaIsIdentity(const uint8_t (&gamma)[256])
{
    for (unsigned i = 0; i < 256; ++i)
        if (gamma[i] != i)
            return false;
    return true;
}

// The composed transform, content first: the accessibility matrix, grayscale, the white point (harmony), then the
// luminance scales. Scales above one would clip highlights in the CSC, so the product stops at one; a scale of zero is no
// scale and an all-zero matrix no matrix (an unset one must never black the screen).
inline void build(const Inputs &in, Tables &out)
{
    const bool inverted = in.remapMode == kRemapInverted || in.remapMode == kRemapGrayscaleInverted;
    const bool grayscale = in.remapMode == kRemapGrayscale || in.remapMode == kRemapGrayscaleInverted;
    const Matrix zero = {};
    Matrix matrix = equal(in.accessibility, zero) ? kIdentity : in.accessibility;
    if (grayscale)
        matrix = multiply(kGrayscale, matrix);
    if (!equal(in.harmony, zero))
        matrix = multiply(in.harmony, matrix);
    uint64_t scale =
        uint64_t(in.correction ? in.correction : kUnity16) * (in.accessibilityScale ? in.accessibilityScale : kUnity16);
    if (scale > uint64_t(kOne))
        scale = uint64_t(kOne);
    matrix = scaled(matrix, Q32(scale));
    for (int r = 0; r < 3; ++r)
        cscRow(matrix.m[r], &out.csc[r * 3]);
    for (unsigned i = 0; i < kLut1Entries; ++i)
        out.lut1[i] = kSrgbToLinear[i];
    for (unsigned k = 0; k < kLut2Entries; ++k)
        out.lut2[k] = outputStage(kLinearToSrgb[k], in, inverted);
    bool identityCsc = true;
    for (unsigned i = 0; i < kCscWords; ++i)
        identityCsc = identityCsc && out.csc[i] == (i % 4 == 0 ? kCscOne : 0);
    const bool contrast = in.contrast && in.contrast != kUnity16, gammaTable = !gammaIsIdentity(in.gamma);
    out.reasons = (equal(in.harmony, kIdentity) && equal(in.accessibility, kIdentity) ? 0 : kReasonMatrix) |
                  (scale < uint64_t(kOne) ? kReasonScale : 0) | (inverted || grayscale ? kReasonRemap : 0) |
                  (contrast ? kReasonContrast : 0) | (gammaTable ? kReasonGamma : 0);
    out.enabled = !identityCsc || inverted || contrast || gammaTable;
    if (in.blank) {
        for (unsigned k = 0; k < kLut2Entries; ++k)
            out.lut2[k] = 0;
        out.reasons |= kReasonBlank;
        out.enabled = true;
    }
}

// LUT2's bucket for a 12-bit linear value: one per value below 512, one per eight above.
constexpr unsigned lut2Index(uint32_t linear)
{
    return linear < 512 ? linear : 512 + ((linear - 512) >> 3);
}

}  // namespace switch_color
