// SwitchEMCGovernor.h: the EMC rates the governor uses, the ACTMON watermarks it arms and its moves.
#include "drivers/SwitchPower/SwitchEMCGovernor.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

// hekate's mtc_tbl_b01.bin rows (204, 1331.2, 1600 MHz) with the Arachne rated row the firmware may add.
constexpr uint32_t kTable[] = {204000, 1331200, 1600000, 1866000};

struct Rates {
    uint32_t khz[SWITCH_EMC_MAX_RATES] = {};
    unsigned n = 0;
};

Rates Usable(uint32_t ceiling)
{
    Rates r;
    r.n = switch_emc_rates(kTable, 4, ceiling, r.khz);
    return r;
}

TEST(SwitchEmcGovernor, RatesUpToTheBootRateAscending)
{
    const Rates r = Usable(1600000);
    ASSERT_EQ(r.n, 3u);
    EXPECT_EQ(r.khz[0], 204000u);
    EXPECT_EQ(r.khz[1], 1331200u);
    EXPECT_EQ(r.khz[2], 1600000u);

    const uint32_t shuffled[] = {1600000, 0, 204000, 1331200, 204000, 2133000};
    uint32_t out[SWITCH_EMC_MAX_RATES] = {};
    ASSERT_EQ(switch_emc_rates(shuffled, 6, 1600000, out), 3u);
    EXPECT_EQ(out[0], 204000u);
    EXPECT_EQ(out[1], 1331200u);
    EXPECT_EQ(out[2], 1600000u);
}

TEST(SwitchEmcGovernor, TheRowNowToleratesTheFirmwaresRounding)
{
    const Rates r = Usable(1600000 + 1600);
    ASSERT_EQ(r.n, 3u);
    EXPECT_EQ(switch_emc_row(r.khz, r.n, 1600000), 2u);
    EXPECT_EQ(switch_emc_row(r.khz, r.n, 1599999), 2u);
    EXPECT_EQ(switch_emc_row(r.khz, r.n, 1331199), 1u);
    EXPECT_EQ(switch_emc_row(r.khz, r.n, 204000), 0u);
    EXPECT_EQ(switch_emc_row(r.khz, r.n, 800000), 3u);
    EXPECT_EQ(Usable(1599999 + 1599).n, 3u) << "a top rate read one kHz low still keeps its row";
}

TEST(SwitchEmcGovernor, WatermarksAtEachRate)
{
    const Rates r = Usable(1600000);
    const switch_emc_wmarks low = switch_emc_watermarks(r.khz, r.n, 0, false);
    EXPECT_TRUE(low.up_on);
    EXPECT_FALSE(low.down_on);
    EXPECT_EQ(low.up, 204000u * 4 * 60 / 100);

    const switch_emc_wmarks top = switch_emc_watermarks(r.khz, r.n, 2, false);
    EXPECT_FALSE(top.up_on);
    EXPECT_TRUE(top.down_on);
    EXPECT_EQ(top.down, 1331200u * 4 * 40 / 100);

    const switch_emc_wmarks mid = switch_emc_watermarks(r.khz, r.n, 1, false);
    EXPECT_TRUE(mid.up_on && mid.down_on);
    EXPECT_EQ(mid.down, 204000u * 4 * 40 / 100);
    EXPECT_LT(mid.down, mid.up);
}

TEST(SwitchEmcGovernor, AStepDownLeavesTheLowerRateFortyPercentBusyAtMost)
{
    const Rates r = Usable(1600000);
    for (unsigned cur = 1; cur < r.n; ++cur) {
        const switch_emc_wmarks w = switch_emc_watermarks(r.khz, r.n, cur, false);
        EXPECT_LE(uint64_t{w.down} * 100, uint64_t{r.khz[cur - 1]} * SWITCH_EMC_SAMPLE_MS * 40) << cur;
        const switch_emc_wmarks below = switch_emc_watermarks(r.khz, r.n, cur - 1, false);
        EXPECT_LT(w.down, below.up) << "a step down must not trip the lower rate's up watermark at once";
    }
}

TEST(SwitchEmcGovernor, WhileHeldTheDownWatermarkIsOff)
{
    const Rates r = Usable(1600000);
    EXPECT_FALSE(switch_emc_watermarks(r.khz, r.n, 2, true).down_on);
    EXPECT_FALSE(switch_emc_watermarks(r.khz, r.n, 1, true).down_on);
}

TEST(SwitchEmcGovernor, UpGoesStraightToTheTop)
{
    EXPECT_EQ(switch_emc_next(3, 0, SWITCH_EMC_EVENT_UP, false), 2u);
    EXPECT_EQ(switch_emc_next(3, 1, SWITCH_EMC_EVENT_UP, false), 2u);
    EXPECT_EQ(switch_emc_next(3, 0, SWITCH_EMC_EVENT_UP | SWITCH_EMC_EVENT_DOWN, false), 2u);
}

TEST(SwitchEmcGovernor, DownStepsOneRateAtATime)
{
    EXPECT_EQ(switch_emc_next(3, 2, SWITCH_EMC_EVENT_DOWN, false), 1u);
    EXPECT_EQ(switch_emc_next(3, 1, SWITCH_EMC_EVENT_DOWN, false), 0u);
    EXPECT_EQ(switch_emc_next(3, 0, SWITCH_EMC_EVENT_DOWN, false), 0u);
    EXPECT_EQ(switch_emc_next(3, 1, 0, false), 1u);
}

TEST(SwitchEmcGovernor, FramesAndHintsHoldTheTop)
{
    for (unsigned cur = 0; cur < 3; ++cur) {
        EXPECT_EQ(switch_emc_next(3, cur, SWITCH_EMC_EVENT_DOWN, true), 2u) << cur;
        EXPECT_EQ(switch_emc_next(3, cur, 0, true), 2u) << cur;
    }
}

TEST(SwitchEmcGovernor, TheDownWindowOutlastsARefreshPeriod)
{
    EXPECT_GE(SWITCH_EMC_SAMPLE_MS * SWITCH_EMC_DOWN_SAMPLES * 1000, 16667u);
    EXPECT_LE(SWITCH_EMC_DOWN_SAMPLES, 8u);  // ACTMON's DOWN_WMARK_NUM is three bits
}

}  // namespace
