// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <cerrno>
#include <vector>
using u32 = uint32_t;
constexpr u32 GPCPLL_COEFF_R = 0x137004, nv_clk_src_gpc = 0;
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
} tgm_idle;

u32 coefficient;
unsigned inside, locked, reads;
bool fail_switch;
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
	EXPECT_EQ(address, GPCPLL_COEFF_R);
	++reads;
	return coefficient;
}

int nvkm_clk_read(void *, u32)
{
	return int(38400 * ((coefficient >> 8) & 255) /
		   ((coefficient & 255) * 2 * ((coefficient >> 16) & 63)));
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

void nvxnu_log(const char *, ...)
{
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
		tgm_idle = {&device, 768000, 76800, 768000, 0, 1536000, 0x2801, 1, 10, 1, 0, 1};
		coefficient = 0x12801;
		inside = locked = reads = 0;
		fail_switch = false;
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

TEST_F(GpuClock, FailedHardwareChangeDisablesFurtherBoostsAndReportsActual)
{
	fail_switch = true;
	u32 applied = 0;
	EXPECT_EQ(switch_gpu_clock_ceiling(400000, &applied), -EIO);
	EXPECT_EQ(applied, 768000u);
	EXPECT_EQ(tgm_idle.on, 0);
	EXPECT_EQ(tgm_idle.clock_available, 0);
	EXPECT_EQ(switch_gpu_clock_ceiling(200000, &applied), -ENODEV);
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
