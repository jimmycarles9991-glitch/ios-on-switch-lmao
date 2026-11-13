// SPDX-License-Identifier: BSD-2-Clause-Patent
// The transposed 4:2:2 probe (L998), debug kernels only (selector 6, SwitchVICKernelTest.h). tegra_vic_build describes
// a transposed '422v' output before the transpose as N422R, chroma halved down the composed picture, with its siting
// exchanged (TegraVICFormats.h, tegra_vic_chroma_loc): both inferred from the name and the 4:2:0 measurements, never run.
// A 16x16 '422v' goes through a quarter turn as built, with each of those two readings changed (N422 for N422R, the
// siting left unexchanged), beside the proven transposed '420v' copy and the untransposed '422v' copy.
//
// Luma is unique per pixel. Chroma is one value across each source row and a ramp down the rows (Cb rising, Cr falling,
// kStep a row), so an output CbCr pair names the source row position it was sampled at whatever filter took it:
// offsetHalves is that position less the pair's first output column's row, in half rows along the output (0 on it, 1
// between it and the next, 2 on the next). Plain logic, no IOKit: SwitchVICTransposeProbe.cpp runs it,
// test/switch_vic_transpose_probe_test.cpp runs it on test/vic_model.h.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_TRANSPOSE_PROBE_MAGIC 0x56543432u  // 'VT42'
#define SWITCH_VIC_TRANSPOSE_PROBE_CASES 8u

typedef struct {
    uint32_t variant;                   // switch_vic_transpose_probe::Variant
    int32_t status;                     // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t lumaWrong;                 // output luma samples off the turned source
    uint32_t chromaWrong;               // interior CbCr pairs off the value the case expects
    uint32_t offsetHalves;              // measured on the interior pairs: 0, 1 or 2; 0xf mixed or off the ramp
    uint32_t expectedHalves;            // what the case's programmed siting gives if the reading holds
    uint32_t outsideWritten;            // destination bytes changed outside the planes' pictures
    uint32_t firstX, firstY, firstGot;  // the first wrong pair and its Cb | Cr << 8
} SwitchVICTransposeResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: both copies were exact
    uint32_t cleanCases;     // bit i: results[i] was what the case expects, nothing written outside
    SwitchVICTransposeResult results[SWITCH_VIC_TRANSPOSE_PROBE_CASES];
} SwitchVICTransposeProbeReply;

namespace switch_vic_transpose_probe {

constexpr uint32_t k420v = 0x34323076u, k422v = 0x34323276u;
constexpr uint32_t kSize = 16, kStep = 12, kCbBase = 16, kCrBase = 236, kMixed = 0xf;
constexpr uint32_t kSourcePitch = 256, kDestinationPitch = 64;
constexpr uint64_t kBufferBytes = 64 * 1024, kChromaOffset = 4096;
constexpr uint8_t kPoison = 0xab;
// IOSurfaceAccelerator siting codes: 1 Left (on the even sample across), 2 Center (between).
constexpr uint32_t kSiteLeft = 1, kSiteCenter = 2;

enum Variant : uint32_t {
    kCopy420Turned,  // control: '420v' turned, the transposed copy the unit has run
    kCopy422,        // control: '422v' untransposed
    kShipped,        // '422v' turned as tegra_vic_build makes it, sited Center
    kShippedLeft,    // the same sited Left
    kSitingKept,     // N422R, Center's siting not exchanged
    kN422,           // described as N422, siting as built
    kN422Kept,       // N422, siting not exchanged
    kShippedTurn4,   // transform 4 (a quarter turn with its flip), as built, Center
};

struct Case {
    Variant variant;
    uint32_t source, transform, site;
    int32_t format;             // SURF_FORMAT written over the built one; -1 keeps it
    int32_t locHoriz, locVert;  // -1 keeps the built siting
};

constexpr Case kCases[SWITCH_VIC_TRANSPOSE_PROBE_CASES] = {
    {kCopy420Turned, k420v, 6, kSiteLeft, -1, -1, -1},
    {kCopy422, k422v, 0, kSiteLeft, -1, -1, -1},
    {kShipped, k422v, 6, kSiteCenter, -1, -1, -1},
    {kShippedLeft, k422v, 6, kSiteLeft, -1, -1, -1},
    {kSitingKept, k422v, 6, kSiteCenter, -1, 1, 0},
    {kN422, k422v, 6, kSiteCenter, TEGRA_VIC_T_Y8___U8V8_N422, -1, -1},
    {kN422Kept, k422v, 6, kSiteCenter, TEGRA_VIC_T_Y8___U8V8_N422, 1, 0},
    {kShippedTurn4, k422v, 4, kSiteCenter, -1, -1, -1},
};

inline bool control(const Case &c)
{
    return c.variant == kCopy420Turned || c.variant == kCopy422;
}

inline uint8_t luma(uint32_t x, uint32_t y)
{
    return uint8_t(y * kSize + x);
}

// Chroma of source luma row y (a '420v' chroma row holds rows 2r and 2r + 1, sampled at 2r).
inline uint8_t cbAt(uint32_t y)
{
    return uint8_t(kCbBase + kStep * y);
}

inline uint8_t crAt(uint32_t y)
{
    return uint8_t(kCrBase - kStep * y);
}

// IOSurfaceAccelerator's transform (flips, then a clockwise quarter turn): the source pixel output (u, v) shows.
inline void sourceOf(uint32_t transform, uint32_t u, uint32_t v, uint32_t *x, uint32_t *y)
{
    uint32_t sx = u, sy = v;
    if (transform & TEGRA_VIC_ROT90) {
        sx = v;
        sy = kSize - 1 - u;
    }
    if (transform & TEGRA_VIC_FLIP_V)
        sy = kSize - 1 - sy;
    if (transform & TEGRA_VIC_FLIP_H)
        sx = kSize - 1 - sx;
    *x = sx;
    *y = sy;
}

inline uint32_t chromaRows(uint32_t fourcc)
{
    return fourcc == k420v ? kSize / 2 : kSize;
}

inline void fillSource(uint8_t *bytes, const Case &c)
{
    memset(bytes, 0x5a, kBufferBytes);
    for (uint32_t y = 0; y < kSize; y++) {
        for (uint32_t x = 0; x < kSize; x++)
            bytes[uint64_t(y) * kSourcePitch + x] = luma(x, y);
    }
    const bool half = c.source == k420v;
    for (uint32_t r = 0; r < chromaRows(c.source); r++) {
        uint8_t *row = bytes + kChromaOffset + uint64_t(r) * kSourcePitch;
        for (uint32_t pair = 0; pair < kSize / 2; pair++) {
            row[2 * pair] = cbAt(half ? 2 * r : r);
            row[2 * pair + 1] = crAt(half ? 2 * r : r);
        }
    }
}

inline TegraVICImage image(uint32_t fourcc, bool destination, uint64_t address, uint32_t site)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(fourcc);
    im.width = kSize;
    im.height = kSize;
    im.chromaSite = site;
    const uint32_t pitch = destination ? kDestinationPitch : kSourcePitch;
    for (uint32_t p = 0; p < 2; p++) {
        const uint64_t offset = p ? kChromaOffset : 0;
        im.pitch[p] = pitch;
        im.rows[p] = p ? chromaRows(fourcc) : kSize;
        im.base[p] = address + offset;
        im.span[p] = kBufferBytes - offset;
    }
    return im;
}

inline const char *build(const Case &c, uint64_t source, uint64_t destination, uint64_t config, uint64_t filter,
                         TegraVICJob *job)
{
    const TegraVICImage in = image(c.source, false, source, kSiteLeft);
    const TegraVICImage out = image(c.source, true, destination, c.site);
    TegraVICOp op = {};
    op.srcW = uint64_t(kSize) << 16;
    op.srcH = uint64_t(kSize) << 16;
    op.dstW = kSize;
    op.dstH = kSize;
    op.transform = c.transform;
    op.nearest = 1;
    if (const char *why = tegra_vic_build(&in, &out, &op, &kTegraVICTraits, config, filter, job))
        return why;
    if (c.format >= 0)
        TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT, uint32_t(c.format));
    if (c.locHoriz >= 0)
        TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ, uint32_t(c.locHoriz));
    if (c.locVert >= 0)
        TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT, uint32_t(c.locVert));
    return nullptr;
}

// The siting each reading predicts along the output's halved axis, as half columns: N422R halves the composed picture
// down, so its vertical code; N422 would describe the memory as it lies, so its horizontal code. The copies' chroma is
// one value across a pair.
inline uint32_t expectedHalves(const Case &c)
{
    if (control(c))
        return 0;
    uint32_t horiz = 0, vert = 0;
    tegra_vic_chroma_loc(tegra_vic_format(c.source), c.site, c.transform & TEGRA_VIC_ROT90, &horiz, &vert);
    if (c.locHoriz >= 0)
        horiz = uint32_t(c.locHoriz);
    if (c.locVert >= 0)
        vert = uint32_t(c.locVert);
    return c.format == TEGRA_VIC_T_Y8___U8V8_N422 ? horiz : vert;
}

// The CbCr pair output columns 2k and 2k + 1 of row v hold when sampled `halves` half columns past 2k.
inline uint32_t pairAt(const Case &c, uint32_t k, uint32_t v, uint32_t halves)
{
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    sourceOf(c.transform, 2 * k, v, &x0, &y0);
    sourceOf(c.transform, 2 * k + 1, v, &x1, &y1);
    if (c.source == k420v) {
        y0 &= ~1u;
        y1 = y0;
    }
    const uint32_t cb = (cbAt(y0) * (2 - halves) + cbAt(y1) * halves) / 2;
    const uint32_t cr = (crAt(y0) * (2 - halves) + crAt(y1) * halves) / 2;
    return cb | cr << 8;
}

inline uint32_t distance(uint32_t a, uint32_t b)
{
    const auto apart = [](uint32_t p, uint32_t q) {
        return p > q ? p - q : q - p;
    };
    const uint32_t cb = apart(a & 0xff, b & 0xff), cr = apart(a >> 8, b >> 8);
    return cb > cr ? cb : cr;
}

// Halves 0-2 whose value `got` is (within one step of the engine's rounding), else kMixed.
inline uint32_t halvesOf(const Case &c, uint32_t k, uint32_t v, uint32_t got)
{
    for (uint32_t halves = 0; halves < 3; halves++) {
        if (distance(got, pairAt(c, k, v, halves)) <= 1)
            return halves;
    }
    return kMixed;
}

inline uint32_t outputChromaRows(const Case &c)
{
    return c.source == k420v ? kSize / 2 : kSize;
}

inline uint32_t gotPair(const uint8_t *bytes, uint32_t k, uint32_t row)
{
    const uint8_t *pair = bytes + kChromaOffset + uint64_t(row) * kDestinationPitch + 2 * k;
    return pair[0] | uint32_t(pair[1]) << 8;
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICTransposeResult *r)
{
    r->expectedHalves = expectedHalves(c);
    for (uint32_t v = 0; v < kSize; v++) {
        for (uint32_t u = 0; u < kSize; u++) {
            uint32_t x = 0, y = 0;
            sourceOf(c.transform, u, v, &x, &y);
            r->lumaWrong += bytes[uint64_t(v) * kDestinationPitch + u] != luma(x, y);
        }
    }
    uint32_t seen = 0;
    for (uint32_t row = 0; row < outputChromaRows(c); row++) {
        const uint32_t v = c.source == k420v ? 2 * row : row;
        for (uint32_t k = 1; k + 1 < kSize / 2; k++) {
            const uint32_t got = gotPair(bytes, k, row);
            seen |= 1u << halvesOf(c, k, v, got);
            if (distance(got, pairAt(c, k, v, r->expectedHalves)) > 1 && !r->chromaWrong++) {
                r->firstX = k;
                r->firstY = row;
                r->firstGot = got;
            }
        }
    }
    r->offsetHalves = seen && !(seen & (seen - 1)) ? uint32_t(__builtin_ctz(seen)) : kMixed;
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        const bool chroma = at >= kChromaOffset;
        const uint64_t from = chroma ? kChromaOffset : 0;
        const uint64_t rows = chroma ? outputChromaRows(c) : kSize;
        const bool inside = at - from < rows * kDestinationPitch && (at - from) % kDestinationPitch < kSize;
        r->outsideWritten += !inside && bytes[at] != kPoison;
    }
}

inline bool clean(const SwitchVICTransposeResult &r)
{
    return !r.status && !r.lumaWrong && !r.chromaWrong && !r.outsideWritten;
}

inline void summarise(SwitchVICTransposeProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    }
    reply->controlsClean = (reply->cleanCases & 3u) == 3u;
}

}  // namespace switch_vic_transpose_probe
