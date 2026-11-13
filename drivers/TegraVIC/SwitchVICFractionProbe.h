// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fractional source offset and alpha override probe, debug kernels only (selector 6, SwitchVICKernelTest.h).
// IOSurfaceAccelerator's request carries the source rectangle in 16.16 (TransferSurface sends quarter pixels:
// CONTRACT.md section 4) and VIC's SourceRectLeft/Top are 16.16, which tegra_vic_build hands through, yet the capability
// says FractionalXYOffsetsSupported 0. VIC copies a gradient card 1:1 with the 2-tap filter, its source rectangle a
// quarter, half and three quarters of a pixel across, down and both, and every output pixel is scored against three
// answers: the linear interpolation at the offset (the claim), the offset truncated and the offset rounded. B steps 8
// codes a column and G 8 a row, so a quarter pixel is 2 codes and the answers are further apart than the tolerance (1
// code a channel, for the 10-bit pipeline's truncation). The last column and row are edges (VIC may clamp its second
// tap at the source rectangle) and are counted apart. Then AlphaFillMode BACKGROUND (NVB0B6_DXVAHD_ALPHA_FILL_MODE_
// BACKGROUND 1, averne's clb0b6.h), which AlphaOverride needs and TegraVICRequest.h refuses today: a BGRA copy whose
// source alpha varies must come out with every alpha the background's, after the same copy in SOURCE_STREAM, which
// keeps the source's. The integer-offset copy and the SOURCE_STREAM copy are the controls. Plain logic, no IOKit:
// SwitchVICFractionProbe.cpp runs it, test/switch_vic_fraction_probe_test.cpp checks it.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_FRACTION_PROBE_MAGIC 0x56465241u  // 'VFRA'
#define SWITCH_VIC_FRACTION_PROBE_CASES 10u

enum {
    SWITCH_VIC_FRACTION_INTERPOLATED = 0,  // answers; for the alpha cases: the claim, source alpha, opaque
    SWITCH_VIC_FRACTION_TRUNCATED = 1,
    SWITCH_VIC_FRACTION_ROUNDED = 2,
    SWITCH_VIC_FRACTION_ANSWERS = 3,
};

typedef struct {
    uint32_t fractionX, fractionY;                // the source rectangle's offset past whole pixels, 16.16
    uint32_t mode;                                // switch_vic_fraction_probe::Mode
    int32_t status;                               // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t wrong[SWITCH_VIC_FRACTION_ANSWERS];  // inner pixels further than the tolerance from each answer
    uint32_t maxError;                            // the largest channel difference from the claim, inner pixels
    uint32_t edges;                               // last-column and last-row pixels further than that from it
    uint32_t firstX, firstY, firstGot;            // the first inner pixel off the claim, and what it holds
    uint32_t outsideWritten;                      // changed destination bytes outside the picture
} SwitchVICFractionResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: the integer-offset copy and the SOURCE_STREAM copy were exact
    uint32_t cleanCases;     // bit i: results[i] matched its claim with nothing written outside the picture
    SwitchVICFractionResult results[SWITCH_VIC_FRACTION_PROBE_CASES];
} SwitchVICFractionProbeReply;

namespace switch_vic_fraction_probe {

enum Mode : uint32_t {
    kFraction = 0,
    kSourceAlpha = 1,
    kBackgroundAlpha = 2,
};

struct Case {
    uint32_t fractionX, fractionY;
    Mode mode;
};

constexpr Case kCases[SWITCH_VIC_FRACTION_PROBE_CASES] = {
    {0, 0, kFraction},      {0x4000, 0, kFraction},   {0x8000, 0, kFraction}, {0xc000, 0, kFraction},
    {0, 0x4000, kFraction}, {0, 0x8000, kFraction},   {0, 0xc000, kFraction}, {0x8000, 0x8000, kFraction},
    {0, 0, kSourceAlpha},   {0, 0, kBackgroundAlpha},
};
constexpr uint32_t kControlMask = (1u << 0) | (1u << 8);

constexpr uint32_t kSourceSize = 40, kOrigin = 4, kSize = 24, kStep = 8, kRed = 0x5a, kTolerance = 1, kOverride = 0x80;
constexpr uint32_t kSourcePitch = 256, kDestinationPitch = 128;
constexpr uint64_t kBufferBytes = 64 * 1024;
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a;

inline uint32_t sourcePixel(const Case &c, uint32_t x, uint32_t y)
{
    const uint32_t b = kStep * x < 255 ? kStep * x : 255, g = kStep * y < 255 ? kStep * y : 255;
    const uint32_t a = c.mode == kFraction ? 0xff : 0x10 + (x * 5 + y * 3) % 0xe0;
    return a << 24 | kRed << 16 | g << 8 | b;
}

// What output pixel (x, y) holds under answer `which`, as a BGRA word.
inline uint32_t answer(const Case &c, uint32_t which, uint32_t x, uint32_t y)
{
    if (c.mode != kFraction) {
        const uint32_t source = sourcePixel(c, kOrigin + x, kOrigin + y);
        uint32_t alpha = which == SWITCH_VIC_FRACTION_ROUNDED ? 0xff : source >> 24;
        if (which == SWITCH_VIC_FRACTION_INTERPOLATED && c.mode == kBackgroundAlpha)
            alpha = kOverride;
        return (source & 0xffffffu) | alpha << 24;
    }
    const auto along = [which](uint32_t at, uint32_t fraction) {
        const uint32_t whole = kStep * (kOrigin + at);
        if (which == SWITCH_VIC_FRACTION_INTERPOLATED)
            return whole + kStep * fraction / 65536;
        return which == SWITCH_VIC_FRACTION_ROUNDED && fraction >= 0x8000 ? whole + kStep : whole;
    };
    return 0xffu << 24 | kRed << 16 | along(y, c.fractionY) << 8 | along(x, c.fractionX);
}

inline uint32_t distance(uint32_t a, uint32_t b)
{
    uint32_t most = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const int32_t d = int32_t((a >> shift) & 0xff) - int32_t((b >> shift) & 0xff);
        const uint32_t m = uint32_t(d < 0 ? -d : d);
        most = m > most ? m : most;
    }
    return most;
}

inline void fillSource(uint8_t *bytes, const Case &c)
{
    memset(bytes, kSourcePad, kBufferBytes);
    for (uint32_t y = 0; y < kSourceSize; y++) {
        for (uint32_t x = 0; x < kSourceSize; x++) {
            const uint32_t p = sourcePixel(c, x, y);
            memcpy(bytes + uint64_t(y) * kSourcePitch + 4ull * x, &p, 4);
        }
    }
}

inline TegraVICImage image(uint64_t address, bool destination)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = im.height = im.rows[0] = destination ? kSize : kSourceSize;
    im.pitch[0] = destination ? kDestinationPitch : kSourcePitch;
    im.base[0] = address;
    im.span[0] = kBufferBytes;
    return im;
}

inline TegraVICOp operation(const Case &c)
{
    TegraVICOp op = {};
    op.srcX = (uint64_t(kOrigin) << 16) + c.fractionX;
    op.srcY = (uint64_t(kOrigin) << 16) + c.fractionY;
    op.srcW = uint64_t(kSize) << 16;
    op.srcH = uint64_t(kSize) << 16;
    op.dstW = op.dstH = kSize;
    op.nearest = c.mode != kFraction;
    return op;
}

// What AlphaOverride would program: every pixel's alpha the background's.
inline void overrideAlpha(TegraVICJob *job, uint32_t alpha8)
{
    TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE, TEGRA_VIC_ALPHA_BACKGROUND);
    TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA, alpha8 << 2 | alpha8 >> 6);
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICFractionResult *r)
{
    for (uint32_t y = 0; y < kSize; y++) {
        for (uint32_t x = 0; x < kSize; x++) {
            uint32_t got = 0;
            memcpy(&got, bytes + uint64_t(y) * kDestinationPitch + 4ull * x, 4);
            const uint32_t off = distance(got, answer(c, SWITCH_VIC_FRACTION_INTERPOLATED, x, y));
            if (x == kSize - 1 || y == kSize - 1) {
                r->edges += off > kTolerance;
                continue;
            }
            for (uint32_t which = 0; which < SWITCH_VIC_FRACTION_ANSWERS; which++)
                r->wrong[which] += distance(got, answer(c, which, x, y)) > kTolerance;
            r->maxError = off > r->maxError ? off : r->maxError;
            if (off > kTolerance && r->wrong[SWITCH_VIC_FRACTION_INTERPOLATED] == 1) {
                r->firstX = x;
                r->firstY = y;
                r->firstGot = got;
            }
        }
    }
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        const bool inside = at / kDestinationPitch < kSize && at % kDestinationPitch < 4ull * kSize;
        r->outsideWritten += !inside && bytes[at] != kPoison;
    }
}

inline bool clean(const SwitchVICFractionResult &r)
{
    return !r.status && !r.wrong[SWITCH_VIC_FRACTION_INTERPOLATED] && !r.outsideWritten;
}

inline void summarise(SwitchVICFractionProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    }
    reply->controlsClean = (reply->cleanCases & kControlMask) == kControlMask;
}

}  // namespace switch_vic_fraction_probe
