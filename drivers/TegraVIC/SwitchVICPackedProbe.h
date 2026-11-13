// SPDX-License-Identifier: BSD-2-Clause-Patent
// The packed 4:2:2 probe, debug kernels only (selector 6, SwitchVICKernelTest.h). kTegraVICTraits.packedYcc is 0:
// nothing says how VIC counts a packed row (one element a pixel, as TegraVICFormats.h assumes, or one a pixel pair),
// or which byte of a pair NVB0B6_T_U8_Y8__V8_Y8 (59, '2vuy') and NVB0B6_T_Y8_U8__Y8_V8 (57, 'yuvs') put where. Each
// packed format goes in against NV12 out, NV12 in against it out, and to itself, each under both row readings
// (LumaWidth + 1 the row's bytes over 2, or over 4); NV12 both ways is proven, and the same range and matrix pass
// channels through untouched. Then each goes to BGRA under the pixel reading (BT.601 video range, against the engine's
// arithmetic within kTolerance). Every byte of a packed source lies in a range that names its place in the pair, and
// every byte of an NV12 source in one that names its role, so roleMap says which byte carried what, whichever way the
// engine reads them. Chroma is one value over the surface, so no chroma resampling changes it. The NV12 copy is the
// control. Plain logic, no IOKit: SwitchVICPackedProbe.cpp runs it, test/switch_vic_packed_probe_test.cpp checks it.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_PACKED_PROBE_MAGIC 0x56504b44u  // 'VPKD'
#define SWITCH_VIC_PACKED_PROBE_CASES 15u

typedef struct {
    uint32_t source, destination;  // fourccs
    uint32_t elementBytes;         // the reading programmed: 2, a pixel an element; 4, a pixel pair
    int32_t status;                // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t mismatches;  // destination bytes (BGRA: pixels past kTolerance) off what TegraVICFormats.h expects
    uint32_t maxError;    // BGRA: the largest channel difference from the reference
    // Nibble p: what byte p of the packed side's pixel pair carried (0 even luma, 1 odd luma, 2 Cb, 3 Cr), or, packed
    // to packed, the source byte it came from; 0xf mixed or none. 0xffff: no packed side to read.
    uint32_t roleMap;
    uint32_t firstPlane, firstX, firstY, firstGot;  // the first wrong byte (BGRA: pixel) and what it holds
    uint32_t outsideWritten;                        // changed destination bytes outside the picture
} SwitchVICPackedResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: the NV12 copy was exact
    uint32_t cleanCases;     // bit i: results[i] was what TegraVICFormats.h expects, nothing written outside
    SwitchVICPackedResult results[SWITCH_VIC_PACKED_PROBE_CASES];
} SwitchVICPackedProbeReply;

namespace switch_vic_packed_probe {

constexpr uint32_t k2vuy = 0x32767579u, kYuvs = 0x79757673u, kNV12 = 0x34323076u, kBGRA = 0x42475241u;

enum Role : uint32_t {
    kLumaEven = 0,
    kLumaOdd = 1,
    kCb = 2,
    kCr = 3,
};

constexpr uint32_t kWidth = 64, kHeight = 16, kTolerance = 2, kNoRoles = 0xffff;
constexpr uint64_t kBufferBytes = 64 * 1024;
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a, kCbValue = 0x9a, kCrValue = 0xd5;

struct Case {
    uint32_t source, destination, elementBytes;
};

constexpr Case kCases[SWITCH_VIC_PACKED_PROBE_CASES] = {
    {kNV12, kNV12, 2}, {k2vuy, kNV12, 2}, {k2vuy, kNV12, 4}, {kYuvs, kNV12, 2}, {kYuvs, kNV12, 4},
    {kNV12, k2vuy, 2}, {kNV12, k2vuy, 4}, {kNV12, kYuvs, 2}, {kNV12, kYuvs, 4}, {k2vuy, k2vuy, 2},
    {k2vuy, k2vuy, 4}, {kYuvs, kYuvs, 2}, {kYuvs, kYuvs, 4}, {k2vuy, kBGRA, 2}, {kYuvs, kBGRA, 2},
};

inline bool packed(uint32_t fourcc)
{
    return fourcc == k2vuy || fourcc == kYuvs;
}

// TegraVICFormats.h's reading: '2vuy' is Cb Y0 Cr Y1, 'yuvs' Y0 Cb Y1 Cr.
inline Role role(uint32_t fourcc, uint32_t position)
{
    static constexpr Role uyvy[4] = {kCb, kLumaEven, kCr, kLumaOdd}, yuyv[4] = {kLumaEven, kCb, kLumaOdd, kCr};
    return (fourcc == k2vuy ? uyvy : yuyv)[position % 4];
}

inline uint32_t position(uint32_t fourcc, Role wanted)
{
    for (uint32_t p = 0; p < 4; p++) {
        if (role(fourcc, p) == wanted)
            return p;
    }
    return 0;
}

inline uint32_t expectedRoles(const Case &c)
{
    if (c.destination == kBGRA || !(packed(c.source) || packed(c.destination)))
        return kNoRoles;
    if (packed(c.source) && packed(c.destination))
        return 0x3210;
    const uint32_t fourcc = packed(c.source) ? c.source : c.destination;
    return role(fourcc, 0) | role(fourcc, 1) << 4 | role(fourcc, 2) << 8 | role(fourcc, 3) << 12;
}

struct Plane {
    uint64_t offset;
    uint32_t pitch, rowBytes, height;
};

struct Layout {
    uint32_t planes;
    Plane plane[2];
};

// Sources at VIC's source row alignment (256 bytes), destinations at its destination one (64).
inline Layout layout(uint32_t fourcc, bool destination)
{
    Layout l = {};
    l.planes = fourcc == kNV12 ? 2 : 1;
    uint64_t at = 0;
    for (uint32_t p = 0; p < l.planes; p++) {
        Plane &plane = l.plane[p];
        plane.rowBytes = fourcc == kNV12 ? kWidth : fourcc == kBGRA ? 4 * kWidth : 2 * kWidth;
        plane.height = p ? kHeight / 2 : kHeight;
        const uint32_t align = destination ? 64 : 256;
        plane.pitch = (plane.rowBytes + align - 1) / align * align;
        plane.offset = at;
        at += (uint64_t(plane.pitch) * plane.height + 255) / 256 * 256;
    }
    return l;
}

// A packed source's byte lies in [0x40 p, 0x40 p + 0x3f] for its place p in the pair; luma varies with the pixel.
inline uint8_t packedByte(uint32_t fourcc, uint32_t pair, uint32_t p, uint32_t y)
{
    const Role r = role(fourcc, p);
    const uint32_t low = r == kCb ? 0x1a : r == kCr ? 0x15 : (pair * 7 + y * 3 + r * 29) % 64;
    return uint8_t(0x40 * p + low);
}

// An NV12 source's byte lies in [0x40 r, 0x40 r + 0x3f] for its role r.
inline uint8_t nv12Luma(uint32_t x, uint32_t y)
{
    return uint8_t(0x40 * (x % 2) + ((x / 2) * 7 + y * 3 + (x % 2) * 29) % 64);
}

inline uint8_t sourceByte(uint32_t fourcc, uint32_t plane, uint32_t x, uint32_t y)
{
    if (packed(fourcc))
        return packedByte(fourcc, x / 4, x % 4, y);
    return plane ? (x % 2 ? kCrValue : kCbValue) : nv12Luma(x, y);
}

// The source's sample of `wanted` for pixel x of row y.
inline uint8_t sample(uint32_t fourcc, Role wanted, uint32_t x, uint32_t y)
{
    if (packed(fourcc)) {
        const Role r = wanted == kLumaEven || wanted == kLumaOdd ? Role(x % 2) : wanted;
        return packedByte(fourcc, x / 2, position(fourcc, r), y);
    }
    if (wanted == kCb || wanted == kCr)
        return wanted == kCb ? kCbValue : kCrValue;
    return nv12Luma(x, y);
}

inline uint8_t expectedByte(const Case &c, uint32_t plane, uint32_t x, uint32_t y)
{
    if (packed(c.destination)) {
        if (packed(c.source))
            return packedByte(c.source, x / 4, x % 4, y);
        const Role r = role(c.destination, x % 4);
        return sample(c.source, r, 2 * (x / 4) + (r == kLumaOdd ? 1 : 0), y);
    }
    if (plane)
        return sample(c.source, x % 2 ? kCr : kCb, x, 2 * y);
    return sample(c.source, x % 2 ? kLumaOdd : kLumaEven, x, y);
}

// BT.601 video range through the engine's arithmetic (TegraVICConfig.h): samples widened x << 2 | x >> 6.
inline uint32_t rgbOf(uint32_t luma, uint32_t cb, uint32_t cr)
{
    const TegraVICTraits &t = kTegraVICTraits;
    TegraVICMatrix m;
    tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_601, false, &t.in, &m);
    const auto widen = [](uint32_t v) {
        return v << 2 | v >> 6;
    };
    uint32_t in10[3] = {}, out10[3] = {};
    in10[0] = widen(luma);
    in10[t.in.cbChannel] = widen(cb);
    in10[t.in.crChannel] = widen(cr);
    tegra_vic_matrix_apply(&m, in10, out10);
    return 0xff000000u | (out10[0] >> 2) << 16 | (out10[1] >> 2) << 8 | (out10[2] >> 2);
}

inline uint32_t expectedPixel(const Case &c, uint32_t x, uint32_t y)
{
    return rgbOf(sample(c.source, x % 2 ? kLumaOdd : kLumaEven, x, y), sample(c.source, kCb, x, y),
                 sample(c.source, kCr, x, y));
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
    const Layout l = layout(c.source, false);
    for (uint32_t p = 0; p < l.planes; p++) {
        const Plane &plane = l.plane[p];
        for (uint32_t y = 0; y < plane.height; y++) {
            for (uint32_t x = 0; x < plane.rowBytes; x++)
                bytes[plane.offset + uint64_t(y) * plane.pitch + x] = sourceByte(c.source, p, x, y);
        }
    }
}

inline TegraVICImage image(uint32_t fourcc, bool destination, uint64_t address)
{
    const Layout l = layout(fourcc, destination);
    TegraVICImage im = {};
    im.format = tegra_vic_format(fourcc);
    im.width = kWidth;
    im.height = kHeight;
    for (uint32_t p = 0; p < l.planes; p++) {
        im.pitch[p] = l.plane[p].pitch;
        im.rows[p] = l.plane[p].height;
        im.base[p] = address + l.plane[p].offset;
        im.span[p] = kBufferBytes - l.plane[p].offset;
    }
    return im;
}

// The job for case c, the packed side's rows read as c.elementBytes a pixel's element; null, or why VIC refuses it.
inline const char *build(const Case &c, uint64_t source, uint64_t destination, uint64_t config, uint64_t filter,
                         TegraVICJob *job)
{
    TegraVICTraits traits = kTegraVICTraits;
    traits.packedYcc = 1;
    const TegraVICImage in = image(c.source, false, source), out = image(c.destination, true, destination);
    TegraVICOp op = {};
    op.srcW = uint64_t(kWidth) << 16;
    op.srcH = uint64_t(kHeight) << 16;
    op.dstW = kWidth;
    op.dstH = kHeight;
    op.nearest = 1;
    if (const char *why = tegra_vic_build(&in, &out, &op, &traits, config, filter, job))
        return why;
    if (c.elementBytes == 4 && packed(c.source))
        TEGRA_VIC_SET(job->config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, SURF_LUMA_WIDTH, in.pitch[0] / 4 - 1);
    if (c.elementBytes == 4 && packed(c.destination))
        TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH, out.pitch[0] / 4 - 1);
    return nullptr;
}

// Nibble p of the packed side: the one range (a role, or a source byte) every value at its place p came from.
inline uint32_t readRoles(const uint8_t *bytes, const Case &c)
{
    if (expectedRoles(c) == kNoRoles)
        return kNoRoles;
    const Layout d = layout(c.destination, true);
    uint32_t seen[4] = {0, 0, 0, 0};  // bit q: a value from range q
    for (uint32_t p = 0; p < d.planes; p++) {
        const Plane &plane = d.plane[p];
        for (uint32_t y = 0; y < plane.height; y++) {
            for (uint32_t x = 0; x < plane.rowBytes; x++) {
                const uint32_t place = packed(c.destination) ? x % 4 : p ? kCb + x % 2 : x % 2;
                seen[place] |= 1u << (bytes[plane.offset + uint64_t(y) * plane.pitch + x] >> 6);
            }
        }
    }
    uint32_t map = kNoRoles;
    for (uint32_t place = 0; place < 4; place++) {
        const uint32_t ranges = seen[place];
        if (!ranges || (ranges & (ranges - 1)))
            continue;
        const uint32_t q = uint32_t(__builtin_ctz(ranges));
        const uint32_t nibble = packed(c.destination) ? place : q, value = packed(c.destination) ? q : place;
        map = (map & ~(0xfu << (4 * nibble))) | value << (4 * nibble);
    }
    return map;
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICPackedResult *r)
{
    const Layout d = layout(c.destination, true);
    const auto wrong = [r](uint32_t plane, uint32_t x, uint32_t y, uint32_t got) {
        if (!r->mismatches++) {
            r->firstPlane = plane;
            r->firstX = x;
            r->firstY = y;
            r->firstGot = got;
        }
    };
    for (uint32_t p = 0; p < d.planes; p++) {
        const Plane &plane = d.plane[p];
        for (uint32_t y = 0; y < plane.height; y++) {
            const uint8_t *row = bytes + plane.offset + uint64_t(y) * plane.pitch;
            if (c.destination == kBGRA) {
                for (uint32_t x = 0; x < kWidth; x++) {
                    uint32_t got = 0;
                    memcpy(&got, row + 4ull * x, 4);
                    const uint32_t off = distance(got, expectedPixel(c, x, y));
                    r->maxError = off > r->maxError ? off : r->maxError;
                    if (off > kTolerance)
                        wrong(p, x, y, got);
                }
                continue;
            }
            for (uint32_t x = 0; x < plane.rowBytes; x++) {
                if (row[x] != expectedByte(c, p, x, y))
                    wrong(p, x, y, row[x]);
            }
        }
    }
    r->roleMap = readRoles(bytes, c);
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        bool inside = false;
        for (uint32_t p = 0; p < d.planes; p++) {
            const Plane &plane = d.plane[p];
            const uint64_t from = plane.offset, to = plane.offset + uint64_t(plane.pitch) * plane.height;
            inside |= at >= from && at < to && (at - from) % plane.pitch < plane.rowBytes;
        }
        r->outsideWritten += !inside && bytes[at] != kPoison;
    }
}

inline bool clean(const SwitchVICPackedResult &r)
{
    return !r.status && !r.mismatches && !r.outsideWritten;
}

inline void summarise(SwitchVICPackedProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    }
    reply->controlsClean = reply->cleanCases & 1u;
}

}  // namespace switch_vic_packed_probe
