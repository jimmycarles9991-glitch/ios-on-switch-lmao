// SPDX-License-Identifier: BSD-2-Clause-Patent
// The large-source probe (L1002, L992), debug kernels only (selector 6, SwitchVICKernelTest.h). A 4K picture in NVDEC's
// layout (3840x2160 '420v', 2-GOB blocks) scaled and turned onto the 720x1280 panel by one job, into '420v' and BGRA,
// as an IOSurfaceAccelerator request for a landscape 4K video plans it (tegra_vic_plan makes one copy op; the 12.4 MB
// picture fits the 128 MiB window). And 4096-wide pictures, the T210 NVDEC maximum, block linear and pitch, 4x down.
// A 64x64 copy is the control. Pictures are one colour in each kBlock x kBlock luma block, so each output pixel whose
// source footprint (its scale and the chroma filter's reach either side) lies in one block has one right value whatever
// the filter and phase; those are checked exactly ('420v') or within kTolerance of the engine's arithmetic (BGRA), the
// rest only for being written. Plain logic, no IOKit: SwitchVICLargeProbe.cpp runs it, test/switch_vic_large_probe_test.cpp
// checks it on a model engine.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

#define SWITCH_VIC_LARGE_PROBE_MAGIC 0x5642494bu  // 'VBIK'
#define SWITCH_VIC_LARGE_PROBE_CASES 5u

typedef struct {
    uint32_t sourceWidth, sourceHeight, destination;  // destination: fourcc
    int32_t status;                                   // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t checked, wrong;  // interior output pixels (and '420v' CbCr pairs) checked, and off their value
    uint32_t unwritten;       // output pixels still holding the poison
    uint32_t outsideWritten;  // destination bytes changed outside the planes' pictures
    uint32_t maxError;        // BGRA: the largest channel difference on interior pixels
    uint32_t firstX, firstY, firstGot;
} SwitchVICLargeResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;
    uint32_t cleanCases;
    SwitchVICLargeResult results[SWITCH_VIC_LARGE_PROBE_CASES];
} SwitchVICLargeProbeReply;

namespace switch_vic_large_probe {

constexpr uint32_t k420v = 0x34323076u, kBGRA = 0x42475241u;
constexpr uint32_t kBlock = 32, kTolerance = 2, kPitch = 0xff, kSector = 16;
constexpr uint64_t kBufferBytes = 14ull << 20;
constexpr uint8_t kPoison = 0xab;

struct Case {
    uint32_t sourceWidth, sourceHeight, sourceLog2;  // kPitch: pitch linear
    uint32_t destination, destinationWidth, destinationHeight, transform;
};

constexpr Case kCases[SWITCH_VIC_LARGE_PROBE_CASES] = {
    {64, 64, kPitch, k420v, 64, 64, 0},        {3840, 2160, 1, k420v, 720, 1280, 4},
    {3840, 2160, 1, kBGRA, 720, 1280, 4},      {4096, 2160, 1, k420v, 1024, 540, 0},
    {4096, 2160, kPitch, k420v, 1024, 540, 0},
};

inline uint64_t roundUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

struct Plane {
    uint64_t offset;
    uint32_t pitch, rows, height, rowBytes;
};

struct Layout {
    uint32_t planes;
    Plane plane[2];
};

inline Layout sourceLayout(const Case &c)
{
    Layout l = {2, {}};
    const uint32_t blockRows = c.sourceLog2 == kPitch ? 1 : 8u << c.sourceLog2;
    uint64_t at = 0;
    for (uint32_t p = 0; p < 2; p++) {
        Plane &plane = l.plane[p];
        plane.height = p ? c.sourceHeight / 2 : c.sourceHeight;
        plane.rowBytes = c.sourceWidth;
        plane.pitch = uint32_t(roundUp(c.sourceWidth, 256));
        plane.rows = uint32_t(roundUp(plane.height, blockRows));
        plane.offset = at;
        at = roundUp(at + uint64_t(plane.pitch) * plane.rows, 256);
    }
    return l;
}

inline Layout destinationLayout(const Case &c)
{
    const bool bgra = c.destination == kBGRA;
    Layout l = {bgra ? 1u : 2u, {}};
    uint64_t at = 0;
    for (uint32_t p = 0; p < l.planes; p++) {
        Plane &plane = l.plane[p];
        plane.height = p ? c.destinationHeight / 2 : c.destinationHeight;
        plane.rowBytes = bgra ? 4 * c.destinationWidth : c.destinationWidth;
        plane.pitch = uint32_t(roundUp(plane.rowBytes, 64));
        plane.rows = plane.height;
        plane.offset = at;
        at = roundUp(at + uint64_t(plane.pitch) * plane.rows, 256);
    }
    return l;
}

inline uint32_t mix(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = (x * 0x9e3779b1u) ^ ((y + seed) * 0x85ebca77u);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 13;
    return h;
}

// Luma, Cb and Cr of luma block (bx, by), inside video range and never the poison.
inline uint8_t blockValue(uint32_t bx, uint32_t by, uint32_t channel)
{
    const uint8_t value = uint8_t(32 + mix(bx, by, channel) % 192);
    return value == kPoison ? uint8_t(value + 1) : value;
}

inline uint64_t sourceOffset(const Case &c, const Plane &plane, uint32_t xB, uint32_t y)
{
    if (c.sourceLog2 == kPitch)
        return plane.offset + uint64_t(y) * plane.pitch + xB;
    return plane.offset + tgm_layout_offset_of(plane.pitch, uint8_t(c.sourceLog2), xB, y);
}

// A 16-byte run at xB of row y never crosses a block or a sector, so it is one store in either layout.
inline void fillSource(uint8_t *bytes, const Case &c)
{
    const Layout l = sourceLayout(c);
    for (uint32_t p = 0; p < 2; p++) {
        const Plane &plane = l.plane[p];
        for (uint32_t y = 0; y < plane.height; y++) {
            for (uint32_t xB = 0; xB < plane.rowBytes; xB += kSector) {
                uint8_t *run = bytes + sourceOffset(c, plane, xB, y);
                const uint32_t bx = (p ? xB / 2 : xB) / (p ? kBlock / 2 : kBlock);
                const uint32_t by = y / (p ? kBlock / 2 : kBlock);
                if (!p) {
                    memset(run, blockValue(bx, by, 0), kSector);
                    continue;
                }
                for (uint32_t i = 0; i < kSector; i += 2) {
                    run[i] = blockValue(bx, by, 1);
                    run[i + 1] = blockValue(bx, by, 2);
                }
            }
        }
    }
}

inline TegraVICImage image(const Case &c, bool destination, uint64_t address)
{
    const Layout l = destination ? destinationLayout(c) : sourceLayout(c);
    TegraVICImage im = {};
    im.format = tegra_vic_format(destination ? c.destination : k420v);
    im.width = destination ? c.destinationWidth : c.sourceWidth;
    im.height = destination ? c.destinationHeight : c.sourceHeight;
    im.blockLinear = !destination && c.sourceLog2 != kPitch;
    im.blockHeightLog2 = im.blockLinear ? c.sourceLog2 : 0;
    for (uint32_t p = 0; p < l.planes; p++) {
        im.pitch[p] = l.plane[p].pitch;
        im.rows[p] = l.plane[p].rows;
        im.base[p] = address + l.plane[p].offset;
        im.span[p] = kBufferBytes - l.plane[p].offset;
    }
    return im;
}

inline const char *build(const Case &c, uint64_t source, uint64_t destination, uint64_t config, uint64_t filter,
                         TegraVICJob *job)
{
    const TegraVICImage in = image(c, false, source), out = image(c, true, destination);
    TegraVICOp op = {};
    op.srcW = uint64_t(c.sourceWidth) << 16;
    op.srcH = uint64_t(c.sourceHeight) << 16;
    op.dstW = c.destinationWidth;
    op.dstH = c.destinationHeight;
    op.transform = c.transform;
    return tegra_vic_build(&in, &out, &op, &kTegraVICTraits, config, filter, job);
}

// The source pixel output pixel (u, v) of the transformed picture shows, for a source w x h (IOSurfaceAccelerator's
// transform: flips, then a clockwise quarter turn).
inline void sourceOf(const Case &c, uint32_t u, uint32_t v, uint32_t *x, uint32_t *y)
{
    uint32_t sx = u, sy = v;
    if (c.transform & TEGRA_VIC_ROT90) {
        sx = v;
        sy = c.sourceHeight - 1 - u;
    }
    if (c.transform & TEGRA_VIC_FLIP_V)
        sy = c.sourceHeight - 1 - sy;
    if (c.transform & TEGRA_VIC_FLIP_H)
        sx = c.sourceWidth - 1 - sx;
    *x = sx;
    *y = sy;
}

// The luma block every source sample output pixel (u, v) can draw on lies in, or false when its footprint spans two:
// the pixel's centre in the turned picture, its scale either side, and two chroma samples' reach.
inline bool interior(const Case &c, uint32_t u, uint32_t v, uint32_t *bx, uint32_t *by)
{
    const bool turned = (c.transform & TEGRA_VIC_ROT90) != 0;
    const uint32_t tw = turned ? c.sourceHeight : c.sourceWidth, th = turned ? c.sourceWidth : c.sourceHeight;
    const uint64_t reachX = (tw + c.destinationWidth - 1) / c.destinationWidth + 4;
    const uint64_t reachY = (th + c.destinationHeight - 1) / c.destinationHeight + 4;
    const uint64_t cx = (2 * uint64_t(u) + 1) * tw / (2 * c.destinationWidth);
    const uint64_t cy = (2 * uint64_t(v) + 1) * th / (2 * c.destinationHeight);
    if (cx < reachX || cy < reachY || cx + reachX >= tw || cy + reachY >= th)
        return false;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    sourceOf(c, uint32_t(cx - reachX), uint32_t(cy - reachY), &x0, &y0);
    sourceOf(c, uint32_t(cx + reachX), uint32_t(cy + reachY), &x1, &y1);
    if (x0 / kBlock != x1 / kBlock || y0 / kBlock != y1 / kBlock)
        return false;
    *bx = x0 / kBlock;
    *by = y0 / kBlock;
    return true;
}

// Block colour through the engine's arithmetic for the source's standard (BT.601 video range), as BGRA.
inline uint32_t rgbOf(uint32_t bx, uint32_t by)
{
    const TegraVICTraits &t = kTegraVICTraits;
    TegraVICMatrix m;
    tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_601, false, &t.in, &m);
    const auto widen = [](uint32_t v) {
        return v << 2 | v >> 6;
    };
    uint32_t in10[3] = {}, out10[3] = {};
    in10[0] = widen(blockValue(bx, by, 0));
    in10[t.in.cbChannel] = widen(blockValue(bx, by, 1));
    in10[t.in.crChannel] = widen(blockValue(bx, by, 2));
    tegra_vic_matrix_apply(&m, in10, out10);
    return 0xff000000u | (out10[0] >> 2) << 16 | (out10[1] >> 2) << 8 | (out10[2] >> 2);
}

inline uint32_t distance(uint32_t a, uint32_t b)
{
    uint32_t most = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const uint32_t p = (a >> shift) & 0xff, q = (b >> shift) & 0xff;
        const uint32_t d = p > q ? p - q : q - p;
        most = d > most ? d : most;
    }
    return most;
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICLargeResult *r)
{
    const Layout d = destinationLayout(c);
    const auto wrong = [r](uint32_t u, uint32_t v, uint32_t got) {
        if (!r->wrong++) {
            r->firstX = u;
            r->firstY = v;
            r->firstGot = got;
        }
    };
    const bool bgra = c.destination == kBGRA;
    for (uint32_t v = 0; v < c.destinationHeight; v++) {
        for (uint32_t u = 0; u < c.destinationWidth; u++) {
            uint32_t got = 0;
            const uint8_t *at = bytes + d.plane[0].offset + uint64_t(v) * d.plane[0].pitch + (bgra ? 4 * u : u);
            memcpy(&got, at, bgra ? 4 : 1);
            r->unwritten += bgra ? got == 0xababababu : got == kPoison;
            uint32_t bx = 0, by = 0;
            if (!interior(c, u, v, &bx, &by))
                continue;
            r->checked++;
            if (bgra) {
                const uint32_t off = distance(got, rgbOf(bx, by));
                r->maxError = off > r->maxError ? off : r->maxError;
                if (off > kTolerance)
                    wrong(u, v, got);
            } else if (got != blockValue(bx, by, 0)) {
                wrong(u, v, got);
            }
            if (bgra || (u | v) & 1)
                continue;
            uint32_t bx1 = 0, by1 = 0;
            if (!interior(c, u + 1, v + 1, &bx1, &by1) || bx1 != bx || by1 != by)
                continue;
            const uint8_t *pair = bytes + d.plane[1].offset + uint64_t(v / 2) * d.plane[1].pitch + u;
            r->checked++;
            if (pair[0] != blockValue(bx, by, 1) || pair[1] != blockValue(bx, by, 2))
                wrong(u, v, pair[0] | uint32_t(pair[1]) << 8 | 1u << 31);
        }
    }
    const Plane &last = d.plane[d.planes - 1];
    const uint64_t end = last.offset + uint64_t(last.pitch) * last.rows;
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        bool inside = false;
        for (uint32_t p = 0; p < d.planes && at < end; p++) {
            const Plane &plane = d.plane[p];
            inside |= at >= plane.offset && at - plane.offset < uint64_t(plane.pitch) * plane.rows &&
                      (at - plane.offset) % plane.pitch < plane.rowBytes;
        }
        r->outsideWritten += !inside && bytes[at] != kPoison;
    }
}

inline bool clean(const SwitchVICLargeResult &r)
{
    return !r.status && r.checked && !r.wrong && !r.unwritten && !r.outsideWritten;
}

inline void summarise(SwitchVICLargeProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    }
    reply->controlsClean = reply->cleanCases & 1u;
}

}  // namespace switch_vic_large_probe
