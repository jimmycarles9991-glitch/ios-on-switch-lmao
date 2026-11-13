// SPDX-License-Identifier: BSD-2-Clause-Patent
// The slot composition probe, debug kernels only (selector 6, SwitchVICKernelTest.h). kTegraVICTraits.multiSlot is 1
// with no job of two slots ever run, and VIC 4.0's blend select codes are VIC 4.1's by inference (TegraVICCompose.h).
// Small BGRA cards and a flat NV12 picture, all in probe buffer 0, are composed into a 64 x 32 BGRA destination: one
// slot through tegra_vic_build (the control) and through tegra_vic_compose_build, two disjoint slots, overlapping
// opaque slots in both orders, a premultiplied and a straight per-pixel alpha slot, a half global alpha, a slot scaled
// down 2:1 from a flat card, and the premultiplied case into GENERIC_16Bx2 blocks of 16 GOBs. Two more change one thing
// after the build: overlapping opaque slots with BlendingSlotStruct all zero (as every single-slot job has run, and
// hekate's), and the premultiplied case with alpha fill COMPOSITED, the mode NVIDIA's VIC 5.1 header ties the blend
// factors to. Each result is read back
// against several readings of what the engine did (Model): the blend as programmed, the top slot replacing what lies
// under it, the top slot missing, the colour read premultiplied or straight whatever was asked, and nothing drawn, so
// a wrong reading says which one the engine follows. Pictures are 1:1 or flat, so no filter tap changes a pixel. Plain
// logic, no IOKit: SwitchVICComposeProbe.cpp runs it, test/switch_vic_compose_probe_test.cpp checks it.
#pragma once
#include <stdint.h>
#include <string.h>
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "TegraVICCompose.h"

#define SWITCH_VIC_COMPOSE_PROBE_MAGIC 0x56434d50u  // 'VCMP'
// HW Test display.planes_composite: arm the display's composite probe, then report and disarm it
// (NativeMobileDisplay/SwitchPlaneCompositeProbe.h). The reply: magic, armed, frames composited, the display's error.
#define SWITCH_VIC_COMPOSITE_ARM_MAGIC    0x56434641u  // 'VCFA'
#define SWITCH_VIC_COMPOSITE_REPORT_MAGIC 0x56434652u  // 'VCFR'
#define SWITCH_VIC_COMPOSITE_REPLY_BYTES  16u
#define SWITCH_VIC_COMPOSE_PROBE_CASES    12u

typedef struct {
    uint32_t slots;                 // 0: the control (tegra_vic_build)
    int32_t status;                 // 0, -95 refused by the builder, else the job's error
    uint32_t models;                // bit m: every pixel within kTolerance of Model m
    uint32_t mismatches, maxError;  // against the case's own model
    uint32_t firstX, firstY, firstGot, firstWant;
    uint32_t outsideWritten;  // changed bytes outside the destination's rows (block linear: its blocks)
} SwitchVICComposeResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: the tegra_vic_build copy was exact
    uint32_t cleanCases;     // bit i: results[i] met its own model, nothing written outside
    SwitchVICComposeResult results[SWITCH_VIC_COMPOSE_PROBE_CASES];
} SwitchVICComposeProbeReply;

namespace switch_vic_compose_probe {

constexpr uint32_t kWidth = 64, kHeight = 32, kCardWidth = 32, kCardHeight = 16, kTolerance = 3;
constexpr uint32_t kPitch = 256, kBlockLog2 = 4, kBlockRows = 8u << kBlockLog2;
constexpr uint64_t kBufferBytes = 64 * 1024;
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a;
constexpr uint8_t kLuma = 120, kCb = 90, kCr = 170;
constexpr uint32_t kAlphaFillComposited = 4;

enum Source : uint32_t {
    kOpaqueA,
    kOpaqueC,
    kAlphaB,
    kVideo,
    kFlat,
    kSources
};

// Byte offsets in buffer 0: every card at VIC's 256-byte source row pitch.
constexpr uint64_t kOffset[kSources] = {0, 4096, 8192, 12288, 20480};
constexpr uint64_t kVideoChroma = 16384;

enum Model : uint32_t {
    kProgrammed,
    kTopReplaces,
    kTopMissing,
    kAsPremultiplied,
    kAsStraight,
    kNothing,
    kModels
};

struct Placed {
    Source source;
    uint32_t x, y, w, h;
    uint32_t blend, alpha10;
};

enum Tweak : uint32_t {
    kAsBuilt,
    kZeroBlend,
    kCompositedFill,
};

struct Case {
    uint32_t slots;
    Placed slot[2];
    bool blockLinear;
    Tweak tweak;
};

constexpr uint32_t kOpaque = TEGRA_VIC_BLEND_OPAQUE, kPremultiplied = TEGRA_VIC_BLEND_PREMULTIPLIED,
                   kStraight = TEGRA_VIC_BLEND_STRAIGHT;

constexpr Case kCases[SWITCH_VIC_COMPOSE_PROBE_CASES] = {
    {0, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {1, {{kOpaqueA, 8, 4, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kVideo, 32, 16, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kOpaqueC, 16, 8, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueC, 16, 8, 32, 16, kOpaque, 1023}, {kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kAlphaB, 16, 8, 32, 16, kPremultiplied, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kAlphaB, 16, 8, 32, 16, kStraight, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kOpaqueC, 16, 8, 32, 16, kOpaque, 512}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kFlat, 16, 8, 32, 16, kOpaque, 1023}}, false, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kAlphaB, 16, 8, 32, 16, kPremultiplied, 1023}}, true, kAsBuilt},
    {2, {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kOpaqueC, 16, 8, 32, 16, kOpaque, 1023}}, false, kZeroBlend},
    {2,
     {{kOpaqueA, 0, 0, 32, 16, kOpaque, 1023}, {kAlphaB, 16, 8, 32, 16, kPremultiplied, 1023}},
     false,
     kCompositedFill},
};

struct Rgba {
    uint32_t c[4];  // R, G, B, A, 8-bit
};

inline uint32_t sourceWidth(Source s)
{
    return s == kFlat ? 2 * kCardWidth : kCardWidth;
}

inline uint32_t sourceHeight(Source s)
{
    return s == kFlat ? 2 * kCardHeight : kCardHeight;
}

// A card's colour at (x, y), straight for kAlphaB (its stored colour is this times alpha).
inline Rgba cardColour(Source s, uint32_t x, uint32_t y)
{
    switch (s) {
    case kOpaqueA:
        return {{90, 200 - 5 * y, 40 + 3 * x, 255}};
    case kOpaqueC:
        return {{160 - 4 * y, 30 + 2 * x, 220, 255}};
    case kAlphaB:
        return {{250 - 3 * x, 60 + 6 * y, 180, 32 + 7 * x}};
    case kFlat:
        return {{70, 140, 210, 255}};
    default:
        return {{0, 0, 0, 255}};
    }
}

inline uint32_t premultiply(uint32_t c, uint32_t a)
{
    return (c * a + 127) / 255;
}

inline uint32_t packed(const Rgba &p)
{
    return p.c[3] << 24 | p.c[0] << 16 | p.c[1] << 8 | p.c[2];
}

// The flat NV12 picture in RGB, BT.601 video range through the engine's arithmetic (samples widened x << 2 | x >> 6).
inline Rgba videoColour()
{
    const TegraVICTraits &t = kTegraVICTraits;
    TegraVICMatrix m;
    tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_601, false, &t.in, &m);
    const auto widen = [](uint32_t v) {
        return v << 2 | v >> 6;
    };
    uint32_t in10[3] = {}, out10[3] = {};
    in10[0] = widen(kLuma);
    in10[t.in.cbChannel] = widen(kCb);
    in10[t.in.crChannel] = widen(kCr);
    tegra_vic_matrix_apply(&m, in10, out10);
    return {{out10[0] >> 2, out10[1] >> 2, out10[2] >> 2, 255}};
}

// What a slot's source holds at its own (x, y): colour as stored (kAlphaB premultiplied).
inline Rgba stored(Source s, uint32_t x, uint32_t y)
{
    if (s == kVideo)
        return videoColour();
    Rgba p = cardColour(s, x, y);
    if (s == kAlphaB)
        for (uint32_t i = 0; i < 3; i++)
            p.c[i] = premultiply(p.c[i], p.c[3]);
    return p;
}

inline void fillSource(uint8_t *bytes)
{
    memset(bytes, kSourcePad, kBufferBytes);
    for (uint32_t s = 0; s < kSources; s++) {
        if (s == kVideo) {
            for (uint32_t y = 0; y < kCardHeight; y++)
                memset(bytes + kOffset[s] + uint64_t(y) * kPitch, kLuma, kCardWidth);
            for (uint32_t y = 0; y < kCardHeight / 2; y++)
                for (uint32_t x = 0; x < kCardWidth; x += 2) {
                    bytes[kVideoChroma + uint64_t(y) * kPitch + x] = kCb;
                    bytes[kVideoChroma + uint64_t(y) * kPitch + x + 1] = kCr;
                }
            continue;
        }
        const Source source = Source(s);
        for (uint32_t y = 0; y < sourceHeight(source); y++)
            for (uint32_t x = 0; x < sourceWidth(source); x++) {
                const uint32_t v = packed(stored(source, x, y));
                memcpy(bytes + kOffset[s] + uint64_t(y) * kPitch + 4ull * x, &v, 4);
            }
    }
}

inline TegraVICImage sourceImage(Source s, uint64_t address)
{
    TegraVICImage im = {};
    im.format =
        tegra_vic_format(s == kVideo ? TEGRA_VIC_FOURCC('4', '2', '0', 'v') : TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = sourceWidth(s);
    im.height = sourceHeight(s);
    im.pitch[0] = kPitch;
    im.rows[0] = im.height;
    im.base[0] = address + kOffset[s];
    im.span[0] = uint64_t(kPitch) * im.height;
    if (s == kVideo) {
        im.pitch[1] = kPitch;
        im.rows[1] = im.height / 2;
        im.base[1] = address + kVideoChroma;
        im.span[1] = uint64_t(kPitch) * im.rows[1];
        im.standard = TEGRA_VIC_YCC_601;
    }
    return im;
}

inline TegraVICImage destinationImage(bool blockLinear, uint64_t address)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = kWidth;
    im.height = kHeight;
    im.pitch[0] = kPitch;
    im.rows[0] = blockLinear ? kBlockRows : kHeight;
    im.base[0] = address;
    im.span[0] = uint64_t(kPitch) * im.rows[0];
    im.blockLinear = blockLinear;
    im.blockHeightLog2 = blockLinear ? kBlockLog2 : 0;
    return im;
}

// The job for case c; null, or why the builder refuses it.
inline const char *build(const Case &c, uint64_t source, uint64_t destination, uint64_t config, uint64_t filter,
                         TegraVICJob *job)
{
    TegraVICImage images[2] = {};
    TegraVICComposeSlot slots[2] = {};
    const uint32_t count = c.slots ? c.slots : 1;
    for (uint32_t i = 0; i < count; i++) {
        const Placed &p = c.slot[i];
        images[i] = sourceImage(p.source, source);
        slots[i].source = &images[i];
        slots[i].srcW = uint64_t(images[i].width) << 16;
        slots[i].srcH = uint64_t(images[i].height) << 16;
        slots[i].dstX = p.x;
        slots[i].dstY = p.y;
        slots[i].dstW = p.w;
        slots[i].dstH = p.h;
        slots[i].blend = p.blend;
        slots[i].alpha10 = p.alpha10;
        slots[i].nearest = 1;
    }
    const TegraVICImage out = destinationImage(c.blockLinear, destination);
    if (!c.slots) {
        TegraVICOp op = {};
        op.srcW = slots[0].srcW;
        op.srcH = slots[0].srcH;
        op.dstX = slots[0].dstX;
        op.dstY = slots[0].dstY;
        op.dstW = slots[0].dstW;
        op.dstH = slots[0].dstH;
        op.nearest = 1;
        return tegra_vic_build(&images[0], &out, &op, &kTegraVICTraits, config, filter, job);
    }
    const TegraVICRect target = {0, 0, kWidth, kHeight};
    if (const char *why = tegra_vic_compose_build(slots, count, &out, &target, &kTegraVICTraits, config, filter, job))
        return why;
    for (uint32_t i = 0; i < count && c.tweak == kZeroBlend; i++)
        memset(job->config + TEGRA_VIC_SLOTS + i * TEGRA_VIC_SLOT_BYTES + TEGRA_VIC_SLOT_BLEND, 0, 16);
    if (c.tweak == kCompositedFill)
        TEGRA_VIC_SET(job->config, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE, kAlphaFillComposited);
    return nullptr;
}

inline uint32_t over(uint32_t src, uint32_t dst, uint32_t srcFactor255, uint32_t dstFactor255)
{
    const uint32_t v = (src * srcFactor255 + dst * dstFactor255 + 127) / 255;
    return v > 255 ? 255 : v;
}

// The pixel at (x, y) under model m. The control and anything outside the target keep the poison.
inline bool expected(const Case &c, Model m, uint32_t x, uint32_t y, Rgba &out)
{
    if (!c.slots) {
        const Placed &p = c.slot[0];
        if (x < p.x || y < p.y || x >= p.x + p.w || y >= p.y + p.h)
            return false;
        out = stored(p.source, x - p.x, y - p.y);
        return true;
    }
    out = {{0, 0, 0, 255}};
    if (m == kNothing)
        return true;
    for (uint32_t i = 0; i < c.slots; i++) {
        const Placed &p = c.slot[i];
        const bool top = i + 1 == c.slots && c.slots > 1;
        if (x < p.x || y < p.y || x >= p.x + p.w || y >= p.y + p.h || (top && m == kTopMissing))
            continue;
        const uint32_t sx = (x - p.x) * sourceWidth(p.source) / p.w, sy = (y - p.y) * sourceHeight(p.source) / p.h;
        const Rgba s = stored(p.source, sx, sy);
        if (top && m == kTopReplaces) {
            out = {{s.c[0], s.c[1], s.c[2], 255}};
            continue;
        }
        uint32_t blend = p.blend;
        if (top && m == kAsPremultiplied && blend != kOpaque)
            blend = kPremultiplied;
        if (top && m == kAsStraight && blend != kOpaque)
            blend = kStraight;
        const uint32_t g = (p.alpha10 * 255 + 511) / 1023;
        const uint32_t a = blend == kOpaque ? 255 : s.c[3];
        const uint32_t ga = (g * a + 127) / 255;
        const uint32_t srcFactor = blend == kStraight ? ga : g;
        for (uint32_t ch = 0; ch < 3; ch++)
            out.c[ch] = over(s.c[ch], out.c[ch], srcFactor, 255 - ga);
    }
    out.c[3] = 255;
    return true;
}

inline uint32_t distance(const Rgba &want, uint32_t got)
{
    uint32_t most = 0;
    const uint32_t channel[4] = {(got >> 16) & 0xff, (got >> 8) & 0xff, got & 0xff, got >> 24};
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t d = channel[i] > want.c[i] ? channel[i] - want.c[i] : want.c[i] - channel[i];
        most = d > most ? d : most;
    }
    return most;
}

// Where the destination's pixel (x, y) is in buffer 1.
inline uint64_t pixelOffset(bool blockLinear, uint32_t x, uint32_t y)
{
    return blockLinear ? tgm_layout_offset_of(kPitch, kBlockLog2, 4 * x, y) : uint64_t(y) * kPitch + 4ull * x;
}

inline uint32_t pixelAt(const uint8_t *bytes, bool blockLinear, uint32_t x, uint32_t y)
{
    uint32_t v = 0;
    memcpy(&v, bytes + pixelOffset(blockLinear, x, y), 4);
    return v;
}

inline void compare(const uint8_t *bytes, const Case &c, SwitchVICComposeResult *r)
{
    r->models = (1u << kModels) - 1;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            const uint32_t got = pixelAt(bytes, c.blockLinear, x, y);
            for (uint32_t m = 0; m < kModels; m++) {
                Rgba want;
                const bool written = expected(c, Model(m), x, y, want);
                const uint32_t poison = 0x01010101u * kPoison;
                const uint32_t off = written ? distance(want, got) : (got == poison ? 0 : 255);
                if (off > kTolerance)
                    r->models &= ~(1u << m);
                if (m != kProgrammed)
                    continue;
                r->maxError = off > r->maxError ? off : r->maxError;
                if (off > kTolerance && !r->mismatches++) {
                    r->firstX = x;
                    r->firstY = y;
                    r->firstGot = got;
                    r->firstWant = written ? packed(want) : poison;
                }
            }
        }
    const uint64_t extent = uint64_t(kPitch) * (c.blockLinear ? kBlockRows : kHeight);
    for (uint64_t at = extent; at < kBufferBytes; at++)
        r->outsideWritten += bytes[at] != kPoison;
}

inline bool clean(const SwitchVICComposeResult &r)
{
    return !r.status && !r.mismatches && !r.outsideWritten;
}

inline void summarise(SwitchVICComposeProbeReply *reply)
{
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++)
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
    reply->controlsClean = reply->cleanCases & 1u;
}

}  // namespace switch_vic_compose_probe
