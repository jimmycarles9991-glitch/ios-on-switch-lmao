// SPDX-License-Identifier: BSD-2-Clause-Patent
// The odd-size probe, debug kernels only (selector 6, SwitchVICKernelTest.h). tegra_vic_build describes an odd-sized
// 4:2:0 or 4:2:2 surface to VIC one pixel wider (4:2:0: and taller), in whole chroma pairs, and leaves its rectangles
// odd (TegraVICFormats.h, tegra_vic_padded_width), which no job had run. VIC copies '420v' and '422v' pictures onto
// themselves 1:1 (the same range and matrix pass the channels through), even-sized as the controls, then odd across,
// down and both, and mirrored across (a flip mirrors around the padded surface: the target starts on column 1). Every
// luma byte and CbCr pair holds its own value, so the copy must put every byte where it belongs, those of the last
// column, row and pair (lastWrong) too, and change nothing past the pictures. Plain logic, no IOKit:
// SwitchVICOddProbe.cpp runs it, test/switch_vic_odd_probe_test.cpp runs it on test/vic_model.h.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_ODD_PROBE_MAGIC 0x564f4444u  // 'VODD'
#define SWITCH_VIC_ODD_PROBE_CASES 8u

typedef struct {
    uint32_t fourcc, width, height, transform;
    int32_t status;                                 // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t wrong;                                 // picture bytes that are not the source's
    uint32_t lastWrong;                             // of them, in the last column, row or chroma pair
    uint32_t firstPlane, firstX, firstY, firstGot;  // the first of them (x in bytes)
    uint32_t outsideWritten;                        // destination bytes changed outside the pictures
} SwitchVICOddResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: both even-sized copies were exact
    uint32_t cleanCases;     // bit i: results[i] copied exactly with nothing written outside its pictures
    SwitchVICOddResult results[SWITCH_VIC_ODD_PROBE_CASES];
} SwitchVICOddProbeReply;

namespace switch_vic_odd_probe {

constexpr uint32_t k420v = 0x34323076u, k422v = 0x34323276u;
constexpr uint32_t kSourcePitch = 256, kDestinationPitch = 64, kControls = 2, kLeft = 1;
constexpr uint64_t kChromaOffset = 4096, kBufferBytes = 16 * 1024;
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a;

struct Case {
    uint32_t fourcc, width, height, transform;
};

constexpr Case kCases[SWITCH_VIC_ODD_PROBE_CASES] = {
    {k420v, 32, 8, 0},
    {k422v, 32, 8, 0},
    {k420v, 33, 8, 0},
    {k420v, 32, 9, 0},
    {k420v, 33, 9, 0},
    {k422v, 33, 8, 0},
    {k420v, 32, 8, TEGRA_VIC_FLIP_H},
    {k420v, 33, 9, TEGRA_VIC_FLIP_H},
};

inline const TegraVICFormat *formatOf(const Case &c)
{
    return tegra_vic_format(c.fourcc);
}

// The rows plane p holds: an odd 4:2:0 height's chroma plane its last luma row's pair row too.
inline uint32_t planeRows(const Case &c, uint32_t p)
{
    const TegraVICFormat *f = formatOf(c);
    return p ? tegra_vic_plane_rows(f, tegra_vic_padded_height(f, c.height), p) : c.height;
}

inline uint32_t rowBytes(const Case &c, uint32_t p)
{
    return tegra_vic_row_bytes(formatOf(c), c.width, p);
}

inline uint8_t sourceByte(uint32_t p, uint32_t x, uint32_t y)
{
    return uint8_t(p ? 0x80 + 7 * x + 13 * y : 1 + 3 * x + 37 * y);
}

inline void fillSource(uint8_t *bytes, const Case &c)
{
    memset(bytes, kSourcePad, kBufferBytes);
    for (uint32_t p = 0; p < 2; p++) {
        for (uint32_t y = 0; y < planeRows(c, p); y++) {
            for (uint32_t x = 0; x < rowBytes(c, p); x++)
                bytes[(p ? kChromaOffset : 0) + uint64_t(y) * kSourcePitch + x] = sourceByte(p, x, y);
        }
    }
}

inline TegraVICImage image(const Case &c, bool destination, uint64_t address)
{
    TegraVICImage im = {};
    im.format = formatOf(c);
    im.width = c.width;
    im.height = c.height;
    im.chromaSite = kLeft;
    for (uint32_t p = 0; p < 2; p++) {
        const uint64_t offset = p ? kChromaOffset : 0;
        im.pitch[p] = destination ? kDestinationPitch : kSourcePitch;
        im.rows[p] = planeRows(c, p);
        im.base[p] = address + offset;
        im.span[p] = kBufferBytes - offset;
    }
    return im;
}

// The job for case c: the whole picture onto itself, nearest; null, or why VIC refuses it.
inline const char *build(const Case &c, uint64_t source, uint64_t destination, uint64_t config, uint64_t filter,
                         TegraVICJob *job)
{
    const TegraVICImage in = image(c, false, source), out = image(c, true, destination);
    TegraVICOp op = {};
    op.srcW = uint64_t(c.width) << 16;
    op.srcH = uint64_t(c.height) << 16;
    op.dstW = c.width;
    op.dstH = c.height;
    op.nearest = 1;
    op.transform = c.transform;
    return tegra_vic_build(&in, &out, &op, &kTegraVICTraits, config, filter, job);
}

// The byte at (x, y) of plane p of the output: luma from the source column the flip mirrors to, a CbCr pair from the
// source pair under its first (even) pixel.
inline uint8_t expected(const Case &c, uint32_t p, uint32_t x, uint32_t y)
{
    if (!(c.transform & TEGRA_VIC_FLIP_H))
        return sourceByte(p, x, y);
    if (!p)
        return sourceByte(0, c.width - 1 - x, y);
    return sourceByte(1, 2 * ((c.width - 1 - (x & ~1u)) / 2) + (x & 1), y);
}

inline bool last(const Case &c, uint32_t p, uint32_t x, uint32_t y)
{
    return x + (p ? 2 : 1) >= rowBytes(c, p) || y + 1 == planeRows(c, p);
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICOddResult *r)
{
    for (uint32_t p = 0; p < 2; p++) {
        const uint8_t *plane = bytes + (p ? kChromaOffset : 0);
        for (uint32_t y = 0; y < planeRows(c, p); y++) {
            for (uint32_t x = 0; x < rowBytes(c, p); x++) {
                const uint8_t got = plane[uint64_t(y) * kDestinationPitch + x];
                if (got == expected(c, p, x, y))
                    continue;
                r->lastWrong += last(c, p, x, y);
                if (!r->wrong++) {
                    r->firstPlane = p;
                    r->firstX = x;
                    r->firstY = y;
                    r->firstGot = got;
                }
            }
        }
    }
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        const uint32_t p = at >= kChromaOffset;
        const uint64_t within = at - (p ? kChromaOffset : 0);
        const bool inside =
            within < uint64_t(kDestinationPitch) * planeRows(c, p) && within % kDestinationPitch < rowBytes(c, p);
        r->outsideWritten += !inside && bytes[at] != kPoison;
    }
}

inline bool clean(const SwitchVICOddResult &r)
{
    return !r.status && !r.wrong && !r.outsideWritten;
}

inline void summarise(SwitchVICOddProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    }
    reply->controlsClean = (reply->cleanCases & ((1u << kControls) - 1)) == (1u << kControls) - 1;
}

}  // namespace switch_vic_odd_probe
