// SPDX-License-Identifier: BSD-2-Clause-Patent
// The row-pitch probe, debug kernels only (selector 6, SwitchVICKernelTest.h): struct in SwitchVICPitchProbeRequest,
// struct out SwitchVICPitchProbeReply. The deleted boot self-test lost half of a 1:1 copy at 144- and 192-byte source
// pitches, so kTegraVICTraits.sourcePitchAlign is 256 and every source whose rows are not whole multiples of 256 bytes
// goes through the copy engine's bounce (TegraVICBounce.h). Here VIC copies a BGRA card of 32 rows 1:1 at 320, 576
// and 2880 bytes a row (2880: a 720-pixel row), on the source and then on the destination with the
// other side at the next multiple of 256; 256, 512 and 3072 on both sides are the controls. Every pixel names its own
// column and row, so the first wrong one says which source pixel the engine fetched. The destination is poisoned,
// including each row's tail and 32 rows after the card, so a stride the engine rounds up shows as bytes written outside
// the card. Plain logic, no IOKit: SwitchVICPitchProbe.cpp runs it, test/switch_vic_pitch_probe_test.cpp checks it.
#pragma once
#include <stdint.h>
#include <string.h>

#define SWITCH_VIC_PITCH_PROBE_MAGIC 0x56504954u  // 'VPIT'
#define SWITCH_VIC_PITCH_PROBE_CASES 9u

typedef struct {
    uint32_t magic, reserved;
} SwitchVICPitchProbeRequest;

typedef struct {
    uint32_t sourcePitch, destinationPitch, width;  // bytes a row; pixels a row
    int32_t status;                                 // 0, -95 refused by tegra_vic_build, else the job's error
    uint32_t mismatches;                            // of width x 32 pixels
    uint32_t firstX, firstY, firstGot;              // the first wrong pixel and what it holds
    uint32_t outsideWritten;                        // poisoned destination bytes outside the card that changed
} SwitchVICPitchResult;

typedef struct {
    uint32_t magic, count;
    uint32_t controlsClean;  // 1: every control copied exactly (else the probe, not the pitch, is broken)
    uint32_t cleanCases;     // bit i: results[i] copied exactly with nothing written outside the card
    SwitchVICPitchResult results[SWITCH_VIC_PITCH_PROBE_CASES];
} SwitchVICPitchProbeReply;

namespace switch_vic_pitch_probe {

struct Case {
    uint32_t sourcePitch, destinationPitch;
};

constexpr Case kCases[SWITCH_VIC_PITCH_PROBE_CASES] = {
    {320, 512}, {512, 320}, {576, 768}, {768, 576}, {2880, 3072}, {3072, 2880}, {256, 256}, {512, 512}, {3072, 3072},
};
constexpr uint32_t kFirstControl = 6;
constexpr uint32_t kRows = 32, kSlackRows = 32, kMaxPitch = 3072;
constexpr uint64_t kBufferBytes = uint64_t(kMaxPitch) * (kRows + kSlackRows);
constexpr uint8_t kPoison = 0xab, kSourcePad = 0x5a;

inline uint32_t width(const Case &c)
{
    return (c.sourcePitch < c.destinationPitch ? c.sourcePitch : c.destinationPitch) / 4;
}

// B: column bits 0-7; G: column bits 8-9 and row << 3; R 0xc3; A opaque.
inline uint32_t pixel(uint32_t x, uint32_t y)
{
    return 0xffc30000u | (((x >> 8) | (y << 3)) << 8) | (x & 0xff);
}

inline void fillSource(uint8_t *bytes, uint32_t pitch, uint32_t pixels)
{
    memset(bytes, kSourcePad, kBufferBytes);
    for (uint32_t y = 0; y < kRows; y++) {
        for (uint32_t x = 0; x < pixels; x++) {
            const uint32_t p = pixel(x, y);
            memcpy(bytes + uint64_t(y) * pitch + 4ull * x, &p, 4);
        }
    }
}

inline void compare(const uint8_t *bytes, uint32_t pitch, uint32_t pixels, SwitchVICPitchResult *r)
{
    for (uint64_t at = 0; at < kBufferBytes; at++) {
        const uint64_t y = at / pitch, column = at % pitch;
        if (y >= kRows || column >= 4ull * pixels) {
            r->outsideWritten += bytes[at] != kPoison;
            continue;
        }
        if (column % 4)
            continue;
        uint32_t got;
        memcpy(&got, bytes + at, 4);
        const uint32_t x = uint32_t(column / 4);
        if (got != pixel(x, uint32_t(y)) && !r->mismatches++) {
            r->firstX = x;
            r->firstY = uint32_t(y);
            r->firstGot = got;
        }
    }
}

inline bool clean(const SwitchVICPitchResult &r)
{
    return !r.status && !r.mismatches && !r.outsideWritten;
}

inline void summarise(SwitchVICPitchProbeReply *reply)
{
    reply->controlsClean = 1;
    reply->cleanCases = 0;
    for (uint32_t i = 0; i < reply->count; i++) {
        if (clean(reply->results[i]))
            reply->cleanCases |= 1u << i;
        else if (i >= kFirstControl)
            reply->controlsClean = 0;
    }
}

}  // namespace switch_vic_pitch_probe
