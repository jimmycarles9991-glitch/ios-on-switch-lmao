// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <cerrno>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
constexpr u32 GPCPLL_COEFF_R = 0x137004, GPCPLL_CFG_R = 0x137000, SEL_VCO_R = 0x137100,
	      GPC2CLK_OUT_R = 0x137250, nv_clk_src_gpc = 0;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))

namespace
{
struct Device {
	void *clk;
} device;

struct {
	Device *device;
} tgm_gpu;

struct {
	void *clk;
	u32 maximum_khz, idle_khz, run_khz, requested_khz, vco_khz, vco_coeff;
	u32 run_pl, idle_pl;
	int on, low, clock_available;
	u64 log_start, last_busy;
} tgm_idle;

u32 coefficient;
unsigned inside, locked, reads, reprograms, logs;
bool fail_switch, fail_reprogram;
std::vector<u32> switches;

u32 tegra_gm20b_dvfs_khz(u32 i)
{
	constexpr u32 rates[] = {76800,  153600, 230400, 307200, 384000, 460800,
				 537600, 614400, 691200, 768000, 844800, 921600};
	return i < sizeof(rates) / sizeof(rates[0]) ? rates[i] : 0;
}

void tgm_gate_enter()
{
	EXPECT_EQ(locked, 0u);
	++inside;
}

void gate_leave()
{
	EXPECT_EQ(locked, 0u);
	ASSERT_NE(inside, 0u);
	--inside;
}

void nvxnu_mutex_lock(void *)
{
	ASSERT_EQ(locked, 0u);
	++locked;
}

void nvxnu_mutex_unlock(void *)
{
	ASSERT_EQ(locked, 1u);
	--locked;
}

u32 nvkm_rd32(Device *, u32 address)
{
	EXPECT_EQ(locked, 1u);
	if (address != GPCPLL_COEFF_R)
		return 0;
	++reads;
	return coefficient;
}

int nvkm_clk_read(void *, u32)
{
	return int(38400 * ((coefficient >> 8) & 255) /
		   ((coefficient & 255) * 2 * ((coefficient >> 16) & 63)));
}

int clk_prog_khz(Device *, u32 khz)
{
	EXPECT_EQ(locked, 1u);
	++reprograms;
	if (fail_reprogram)
		return -EIO;
	coefficient = (coefficient & 0xffff) | ((1536000 / (2 * khz)) << 16);
	return 0;
}

int pl_switch(u32 pl)
{
	EXPECT_EQ(locked, 1u);
	switches.push_back(pl);
	if (fail_switch)
		return -EIO;
	coefficient = (coefficient & 0xffff) | (pl << 16);
	return 0;
}

u64 nvxnu_nanotime()
{
	return 1000;
}

void nvxnu_log(const char *, ...)
{
	++logs;
}
}

#include "../SwitchGpuClockControl.h"

namespace
{
class GpuClock : public testing::Test
{
	void SetUp() override
	{
		tgm_gpu.device = &device;
		tgm_idle = {&device, 768000, 76800, 768000, 0, 1536000, 0x2801,
			    1,       10,     1,     0,      1, 0,       0};
		coefficient = 0x12801;
		inside = locked = reads = reprograms = logs = 0;
		fail_switch = fail_reprogram = false;
		switches.clear();
	}

	void TearDown() override
	{
		EXPECT_EQ(inside, 0u);
		EXPECT_EQ(locked, 0u);
	}
};

TEST_F(GpuClock, CapsOnlyAtQualifiedRatesReachableFromTheExistingVco)
{
	u32 applied = 0;
	ASSERT_EQ(switch_gpu_clock_ceiling(600000, &applied), 0);
	EXPECT_EQ(applied, 384000u);
	EXPECT_EQ(tgm_idle.run_pl, 2u);
	EXPECT_EQ(coefficient & 0xffff, 0x2801u);
	ASSERT_EQ(switch_gpu_clock_ceiling(307200, &applied), 0);
	EXPECT_EQ(applied, 153600u);
	EXPECT_EQ(tgm_idle.run_pl, 5u);
}

TEST_F(GpuClock, ZeroRestoresTheBootMaximumWithoutChangingVco)
{
	u32 applied = 0;
	ASSERT_EQ(switch_gpu_clock_ceiling(400000, &applied), 0);
	ASSERT_EQ(switch_gpu_clock_ceiling(0, &applied), 0);
	EXPECT_EQ(applied, 768000u);
	EXPECT_EQ(tgm_idle.run_pl, 1u);
	EXPECT_EQ(coefficient & 0xffff, 0x2801u);
}

TEST_F(GpuClock, AboveBootRequestNeverRaisesBeyondQualifiedVoltage)
{
	u32 applied = 0;
	EXPECT_EQ(switch_gpu_clock_ceiling(UINT32_MAX, &applied), 0);
	EXPECT_EQ(applied, 768000u);
}

TEST_F(GpuClock, UnreachableLowRequestReturnsTheUnchangedActualCeiling)
{
	u32 applied = 0;
	EXPECT_EQ(switch_gpu_clock_ceiling(76800 - 1, &applied), -ERANGE);
	EXPECT_EQ(applied, 768000u);
	EXPECT_TRUE(switches.empty());
}

TEST_F(GpuClock, CapWhileIdleDoesNotRaiseIdleFrequency)
{
	tgm_idle.low = 1;
	coefficient = 0xa2801;
	u32 applied = 0;
	ASSERT_EQ(switch_gpu_clock_ceiling(400000, &applied), 0);
	EXPECT_EQ(applied, 384000u);
	EXPECT_EQ(tgm_idle.run_pl, 2u);
	EXPECT_EQ(coefficient >> 16, 10u);
}

TEST_F(GpuClock, SuspendRestoreReappliesTheCapBeforeEngineResume)
{
	u32 applied = 0;
	ASSERT_EQ(switch_gpu_clock_ceiling(200000, &applied), 0);
	coefficient = 0x12801;
	tgm_idle.on = 0;
	ASSERT_EQ(tgm_clock_ceiling_restore(), 0);
	EXPECT_EQ(coefficient >> 16, 5u);
	EXPECT_EQ(nvkm_clk_read(nullptr, 0), 153600);
}

TEST_F(GpuClock, RestoreRejectsADifferentPllBasis)
{
	coefficient = 0x13001;
	EXPECT_EQ(tgm_clock_ceiling_restore(), -EIO);
	EXPECT_TRUE(switches.empty());
}

TEST_F(GpuClock, FailedHardwareChangeFailsThatRequestAndTheNextOneTriesAgain)
{
	fail_switch = true;
	u32 applied = 0;
	EXPECT_EQ(switch_gpu_clock_ceiling(400000, &applied), -EIO);
	EXPECT_EQ(tgm_idle.on, 1);
	EXPECT_EQ(tgm_idle.clock_available, 1);
	EXPECT_EQ(tgm_idle.run_pl, 1u);
	EXPECT_EQ(logs, 1u);
	EXPECT_EQ(reprograms, 1u);
	EXPECT_EQ(coefficient >> 16, 1u);
	fail_switch = false;
	EXPECT_EQ(switch_gpu_clock_ceiling(200000, &applied), 0);
	EXPECT_EQ(applied, 153600u);
	EXPECT_EQ(coefficient >> 16, 5u);
}

TEST_F(GpuClock, ADifferentVcoRefusesTheRequestAndKeepsTheIdleClock)
{
	coefficient = 0x13001;
	u32 applied = 0;
	EXPECT_EQ(switch_gpu_clock_ceiling(400000, &applied), -EIO);
	EXPECT_TRUE(switches.empty());
	EXPECT_EQ(tgm_idle.on, 1);
	EXPECT_EQ(tgm_idle.clock_available, 1);
}

// The owner's rule: a divider change that does not take is our defect to find, not a state to wait out. Each one logs the
// PLL's registers, and nothing is held back: the next idle period's drop takes the normal path.
TEST_F(GpuClock, EachFailedChangeLogsAndHoldsNothingBack)
{
	locked = 1;
	fail_switch = true;
	for (u32 i = 1; i <= 9; ++i)
		idle_fail("drop", 10);
	EXPECT_EQ(logs, 9u);
	EXPECT_EQ(tgm_idle.on, 1);
	EXPECT_EQ(tgm_idle.clock_available, 1);
	EXPECT_EQ(tgm_idle.low, 0);
	locked = 0;
}

TEST_F(GpuClock, ARunRateThatCouldNotBePutBackLeavesTheNextRaiseToTryAgain)
{
	locked = 1;
	fail_switch = fail_reprogram = true;
	tgm_idle.low = 0;
	idle_fail("raise", 1);
	EXPECT_EQ(tgm_idle.low, 1);
	fail_switch = false;
	idle_fail("drop", 10);
	EXPECT_EQ(tgm_idle.low, 0);
	locked = 0;
}

TEST_F(GpuClock, AWakeOffTheRunPlPutsItBackAndKeepsTheIdleClock)
{
	tgm_idle.on = 0;
	coefficient = 0x32801;
	EXPECT_STREQ(tgm_idle_wake(), "on, its run rate put back");
	EXPECT_EQ(coefficient >> 16, 1u);
	EXPECT_EQ(tgm_idle.on, 1);
	EXPECT_EQ(tgm_idle.low, 0);
	EXPECT_EQ(tgm_idle.last_busy, 1000u);
}

TEST_F(GpuClock, AWakeOnAnotherVcoReprogramsTheRunRate)
{
	tgm_idle.on = 0;
	coefficient = 0x33001;
	EXPECT_STREQ(tgm_idle_wake(), "on, its run rate put back");
	EXPECT_EQ(reprograms, 1u);
	EXPECT_TRUE(switches.empty());
	EXPECT_EQ(tgm_idle.on, 1);
}

TEST_F(GpuClock, AWakeWhoseRunRateCannotBePutBackLeavesTheFirstRaiseToTryAgain)
{
	tgm_idle.on = 0;
	coefficient = 0x32801;
	fail_switch = fail_reprogram = true;
	EXPECT_STREQ(tgm_idle_wake(),
		     "on, its run rate not put back (the first raise tries again)");
	EXPECT_EQ(tgm_idle.on, 1);
	EXPECT_EQ(tgm_idle.low, 1);
}

TEST_F(GpuClock, AWakeAtTheRunPlTouchesNothing)
{
	tgm_idle.on = 0;
	EXPECT_STREQ(tgm_idle_wake(), "on");
	EXPECT_TRUE(switches.empty());
	EXPECT_EQ(logs, 0u);
	EXPECT_EQ(tgm_idle.on, 1);
}

TEST_F(GpuClock, StateQueryDoesNotWakeTheGpuOrTouchRegisters)
{
	tgm_idle.on = 0;
	switch_gpu_clock_state state{};
	ASSERT_EQ(switch_gpu_clock_get(&state), 0);
	EXPECT_EQ(state.maximum_khz, 768000u);
	EXPECT_EQ(state.minimum_khz, 76800u);
	EXPECT_EQ(state.ceiling_khz, 768000u);
	EXPECT_EQ(reads, 0u);
	EXPECT_TRUE(switches.empty());
}

TEST_F(GpuClock, RejectsMissingStorageAndUnavailableControl)
{
	EXPECT_EQ(switch_gpu_clock_get(nullptr), -EINVAL);
	EXPECT_EQ(switch_gpu_clock_ceiling(1, nullptr), -EINVAL);
	tgm_idle.clk = nullptr;
	u32 applied = 5;
	switch_gpu_clock_state state{};
	EXPECT_EQ(switch_gpu_clock_get(&state), -ENODEV);
	EXPECT_EQ(switch_gpu_clock_ceiling(1, &applied), -ENODEV);
	EXPECT_EQ(applied, 0u);
}
}
