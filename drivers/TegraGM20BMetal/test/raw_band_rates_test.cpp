// SPDX-License-Identifier: MIT
// raw/runtime/raw_band_rates.h on the host: a state is priced by its instructions until one of its large draws is
// measured, then by the measured rate, so a draw bound by memory or the ROP is banded as long as it really is.
#include <gtest/gtest.h>

#include <cstdint>

#include "../raw/runtime/raw_band_rates.h"
#include "../raw/runtime/raw_draw_bands.h"

namespace {

using raw::BandDraw;
using raw::BandRate;

constexpr uint64_t kScreen = 720 * 1280;

uint32_t bandsOf(const BandRate &rate, uint64_t units, uint32_t instructions)
{
    BandDraw d;
    d.fragmentNs = raw::estimatedNs(rate, units, raw::workNs(raw::laneWork(units, instructions)));
    d.rows = 1280;
    return raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer);
}

// The poster's 4x resolve (T-v2 gpuprof: 11.5-12 ms over a 720x1280 drawable, 642 instructions): priced 3 ms, so four
// bands of what are really three-millisecond pieces; measured once, twelve bands of one
TEST(BandRates, AMeasuredResolveIsBandedByItsMeasuredLength)
{
    BandRate rate;
    EXPECT_EQ(raw::estimatedNs(rate, kScreen, 3'009'000), 3'009'000u);
    EXPECT_EQ(bandsOf(rate, kScreen, 642), 4u);
    raw::learn(rate, 11'500'000, kScreen);
    EXPECT_EQ(rate.samples.load(), 1u);
    EXPECT_NEAR(double(raw::estimatedNs(rate, kScreen, 0)), 11.5e6, 2e3);
    EXPECT_EQ(bandsOf(rate, kScreen, 642), 12u);
}

// A full-screen draw of a ten-instruction program into a 4x RGBA16F target with blending: 0.19 ms by its instructions,
// five milliseconds of ROP traffic; once measured it is banded
TEST(BandRates, AMemoryBoundDrawIsBandedOnceMeasured)
{
    BandRate rate;
    const uint64_t units = kScreen * 4;
    EXPECT_EQ(bandsOf(rate, units, 10), 1u);
    raw::learn(rate, 5'000'000, units);
    EXPECT_EQ(bandsOf(rate, units, 10), 5u);
    EXPECT_EQ(bandsOf(rate, units / 4, 10), 2u);
}

TEST(BandRates, ANewSampleMovesAWarmRateByAQuarter)
{
    BandRate rate;
    raw::learn(rate, 4'000'000, 1'000'000);
    EXPECT_EQ(rate.psPerUnit.load(), 4000u);
    raw::learn(rate, 8'000'000, 1'000'000);
    EXPECT_EQ(rate.psPerUnit.load(), 5000u);
    raw::learn(rate, 0, 1'000'000);
    raw::learn(rate, 1'000'000, 0);
    EXPECT_EQ(rate.psPerUnit.load(), 5000u);
    EXPECT_EQ(rate.samples.load(), 2u);
    raw::learn(rate, 1, 1'000'000'000);
    EXPECT_GE(rate.psPerUnit.load(), 1u);
}

// Small draws are never measured; a state's first large ones are, then one in kRemeasureEvery
TEST(BandRates, ItsFirstLargeDrawsAreMeasuredThenOneInAWhile)
{
    BandRate rate;
    EXPECT_FALSE(raw::measure(rate, raw::kMeasureUnits - 1));
    for (uint32_t i = 0; i < raw::kWarmSamples; i++) {
        EXPECT_TRUE(raw::measure(rate, raw::kMeasureUnits));
        raw::learn(rate, 1'000'000, raw::kMeasureUnits);
    }
    uint32_t measured = 0;
    for (uint32_t i = 0; i < 4 * raw::kRemeasureEvery; i++)
        measured += raw::measure(rate, raw::kMeasureUnits);
    EXPECT_EQ(measured, 4u);
}

TEST(BandRates, ACopiedStateKeepsItsRate)
{
    BandRate rate;
    raw::learn(rate, 2'000'000, 1'000'000);
    const BandRate copy = rate;
    EXPECT_EQ(copy.psPerUnit.load(), 2000u);
    EXPECT_EQ(copy.samples.load(), 1u);
}

TEST(BandRates, AHugeDrawsPriceDoesNotOverflow)
{
    BandRate rate;
    rate.psPerUnit = UINT32_MAX;
    EXPECT_GT(raw::estimatedNs(rate, UINT64_MAX / 2, 0), uint64_t(1e15));
}

}  // namespace
