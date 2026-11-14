// SPDX-License-Identifier: GPL-2.0
// The CPU's clock against the rate asked of the DFLL, with the vendored clk-dfll.c over tegra_dfll_landing_model.c
// (Tegra X1 TRM 6.1.6 DFLL_FREQ_REQ MULT and SCALE, 6.1.9 DFLL_OUTPUT_CFG MIN). Boot 21 (11-13) ran VDD_CPU at its
// 855 mV floor for 1683 and 1785 MHz alike and measured 1691 and 1783-1795 MHz there: a DVCO floor near 1797 MHz over
// a calibrated minimum of 1785, from which both rows were skipped.
#include "drivers/TegraDFLL/test/tegra_dfll_landing_model.h"

#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

namespace
{
constexpr unsigned long kMHz = 1000000;
constexpr unsigned long kPerLutStep = 30 * kMHz;  // 15 mV
constexpr unsigned int kSettleMs = 200;  // five Vmin calibration periods (DFLL_CALIBR_TIME)

// Within one skipper step (1/256) of `hz`, the loop off its floor, nothing blocking with interrupts off.
void expectLanded(unsigned long hz, const char *how)
{
	const unsigned long cpu = tegra_dfll_landing_cpu_hz();
	EXPECT_LE(cpu, hz + hz / 256) << how << ": " << cpu / 1000 << " kHz";
	EXPECT_GE(cpu + hz / 256, hz) << how << ": " << cpu / 1000 << " kHz";
	EXPECT_FALSE(tegra_dfll_landing_pinned())
		<< how << ": the loop at OUTPUT_CFG MIN, DVCO minimum "
		<< tegra_dfll_landing_dvco_rate_min() / 1000 << " kHz";
	EXPECT_EQ(tegra_dfll_landing_irqs_off_blocking(), 0u) << how;
}

unsigned long askAndSettle(unsigned long hz)
{
	EXPECT_EQ(tegra_dfll_landing_request(hz), 0) << hz;
	tegra_dfll_landing_run_ms(kSettleMs);
	return tegra_dfll_landing_cpu_hz();
}
}  // namespace

TEST(TegraDfllLanding, AFloorBetweenRequestStepsLandsTheTopRow)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		ASSERT_EQ(tegra_dfll_landing_start(1797 * kMHz, kPerLutStep, 1785 * kMHz), 0);
		tegra_dfll_landing_run_ms(kSettleMs);
		expectLanded(1785 * kMHz, "1785 MHz at enable, DVCO floor 1797 MHz");
	});
}

TEST(TegraDfllLanding, AFloorOnARequestStepLandsTheTopRow)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		ASSERT_EQ(tegra_dfll_landing_start(1785 * kMHz, kPerLutStep, 1785 * kMHz), 0);
		tegra_dfll_landing_run_ms(kSettleMs);
		expectLanded(1785 * kMHz, "1785 MHz at enable, DVCO floor 1785 MHz");
	});
}

TEST(TegraDfllLanding, AFloorThatRisesIsFollowedAtTheNextRequest)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		ASSERT_EQ(tegra_dfll_landing_start(1797 * kMHz, kPerLutStep, 1785 * kMHz), 0);
		tegra_dfll_landing_run_ms(kSettleMs);
		tegra_dfll_landing_set_floor(1835 * kMHz);
		askAndSettle(1683 * kMHz);
		expectLanded(1683 * kMHz, "1683 MHz after the floor rose to 1835 MHz");
		askAndSettle(1785 * kMHz);
		expectLanded(1785 * kMHz, "1785 MHz after the floor rose to 1835 MHz");
	});
}

TEST(TegraDfllLanding, ARowLandsTheSameFromAboveAndFromBelow)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		ASSERT_EQ(tegra_dfll_landing_start(1797 * kMHz, kPerLutStep, 1785 * kMHz), 0);
		tegra_dfll_landing_run_ms(kSettleMs);
		const unsigned long fromAbove = askAndSettle(1683 * kMHz);
		expectLanded(1683 * kMHz, "1683 MHz from 1785");
		askAndSettle(1581 * kMHz);
		expectLanded(1581 * kMHz, "1581 MHz from 1683 (low tune)");
		const unsigned long fromBelow = askAndSettle(1683 * kMHz);
		expectLanded(1683 * kMHz, "1683 MHz from 1581 (low to high tune)");
		EXPECT_EQ(fromAbove, fromBelow);
		askAndSettle(918 * kMHz);
		const unsigned long topFromBelow = askAndSettle(1785 * kMHz);
		expectLanded(1785 * kMHz, "1785 MHz from 918");
		askAndSettle(1683 * kMHz);
		EXPECT_EQ(askAndSettle(1785 * kMHz), topFromBelow)
			<< "1785 MHz from 1683 and from 918";
	});
}

TEST(TegraDfllLanding, ALoopStillComingDownAtTheFirstLookIsLookedAtAgain)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		// One LUT step in 10 ms: still over MIN at the first look, 41 ms on.
		tegra_dfll_landing_set_slew(100);
		ASSERT_EQ(tegra_dfll_landing_start(1797 * kMHz, kPerLutStep, 1785 * kMHz), 0);
		tegra_dfll_landing_run_ms(1000);
		expectLanded(1785 * kMHz, "1785 MHz at enable, the loop coming down from 945 mV");
		askAndSettle(1683 * kMHz);
		tegra_dfll_landing_run_ms(800);
		expectLanded(1683 * kMHz, "1683 MHz from 1785, the loop coming down");
	});
}
