// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICPitchProbe.h"

namespace {
using namespace switch_vic_pitch_probe;

uint32_t roundUp(uint32_t pitch)
{
    return (pitch + 255) & ~255u;
}

// An engine that copies kRows rows of `pixels` from the source at `readStride` into the destination at `writeStride`.
SwitchVICPitchResult run(const Case &c, uint32_t readStride, uint32_t writeStride)
{
    std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
    const uint32_t pixels = width(c);
    fillSource(source.data(), c.sourcePitch, pixels);
    for (uint32_t y = 0; y < kRows; y++) {
        const uint64_t to = uint64_t(y) * writeStride, from = uint64_t(y) * readStride;
        const uint64_t bytes = 4ull * pixels;
        if (to + bytes <= kBufferBytes && from + bytes <= kBufferBytes)
            memcpy(destination.data() + to, source.data() + from, bytes);
    }
    SwitchVICPitchResult r = {c.sourcePitch, c.destinationPitch, pixels, 0, 0, 0, 0, 0, 0};
    compare(destination.data(), c.destinationPitch, pixels, &r);
    return r;
}
}  // namespace

TEST(SwitchVICPitchProbe, EveryCaseFitsItsBuffersAndHasAnOddPitchOrIsAControl)
{
    for (uint32_t i = 0; i < SWITCH_VIC_PITCH_PROBE_CASES; i++) {
        const Case &c = kCases[i];
        // A stride the engine rounds up to 256 still lands inside the buffers, where the poison shows it.
        EXPECT_LE(uint64_t(roundUp(c.sourcePitch)) * kRows, kBufferBytes);
        EXPECT_LE(uint64_t(roundUp(c.destinationPitch)) * kRows, kBufferBytes);
        EXPECT_LE(width(c) * 4, c.sourcePitch);
        EXPECT_LE(width(c) * 4, c.destinationPitch);
        EXPECT_EQ(c.sourcePitch % 64, 0u);
        EXPECT_EQ(c.destinationPitch % 64, 0u);
        const bool control = c.sourcePitch == c.destinationPitch && c.sourcePitch % 256 == 0;
        EXPECT_EQ(control, i >= kFirstControl) << i;
    }
}

TEST(SwitchVICPitchProbe, AnExactCopyIsClean)
{
    for (const Case &c : kCases) {
        const SwitchVICPitchResult r = run(c, c.sourcePitch, c.destinationPitch);
        EXPECT_TRUE(clean(r)) << c.sourcePitch << " -> " << c.destinationPitch << ": " << r.mismatches << " wrong, "
                              << r.outsideWritten << " outside";
    }
}

TEST(SwitchVICPitchProbe, ADestinationStrideRoundedTo256WritesOutsideTheCard)
{
    const Case c = kCases[3];  // 768 -> 576
    const SwitchVICPitchResult r = run(c, c.sourcePitch, roundUp(c.destinationPitch));
    EXPECT_GT(r.mismatches, 0u);
    EXPECT_GT(r.outsideWritten, 0u);
    EXPECT_EQ(r.firstY, 1u) << "row 0 lands in place, row 1 does not";
}

TEST(SwitchVICPitchProbe, TheFirstWrongPixelNamesWhatTheEngineFetched)
{
    const Case c = kCases[0];  // 320 -> 512
    const SwitchVICPitchResult r = run(c, roundUp(c.sourcePitch), c.destinationPitch);
    ASSERT_GT(r.mismatches, 0u);
    EXPECT_EQ(r.firstX, 0u);
    EXPECT_EQ(r.firstY, 1u);
    // Destination row 1 starts 512 bytes into the 320-byte-row source: row 1, pixel (512 - 320) / 4 = 48.
    EXPECT_EQ(r.firstGot, pixel(48, 1));
    EXPECT_EQ(r.outsideWritten, 0u);
}

TEST(SwitchVICPitchProbe, AFailedControlMarksTheProbeBroken)
{
    SwitchVICPitchProbeReply reply = {};
    reply.count = SWITCH_VIC_PITCH_PROBE_CASES;
    reply.results[1].mismatches = 3;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, 0x1fdu);
    reply.results[kFirstControl + 1].status = -5;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 0u);
    EXPECT_EQ(reply.cleanCases, 0x17du);
}

TEST(SwitchVICPitchProbe, EveryPixelOfTheCardIsDistinct)
{
    std::vector<bool> seen(1u << 16);
    for (uint32_t y = 0; y < kRows; y++) {
        for (uint32_t x = 0; x < kMaxPitch / 4; x++) {
            const uint32_t key = pixel(x, y) & 0xffff;
            EXPECT_FALSE(seen[key]) << x << "," << y;
            seen[key] = true;
        }
    }
}
