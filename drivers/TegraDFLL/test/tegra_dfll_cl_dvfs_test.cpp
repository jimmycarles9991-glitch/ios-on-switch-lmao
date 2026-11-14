// SPDX-License-Identifier: GPL-2.0
// The CL-DVFS module's clock, reset and sources as tegra_dfll_cpu_prepare() and tegra_dfll_cpu_resume() program them,
// against the TRM (Tegra X1 TRM DP-07225-001 v1.3p): CL-DVFS is never out of reset with its clock stopped, or I2C5
// hangs on its bus grant (35.4); a device in reset gets its clock, then its divisor and source 2 us apart, then leaves
// reset (5.1.7); a running one changes divisor before source (5.1.7 Method 2); CLK_SOURCE_DVFS_* keep reserved bit 28
// at its reset value 1 (1.4, 5.2.245-246); clock and reset bits change through the CAR's _SET/_CLR registers.
#include "drivers/TegraDFLL/TegraDFLL.h"
#include "drivers/TegraDFLL/test/tegra_dfll_fake.h"

#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
constexpr uint32_t kClkEnbWSet = 0x448, kRstDevWClr = 0x43c, kSourceRef = 0x62c, kSourceSoc = 0x630;
constexpr uint32_t kDvfs = 1u << 27;
constexpr uint32_t kClkMDivided = 0xd000000eu;  // clk_m, bit 28 as at reset, 7.1 divisor 14
constexpr uint32_t kPllp51MHz = 0x1000000eu;    // pll_p, bit 28 as at reset, 7.1 divisor 14

struct Write {
	uint32_t offset, value;
};

std::vector<Write> carLog()
{
	std::vector<Write> log;
	for (unsigned i = 0; i < tegra_dfll_fake_car_writes; ++i)
		log.push_back(
			{tegra_dfll_fake_car_log[i].offset, tegra_dfll_fake_car_log[i].value});
	return log;
}

// Index of the first write of `value` to `offset` at or after `from`; the log's size if none.
size_t find(const std::vector<Write> &log, uint32_t offset, uint32_t value, size_t from = 0)
{
	for (size_t i = from; i < log.size(); ++i)
		if (log[i].offset == offset && log[i].value == value)
			return i;
	return log.size();
}

// The microseconds of delay logged between two writes.
uint32_t delayBetween(const std::vector<Write> &log, size_t from, size_t to)
{
	uint32_t us = 0;
	for (size_t i = from + 1; i < to && i < log.size(); ++i)
		if (log[i].offset == TEGRA_DFLL_FAKE_DELAY)
			us += log[i].value;
	return us;
}

int prepare()
{
	char why[160] = "";
	return tegra_dfll_cpu_prepare(why, sizeof(why));
}

uint32_t car(uint32_t offset)
{
	return *tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, offset);
}

// Each source moved divisor first, then source, 2 us after each; returns the index of the last of those writes.
size_t expectDivisorThenSource(const std::vector<Write> &log, uint32_t source, size_t from)
{
	const size_t divisor = find(log, source, kClkMDivided, from);
	const size_t moved = find(log, source, kPllp51MHz, from);
	EXPECT_LT(divisor, moved) << std::hex << source;
	EXPECT_LT(moved, log.size()) << std::hex << source;
	EXPECT_GE(delayBetween(log, divisor, moved), 2u) << std::hex << source;
	return moved;
}

// Brought up from reset: clock on, both sources, then out of reset; never unsafe, everything through _SET/_CLR.
void expectBroughtUpFromReset(const std::vector<Write> &log)
{
	const size_t clock = find(log, kClkEnbWSet, kDvfs);
	const size_t soc = expectDivisorThenSource(log, kSourceSoc, clock);
	const size_t ref = expectDivisorThenSource(log, kSourceRef, clock);
	const size_t release = find(log, kRstDevWClr, kDvfs);
	EXPECT_LT(clock, find(log, kSourceSoc, kClkMDivided));
	EXPECT_LT(soc, release);
	EXPECT_LT(ref, release);
	EXPECT_LT(release, log.size());
	EXPECT_GE(delayBetween(log, soc > ref ? soc : ref, release), 2u);
	EXPECT_EQ(car(kSourceSoc), kPllp51MHz);
	EXPECT_EQ(car(kSourceRef), kPllp51MHz);
	EXPECT_EQ(car(0x364) & kDvfs, kDvfs);
	EXPECT_EQ(car(0x35c) & kDvfs, 0u);
}
}  // namespace

TEST(TegraDfllClDvfs, FromResetItsSourcesAreSetBeforeItLeavesReset)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		tegra_dfll_fake_cl_dvfs_reset();
		ASSERT_EQ(prepare(), 0);
		expectBroughtUpFromReset(carLog());
		EXPECT_EQ(tegra_dfll_fake.cl_dvfs_unsafe, 0u);
		EXPECT_EQ(tegra_dfll_fake.car_direct_writes, 0u);
		EXPECT_EQ(tegra_dfll_fake.i2c5_hangs, 0u);
	});
}

TEST(TegraDfllClDvfs, LeftRunningByTheBootChainItsDivisorChangesBeforeItsSource)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		ASSERT_EQ(prepare(), 0);
		const auto log = carLog();
		const size_t clock = find(log, kClkEnbWSet, kDvfs);
		EXPECT_LT(clock, find(log, kSourceSoc, kClkMDivided));
		expectDivisorThenSource(log, kSourceSoc, clock);
		expectDivisorThenSource(log, kSourceRef, clock);
		EXPECT_EQ(find(log, kRstDevWClr, kDvfs), log.size());
		EXPECT_EQ(car(kSourceSoc), kPllp51MHz);
		EXPECT_EQ(car(kSourceRef), kPllp51MHz);
		EXPECT_EQ(tegra_dfll_fake.cl_dvfs_unsafe, 0u);
		EXPECT_EQ(tegra_dfll_fake.car_direct_writes, 0u);
	});
}

TEST(TegraDfllClDvfs, AfterSc7ResumeBringsItUpAsFromReset)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		ASSERT_EQ(prepare(), 0);
		tegra_dfll_fake_cl_dvfs_reset();
		tegra_dfll_fake_car_writes = 0;
		char why[160] = "";
		ASSERT_EQ(tegra_dfll_cpu_resume(why, sizeof(why)), 0) << why;
		expectBroughtUpFromReset(carLog());
		EXPECT_EQ(tegra_dfll_fake.cl_dvfs_unsafe, 0u);
		EXPECT_EQ(tegra_dfll_fake.car_direct_writes, 0u);
	});
}

// Out of reset with its clock stopped (no boot chain state seen, but I2C5 would hang in it): the clock goes on before
// prepare's first I2C5 transfer.
TEST(TegraDfllClDvfs, ItsClockGoesOnBeforePreparesFirstI2C5Transfer)
{
	switch_ios::test::ExpectPassesInFreshProcess([] {
		tegra_dfll_fake_console();
		*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x364) = 0;
		EXPECT_EQ(prepare(), 0);
		EXPECT_EQ(tegra_dfll_fake.i2c5_hangs, 0u);
		EXPECT_EQ(car(0x364) & kDvfs, kDvfs);
	});
}
