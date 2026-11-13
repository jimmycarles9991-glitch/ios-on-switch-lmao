// SPDX-License-Identifier: BSD-2-Clause-Patent
// The block-linear output probe, debug kernels only (selector 6, SwitchVICKernelTest.h): kTegraVICTraits.blockLinearOut
// says VIC writes OutBlkKind GENERIC_16Bx2 as averne's nvtegra programs it, which no caller has run. VIC copies a BGRA
// card into block-linear destinations of 1, 2 and 16 GOBs a block, and an NV12 picture into NVDEC's own layout (2 GOBs a
// block), 1:1 from pitch-linear sources; pitch-linear copies of both are the controls and run first. The CPU reads the
// destinations back through the Tegra block-linear law below, the law BlockLinear.c de-swizzles NVDEC's pictures with
// and the one the GPU samples its block-linear textures by. Every source byte names its place, and the destination is
// poisoned, so matchedLaws says which layout the engine wrote. VIC writes whole blocks: changed bytes inside a plane's
// blocks but outside the picture are padWritten, not a fault. Plain logic, no IOKit: SwitchVICLayoutProbe.cpp runs it,
// test/switch_vic_layout_probe_test.cpp checks it.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_LAYOUT_PROBE_MAGIC 0x56424c4bu  // 'VBLK'
#define SWITCH_VIC_LAYOUT_PROBE_CASES 6u

typedef struct {
    uint32_t fourcc, blockHeightLog2;               // the destination's; 0xff: pitch linear
    int32_t status;                                 // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t mismatches;                            // elements that do not read back under the destination's layout
    uint32_t firstPlane, firstX, firstY, firstGot;  // the first of them (x in elements) and what it holds
    uint32_t matchedLaws;     // bit k: every plane reads back exactly with 2^k GOBs a block; bit 8: as pitch linear
    uint32_t padWritten;      // changed bytes inside the planes' blocks but outside the picture
    uint32_t outsideWritten;  // changed bytes outside every plane
} SwitchVICLayoutResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: both pitch-linear copies were exact (else the probe, not the layout, is broken)
    uint32_t cleanCases;     // bit i: results[i] read back exactly with nothing written outside its planes
    SwitchVICLayoutResult results[SWITCH_VIC_LAYOUT_PROBE_CASES];
} SwitchVICLayoutProbeReply;

namespace switch_vic_layout_probe {

constexpr uint32_t kBGRA = 0x42475241u, kNV12 = 0x34323076u;  // 'BGRA', '420v'
constexpr uint32_t kPitch = 0xff, kPitchLaw = 8, kBlockLaws = 6;
constexpr uint32_t kAllLaws = ((1u << kBlockLaws) - 1) | (1u << kPitchLaw);
constexpr uint64_t kBufferBytes = 64 * 1024;
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a;

struct Case {
    uint32_t fourcc, width, height, blockHeightLog2;
};

constexpr Case kCases[SWITCH_VIC_LAYOUT_PROBE_CASES] = {
    {kBGRA, 40, 40, kPitch}, {kNV12, 96, 40, kPitch}, {kBGRA, 40, 40, 0},
    {kBGRA, 40, 40, 1},      {kBGRA, 40, 40, 4},      {kNV12, 96, 40, 1},
};
constexpr uint32_t kControls = 2;

struct Plane {
    uint64_t offset;
    uint32_t pitch, rows;            // bytes a row; rows the plane holds (whole blocks when block linear)
    uint32_t rowBytes, height, bpe;  // the picture's bytes a row and rows; bytes an element
};

struct Layout {
    uint32_t planes;
    Plane plane[2];
};

inline uint32_t roundUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

inline uint64_t roundUp64(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

// 64-byte x 8-row GOBs of 16-byte x 2-row sectors, 2^log2 GOBs stacked in a block, blocks left to right across the
// pitch, a row of blocks every 8 << log2 rows (yuzu's and averne's swizzle; BlockLinear.c is the law at log2 1).
inline uint64_t blockLinearOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2)
{
    const uint32_t blockRows = 8u << log2;
    const uint64_t gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
    return uint64_t(y / blockRows) * pitch * blockRows + uint64_t(x / 64) * (512u << log2) +
           ((y % blockRows) / 8) * 512 + gob;
}

// The byte (x, y) that blockLinearOffset puts at `offset`.
inline void blockLinearPosition(uint64_t offset, uint32_t pitch, uint32_t log2, uint32_t *x, uint32_t *y)
{
    const uint32_t blockRows = 8u << log2;
    const uint64_t blockRow = offset / (uint64_t(pitch) * blockRows), inRow = offset % (uint64_t(pitch) * blockRows);
    const uint32_t block = uint32_t(inRow / (512u << log2)), inBlock = uint32_t(inRow % (512u << log2));
    const uint32_t gobRow = inBlock / 512, g = inBlock % 512;
    *x = block * 64 + ((g >> 8) & 1) * 32 + ((g >> 5) & 1) * 16 + (g & 15);
    *y = uint32_t(blockRow) * blockRows + gobRow * 8 + ((g >> 6) & 3) * 2 + ((g >> 4) & 1);
}

inline uint64_t offsetUnder(uint32_t law, uint32_t x, uint32_t y, uint32_t pitch)
{
    return law == kPitchLaw ? uint64_t(y) * pitch + x : blockLinearOffset(x, y, pitch, law);
}

// Sources at VIC's source row alignment (256 bytes); destinations on whole GOBs, their rows whole blocks.
inline Layout layout(const Case &c, bool destination)
{
    const bool nv12 = c.fourcc == kNV12;
    const bool blockLinear = destination && c.blockHeightLog2 != kPitch;
    Layout l = {};
    l.planes = nv12 ? 2 : 1;
    uint64_t at = 0;
    for (uint32_t p = 0; p < l.planes; p++) {
        Plane &plane = l.plane[p];
        plane.bpe = nv12 ? (p ? 2 : 1) : 4;
        plane.rowBytes = nv12 ? c.width : c.width * 4;
        plane.height = nv12 && p ? c.height / 2 : c.height;
        plane.pitch = roundUp(plane.rowBytes, destination ? 64 : 256);
        plane.rows = blockLinear ? roundUp(plane.height, 8u << c.blockHeightLog2) : plane.height;
        plane.offset = at;
        at = roundUp64(at + uint64_t(plane.pitch) * plane.rows, 256);
    }
    return l;
}

inline TegraVICImage image(const Case &c, bool destination, uint64_t address)
{
    const Layout l = layout(c, destination);
    TegraVICImage im = {};
    im.format = tegra_vic_format(c.fourcc);
    im.width = c.width;
    im.height = c.height;
    for (uint32_t p = 0; p < l.planes; p++) {
        im.pitch[p] = l.plane[p].pitch;
        im.rows[p] = l.plane[p].rows;
        im.base[p] = address + l.plane[p].offset;
        im.span[p] = kBufferBytes - l.plane[p].offset;
    }
    if (destination && c.blockHeightLog2 != kPitch) {
        im.blockLinear = 1;
        im.blockHeightLog2 = c.blockHeightLog2;
    }
    return im;
}

// BGRA pixel (x, y) is B = x, G = y, R = 0xc3, A = 0xff; NV12's bytes mix their column and row.
inline uint8_t sourceByte(const Case &c, uint32_t plane, uint32_t x, uint32_t y)
{
    if (c.fourcc == kBGRA) {
        const uint32_t pixel = 0xffc30000u | (y << 8) | (x / 4);
        return uint8_t(pixel >> (8 * (x % 4)));
    }
    uint32_t v = x * 0x9du + y * 0x3bu + plane * 0x55u;
    v ^= v >> 5;
    return uint8_t(v);
}

inline void fillSource(uint8_t *bytes, const Case &c)
{
    memset(bytes, kSourcePad, kBufferBytes);
    const Layout l = layout(c, false);
    for (uint32_t p = 0; p < l.planes; p++) {
        const Plane &plane = l.plane[p];
        for (uint32_t y = 0; y < plane.height; y++) {
            for (uint32_t x = 0; x < plane.rowBytes; x++)
                bytes[plane.offset + uint64_t(y) * plane.pitch + x] = sourceByte(c, p, x, y);
        }
    }
}

inline bool readsBackUnder(const uint8_t *bytes, const Case &c, const Plane &plane, uint32_t p, uint32_t law)
{
    for (uint32_t y = 0; y < plane.height; y++) {
        for (uint32_t x = 0; x < plane.rowBytes; x++) {
            const uint64_t at = plane.offset + offsetUnder(law, x, y, plane.pitch);
            if (at >= kBufferBytes || bytes[at] != sourceByte(c, p, x, y))
                return false;
        }
    }
    return true;
}

inline bool inPicture(const Case &c, const Plane &plane, uint64_t offset)
{
    uint32_t x = uint32_t(offset % plane.pitch), y = uint32_t(offset / plane.pitch);
    if (c.blockHeightLog2 != kPitch)
        blockLinearPosition(offset, plane.pitch, c.blockHeightLog2, &x, &y);
    return x < plane.rowBytes && y < plane.height;
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICLayoutResult *r)
{
    const Layout l = layout(c, true);
    const uint32_t law = c.blockHeightLog2 == kPitch ? kPitchLaw : c.blockHeightLog2;
    r->matchedLaws = kAllLaws;
    for (uint32_t p = 0; p < l.planes; p++) {
        const Plane &plane = l.plane[p];
        for (uint32_t candidate = 0; candidate <= kPitchLaw; candidate++) {
            if ((r->matchedLaws >> candidate & 1) && !readsBackUnder(bytes, c, plane, p, candidate))
                r->matchedLaws &= ~(1u << candidate);
        }
        for (uint32_t y = 0; y < plane.height; y++) {
            for (uint32_t x = 0; x < plane.rowBytes; x += plane.bpe) {
                uint32_t got = 0;
                bool wrong = false;
                for (uint32_t b = 0; b < plane.bpe; b++) {
                    const uint8_t v = bytes[plane.offset + offsetUnder(law, x + b, y, plane.pitch)];
                    got |= uint32_t(v) << (8 * b);
                    wrong |= v != sourceByte(c, p, x + b, y);
                }
                if (wrong && !r->mismatches++) {
                    r->firstPlane = p;
                    r->firstX = x / plane.bpe;
                    r->firstY = y;
                    r->firstGot = got;
                }
            }
        }
    }
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        if (bytes[at] == kPoison)
            continue;
        const Plane *owner = nullptr;
        for (uint32_t p = 0; p < l.planes; p++) {
            const Plane &plane = l.plane[p];
            if (at >= plane.offset && at < plane.offset + uint64_t(plane.pitch) * plane.rows)
                owner = &plane;
        }
        if (!owner)
            r->outsideWritten++;
        else if (!inPicture(c, *owner, at - owner->offset))
            r->padWritten++;
    }
}

inline bool clean(const SwitchVICLayoutResult &r)
{
    return !r.status && !r.mismatches && !r.outsideWritten;
}

inline void summarise(SwitchVICLayoutProbeReply *reply)
{
    reply->controlsClean = reply->count >= kControls;
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
        else if (i < kControls)
            reply->controlsClean = 0;
    }
}

}  // namespace switch_vic_layout_probe
