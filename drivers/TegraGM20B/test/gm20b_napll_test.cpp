// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "drivers/TegraGM20B/TegraGM20BNapll.h"
}

namespace
{
struct B01CvbRow {
	uint32_t khz;
	int c0, c1, c2, c3, c4, c5;
};

constexpr B01CvbRow kB01Cvb[] = {
	{76800, 610000, 0, 0, 0, 0, 0},
	{153600, 610000, 0, 0, 0, 0, 0},
	{230400, 610000, 0, 0, 0, 0, 0},
	{307200, 610000, 0, 0, 0, 0, 0},
	{384000, 610000, 0, 0, 0, 0, 0},
	{460800, 610000, 0, 0, 0, 0, 0},
	{537600, 801688, -10900, -163, 298, -10599, 162},
	{614400, 824214, -5743, -452, 238, -6325, 81},
	{691200, 848830, -3903, -552, 119, -4030, -2},
	{768000, 891575, -4409, -584, 0, -2849, 39},
	{844800, 940071, -5367, -602, -60, -63, -93},
	{921600, 986765, -6637, -614, -179, 1905, -13},
	{998400, 1098475, -13529, -497, -179, 3626, 9},
	{1075200, 1163644, -12688, -648, 0, 1077, 40},
	{1152000, 1204812, -9908, -830, 0, 1469, 110},
	{1228800, 1277303, -11675, -859, 0, 3722, 313},
	{1267200, 1335531, -12567, -867, 0, 3681, 559},
};

constexpr int kTripsC[] = {-25, 20, 30, 50, 70, 90};

int divClosest(int x, int d)
{
	return x > 0 ? (x + d / 2) / d : (x - d / 2) / d;
}

uint32_t b01RowKhz(int row)
{
	return row >= 0 && row < static_cast<int>(sizeof(kB01Cvb) / sizeof(kB01Cvb[0]))
		       ? kB01Cvb[row].khz
		       : 0u;
}

int b01RowNaUv(uint32_t khz, int speedo)
{
	if (speedo <= 0)
		return -22;
	for (const auto &row : kB01Cvb) {
		if (row.khz != khz)
			continue;
		int worst = 610000;
		for (int t : kTripsC) {
			int uv = divClosest(row.c2 * speedo, 100);
			uv = divClosest((uv + row.c1) * speedo, 100) + row.c0;
			int tv = divClosest(row.c3 * speedo, 100) + row.c4 +
				 divClosest(row.c5 * t, 10);
			uv += divClosest(tv * t, 10);
			uv = uv > 0 ? (uv + 4999) / 5000 * 5000 : 0;
			if (uv > worst)
				worst = uv;
		}
		return worst;
	}
	return -22;
}

struct WriteEvent {
	uint32_t reg;
	uint32_t val;
};

struct MockPll {
	std::map<uint32_t, uint32_t> regs;
	std::vector<WriteEvent> writes;
	std::vector<uint32_t> delays;
	uint32_t chipset = TGM_CHIPSET_GM20B_C1;
	uint32_t testout = 18u;
	bool fail_cal = false;
	bool fail_ramp = false;
	bool saw_c1_enable_for_cal = false;
	bool saw_iddq_on_en_dfs = false;
	bool saw_strobe_around_dvfs0 = false;
	bool strobe_high = false;

	MockPll()
	{
		regs[TGM_THERM_USE_A_R] = 0x7u;
		regs[TGM_GPCPLL_CFG_R] = TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_PLL_LOCK;
		regs[TGM_GPCPLL_COEFF_R] = 1u | (34u << 8) | (17u << 16);
		regs[TGM_SEL_VCO_R] = TGM_SEL_VCO_GPC2CLK_OUT;
		regs[TGM_GPC2CLK_OUT_R] = 0x80000000u;
	}

	uint32_t rd32(uint32_t reg)
	{
		if (reg == TGM_GPCPLL_DVFS1_R && !fail_cal &&
		    (regs[reg] & TGM_GPCPLL_DVFS1_EN_DFS_CAL)) {
			const bool c1_ready = chipset != TGM_CHIPSET_GM20B_C1 ||
					      (regs[TGM_GPCPLL_CFG_R] & TGM_GPCPLL_CFG_ENABLE);
			if (c1_ready) {
				if (chipset == TGM_CHIPSET_GM20B_C1)
					saw_c1_enable_for_cal = true;
				regs[TGM_GPCPLL_CFG3_R] =
					(regs[TGM_GPCPLL_CFG3_R] &
					 ~TGM_GPCPLL_CFG3_DFS_TESTOUT_MASK) |
					(testout << TGM_GPCPLL_CFG3_DFS_TESTOUT_SHIFT);
				return regs[reg] | TGM_GPCPLL_DVFS1_DFS_CAL_DONE;
			}
		}
		if (reg == TGM_GPC_BCAST_NDIV_DEBUG_R) {
			if (!fail_ramp &&
			    (regs[TGM_GPCPLL_NDIV_SLOWDOWN_R] & TGM_GPCPLL_SLOWDOWN_EN_DYNRAMP))
				return TGM_GPC_BCAST_DYNRAMP_DONE;
			return 0u;
		}
		return regs[reg];
	}

	void wr32(uint32_t reg, uint32_t val)
	{
		if (reg == TGM_GPCPLL_DVFS1_R && (val & TGM_GPCPLL_DVFS1_EN_DFS) &&
		    !(regs[reg] & TGM_GPCPLL_DVFS1_EN_DFS))
			saw_iddq_on_en_dfs = (regs[TGM_GPCPLL_CFG_R] & TGM_GPCPLL_CFG_IDDQ) != 0u;
		if (reg == TGM_GPC_BCAST_DVFS2_R)
			strobe_high = (val & TGM_GPC_BCAST_DVFS2_EXT_STROBE) != 0u;
		if (reg == TGM_GPCPLL_DVFS0_R && strobe_high)
			saw_strobe_around_dvfs0 = true;
		regs[reg] = val;
		writes.push_back({reg, val});
	}
};

uint32_t mockRd(void *arg, uint32_t reg)
{
	return static_cast<MockPll *>(arg)->rd32(reg);
}

void mockWr(void *arg, uint32_t reg, uint32_t val)
{
	static_cast<MockPll *>(arg)->wr32(reg, val);
}

void mockDelay(void *arg, uint32_t us)
{
	static_cast<MockPll *>(arg)->delays.push_back(us);
}

uint32_t encodeCalibFuse(uint32_t rev, uint32_t slope_mv, uint32_t slope_uv_frac, uint32_t offs_mv,
			 uint32_t offs_100uv)
{
	const uint32_t hi = ((rev & 0x3u) << 30) | ((slope_mv & 0x3fu) << 24);
	const uint32_t mid = (slope_uv_frac & 0x3ffu) << 14;
	const uint32_t lo = ((offs_mv & 0x3ffu) << 4) | (offs_100uv & 0xfu);
	return hi | mid | lo;
}

std::string readText(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

TEST(Gm20bNapll, ChipsetTablesMatchL4tB1AndC1)
{
	tgm_napll_params c1{};
	ASSERT_EQ(tgm_napll_params_for_chipset(TGM_CHIPSET_GM20B_C1, &c1), 0);
	EXPECT_EQ(c1.coeff_slope, -172550);
	EXPECT_EQ(c1.coeff_offs, 195374);
	EXPECT_EQ(c1.vco_ctrl, 0x0fu);
	EXPECT_EQ(c1.dfs_ctrl, 0xc00u);

	tgm_napll_params b1{};
	ASSERT_EQ(tgm_napll_params_for_chipset(TGM_CHIPSET_GM20B_B1, &b1), 0);
	EXPECT_EQ(b1.coeff_slope, -165230);
	EXPECT_EQ(b1.coeff_offs, 214007);
	EXPECT_EQ(b1.vco_ctrl, 0x38u);
	EXPECT_EQ(b1.dfs_ctrl, 0u);

	tgm_napll_params bad{};
	EXPECT_EQ(tgm_napll_params_for_chipset(0x130u, &bad), TGM_NAPLL_ERR_NODEV);
}

TEST(Gm20bNapll, CalibFuseDecodeAndUnfusedRejection)
{
	int32_t slope = 0;
	int32_t offs = 0;
	const uint32_t fuse = encodeCalibFuse(1u, 10u, 250u, 600u, 5u);
	ASSERT_EQ(tgm_napll_decode_calib_fuse(fuse, &slope, &offs), 0);
	EXPECT_EQ(slope, 10250);
	EXPECT_EQ(offs, 600500);

	const uint32_t rev0 = encodeCalibFuse(0u, 10u, 0u, 600u, 0u);
	const uint32_t zero_slope = encodeCalibFuse(1u, 0u, 0u, 600u, 0u);
	EXPECT_EQ(tgm_napll_decode_calib_fuse(rev0, &slope, &offs), TGM_NAPLL_ERR_INVAL);
	EXPECT_EQ(tgm_napll_decode_calib_fuse(zero_slope, &slope, &offs), TGM_NAPLL_ERR_INVAL);
}

TEST(Gm20bNapll, DetectionAndNdivMathMatchL4tAt800mVAnd921_6MHz)
{
	tgm_napll_params c1{};
	ASSERT_EQ(tgm_napll_params_for_chipset(TGM_CHIPSET_GM20B_C1, &c1), 0);

	tgm_napll_target t921{};
	ASSERT_EQ(tgm_napll_calc_mnp(&c1, 38400u, 921600u, &t921), 0);
	EXPECT_EQ(t921.m, 1u);
	EXPECT_EQ(t921.n, 48u);
	EXPECT_EQ(t921.pl, 1u);
	EXPECT_EQ(t921.vco_khz, 1843200u);
	EXPECT_EQ(t921.n_lo, 34u);

	tgm_napll_dvfs dvfs{};
	ASSERT_EQ(tgm_napll_calc_detection(&c1, 10000, 620000, 800, &dvfs), 0);
	EXPECT_EQ(dvfs.dfs_coeff, 57u);
	EXPECT_EQ(dvfs.dfs_ext_cal, 18);
	EXPECT_EQ(dvfs.dfs_det_max, 0u);
	EXPECT_EQ(dvfs.uv_cal, 800000);

	ASSERT_EQ(tgm_napll_calc_ndiv(&c1, 10000, 620000, 800, 48u, &dvfs), 0);
	EXPECT_EQ(dvfs.n_int, 48u);
	EXPECT_EQ(dvfs.sdm_din, 0xf0u);

	tgm_napll_dvfs droop = dvfs;
	ASSERT_EQ(tgm_napll_calc_ndiv(&c1, 10000, 620000, 790, 48u, &droop), 0);
	EXPECT_EQ(droop.n_int, 48u);
	EXPECT_NE(droop.sdm_din, 0xf0u);

	EXPECT_EQ(tgm_napll_calc_detection(&c1, 1000, 600000, 800, &dvfs), TGM_NAPLL_ERR_RANGE);
}

TEST(Gm20bNapll, CvbVoltageSelectionAdmits921_6MHzUnder800mVCeiling)
{
	uint32_t best = 0;
	EXPECT_EQ(tgm_napll_select_rate_khz(921600u, 800000, 0, b01RowKhz, b01RowNaUv, 1673, &best),
		  921600u);
	EXPECT_GE(best, 921600u);

	const uint32_t fixed_sel =
		tgm_napll_select_rate_khz(768000u, 800000, 20, b01RowKhz, b01RowNaUv, 1673, &best);
	EXPECT_EQ(fixed_sel, 768000u);
	EXPECT_EQ(best, 768000u);

	EXPECT_EQ(tgm_napll_select_rate_khz(921600u, 800000, 0, b01RowKhz, b01RowNaUv, 1400, &best),
		  921600u);

	EXPECT_EQ(tgm_napll_select_rate_khz(921600u, 690000, 0, b01RowKhz, b01RowNaUv, 1673, &best),
		  844800u);
	EXPECT_EQ(best, 844800u);

	EXPECT_EQ(tgm_napll_select_rate_khz(921600u, 800000, 0, b01RowKhz, b01RowNaUv, 0, &best),
		  0u);
}

TEST(Gm20bNapll, StartProgramsFusedC1To921_6MHzAndSuspendsCleanly)
{
	MockPll hw;
	const tgm_napll_io io = {&hw, mockRd, mockWr, mockDelay};
	tgm_napll_state state{};
	const uint32_t fuse = encodeCalibFuse(1u, 10u, 0u, 620u, 0u);

	ASSERT_EQ(tgm_napll_start(&io, TGM_CHIPSET_GM20B_C1, fuse, 800000, 38400u, 921600u, &state),
		  0);
	EXPECT_EQ(state.active, 1);
	EXPECT_EQ(state.fused, 1);
	EXPECT_EQ(state.khz, 921600u);
	EXPECT_TRUE(hw.saw_iddq_on_en_dfs);
	EXPECT_TRUE(hw.saw_strobe_around_dvfs0);

	EXPECT_EQ(hw.regs[TGM_GPCPLL_COEFF_R], 1u | (48u << 8) | (1u << 16));
	EXPECT_EQ(hw.regs[TGM_GPCPLL_CFG2_R] & TGM_GPCPLL_CFG2_SDM_DIN_MASK, 0xf0u);
	EXPECT_EQ((hw.regs[TGM_GPCPLL_CFG2_R] & TGM_GPCPLL_CFG2_PLL_STEPA_MASK) >> 24,
		  TGM_NAPLL_STEPA_38400);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_CFG3_R] & TGM_GPCPLL_CFG3_VCO_CTRL_MASK, 0x0fu);
	EXPECT_EQ((hw.regs[TGM_GPCPLL_CFG3_R] & TGM_GPCPLL_CFG3_PLL_STEPB_MASK) >> 16,
		  TGM_NAPLL_STEPB_38400);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_DVFS0_R] & TGM_GPCPLL_DVFS0_DFS_COEFF_MASK, 57u);
	EXPECT_EQ((hw.regs[TGM_GPCPLL_DVFS0_R] & TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK) >> 8, 0u);
	EXPECT_EQ(hw.regs[TGM_GPC_BCAST_DVFS2_R] & TGM_GPC_BCAST_DVFS2_EXT_DET_MASK, 18u);
	EXPECT_EQ(hw.regs[TGM_GPC_BCAST_DVFS2_R] & TGM_GPC_BCAST_DVFS2_EXT_STROBE, 0u);

	const uint32_t dvfs1 = hw.regs[TGM_GPCPLL_DVFS1_R];
	EXPECT_NE(dvfs1 & TGM_GPCPLL_DVFS1_EN_DFS, 0u);
	EXPECT_NE(dvfs1 & TGM_GPCPLL_DVFS1_EN_DFS_CAL, 0u);
	const uint32_t dfs_ctrl =
		(dvfs1 & TGM_GPCPLL_DVFS1_DFS_CTRL_MASK) >> TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT;
	const uint32_t expected_ctrl =
		TGM_DFS_CTRL_C1_INIT | TGM_DFS_CTRL_EXT_CAL_EN | TGM_DFS_CTRL_TESTOUT_DET;
	EXPECT_EQ(dfs_ctrl, expected_ctrl);

	EXPECT_EQ(tgm_napll_cfg_running(hw.regs[TGM_GPCPLL_CFG_R], dvfs1), 1);
	EXPECT_NE(hw.regs[TGM_GPCPLL_CFG_R] & TGM_GPCPLL_CFG_ENB_LCKDET_OFF, 0u);
	EXPECT_EQ(TGM_NAPLL_RAMP_TIMEOUT_US, 500u);
	EXPECT_EQ(hw.regs[TGM_SEL_VCO_R] & TGM_SEL_VCO_GPC2CLK_OUT, TGM_SEL_VCO_GPC2CLK_OUT);
	EXPECT_EQ(hw.regs[TGM_GPC2CLK_OUT_R] & TGM_GPC2CLK_OUT_VCODIV_MASK, 0u);
	EXPECT_EQ(hw.regs[TGM_THERM_USE_A_R], 0x7u);

	bool saw_lock_delay = false;
	for (uint32_t us : hw.delays)
		saw_lock_delay = saw_lock_delay || us == TGM_NAPLL_LOCK_DELAY_US;
	EXPECT_TRUE(saw_lock_delay);

	tgm_napll_suspend(&io, &state);
	EXPECT_EQ(state.active, 0);
	EXPECT_EQ(hw.regs[TGM_SEL_VCO_R] & TGM_SEL_VCO_GPC2CLK_OUT, 0u);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_CFG_R] & (TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_SYNC_MODE),
		  0u);
	EXPECT_EQ((hw.regs[TGM_GPCPLL_COEFF_R] & TGM_GPCPLL_COEFF_NDIV_MASK) >> 8, 34u);
}

TEST(Gm20bNapll, UnfusedC1EnablesPllDuringSelfCalibration)
{
	MockPll hw;
	hw.testout = 16u;
	const tgm_napll_io io = {&hw, mockRd, mockWr, mockDelay};
	tgm_napll_state state{};

	ASSERT_EQ(tgm_napll_start(&io, TGM_CHIPSET_GM20B_C1, 0u, 800000, 38400u, 921600u, &state),
		  0);
	EXPECT_TRUE(hw.saw_c1_enable_for_cal);
	EXPECT_EQ(state.fused, 0);
	EXPECT_EQ(state.uvdet_slope, TGM_NAPLL_ADC_SLOPE_UV);
	EXPECT_EQ(state.uvdet_offs, 640000);
	EXPECT_EQ(state.dvfs.dfs_ext_cal, 16);
}

TEST(Gm20bNapll, CalibrationOrRampFailureCleansDvfsAndStaysInactive)
{
	MockPll hw;
	hw.fail_cal = true;
	const tgm_napll_io io = {&hw, mockRd, mockWr, mockDelay};
	tgm_napll_state state{};

	EXPECT_EQ(tgm_napll_start(&io, TGM_CHIPSET_GM20B_C1, 0u, 800000, 38400u, 921600u, &state),
		  TGM_NAPLL_ERR_TIMEDOUT);
	EXPECT_EQ(state.active, 0);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_DVFS1_R] & TGM_GPCPLL_DVFS1_EN_DFS, 0u);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_CFG_R] & TGM_GPCPLL_CFG_IDDQ, 0u);

	MockPll hw2;
	hw2.fail_ramp = true;
	const tgm_napll_io io2 = {&hw2, mockRd, mockWr, mockDelay};
	const uint32_t fuse = encodeCalibFuse(1u, 10u, 0u, 620u, 0u);
	const int ramp_ret =
		tgm_napll_start(&io2, TGM_CHIPSET_GM20B_C1, fuse, 800000, 38400u, 921600u, &state);
	EXPECT_EQ(ramp_ret, TGM_NAPLL_ERR_TIMEDOUT);
	EXPECT_EQ(state.active, 0);
	EXPECT_EQ(state.khz, 0u);
	EXPECT_EQ(hw2.regs[TGM_GPCPLL_DVFS1_R] &
			  (TGM_GPCPLL_DVFS1_EN_DFS | TGM_GPCPLL_DVFS1_EN_DFS_CAL |
			   TGM_GPCPLL_DVFS1_DFS_CTRL_MASK),
		  0u);
	EXPECT_EQ(hw2.regs[TGM_GPCPLL_CFG3_R] & TGM_GPCPLL_CFG3_VCO_CTRL_MASK, 0u);
	EXPECT_EQ(hw2.regs[TGM_GPCPLL_DVFS0_R] &
			  (TGM_GPCPLL_DVFS0_DFS_COEFF_MASK | TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK),
		  0u);
	EXPECT_EQ(hw2.regs[TGM_GPC_BCAST_DVFS2_R] &
			  (TGM_GPC_BCAST_DVFS2_EXT_DET_MASK | TGM_GPC_BCAST_DVFS2_EXT_STROBE),
		  0u);
	EXPECT_EQ(hw2.regs[TGM_GPCPLL_CFG2_R] &
			  (TGM_GPCPLL_CFG2_SDM_DIN_MASK | TGM_GPCPLL_CFG2_SDM_DIN_NEW_MASK),
		  0u);
	EXPECT_EQ(hw2.regs[TGM_GPC2CLK_OUT_R] & TGM_GPC2CLK_OUT_VCODIV_MASK, 0u);

	MockPll hw_b1;
	hw_b1.chipset = TGM_CHIPSET_GM20B_B1;
	hw_b1.regs[TGM_GPCPLL_DVFS1_R] = TGM_DFS_CTRL_C1_INIT << TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT;
	const tgm_napll_io io_b1 = {&hw_b1, mockRd, mockWr, mockDelay};
	tgm_napll_params b1{};
	ASSERT_EQ(tgm_napll_params_for_chipset(TGM_CHIPSET_GM20B_B1, &b1), 0);
	int32_t slope = 10000;
	int32_t offs = 620000;
	ASSERT_EQ(tgm_napll_enable_dvfs(&io_b1, &b1, 800000, &slope, &offs), 0);
	EXPECT_EQ(hw_b1.regs[TGM_GPCPLL_DVFS1_R] & TGM_GPCPLL_DVFS1_DFS_CTRL_MASK, 0u);
}

TEST(Gm20bNapll, ReprogramPreservesVcoCoeffAndClearsRateOnPartialFailure)
{
	MockPll hw;
	const tgm_napll_io io = {&hw, mockRd, mockWr, mockDelay};
	tgm_napll_state state{};
	const uint32_t fuse = encodeCalibFuse(1u, 10u, 0u, 620u, 0u);

	ASSERT_EQ(tgm_napll_start(&io, TGM_CHIPSET_GM20B_C1, fuse, 800000, 38400u, 921600u, &state),
		  0);
	ASSERT_EQ(tgm_napll_reprogram_mnp(&io, &state, 1u, 48u, 4u), 0);
	EXPECT_EQ(state.khz, 230400u);
	EXPECT_EQ(hw.regs[TGM_GPCPLL_COEFF_R], 1u | (48u << 8) | (4u << 16));

	EXPECT_EQ(tgm_napll_reprogram(&io, &state, 0u), TGM_NAPLL_ERR_INVAL);
	EXPECT_EQ(state.khz, 230400u);
	EXPECT_EQ(state.active, 1);

	hw.fail_ramp = true;
	EXPECT_EQ(tgm_napll_reprogram_mnp(&io, &state, 1u, 48u, 1u), TGM_NAPLL_ERR_TIMEDOUT);
	EXPECT_EQ(state.khz, 0u);
	EXPECT_EQ(state.active, 1);

	hw.fail_ramp = false;
	ASSERT_EQ(tgm_napll_reprogram_mnp(&io, &state, 1u, 34u, 4u), 0);
	EXPECT_EQ(state.khz, 163200u);
	hw.fail_ramp = true;
	EXPECT_EQ(tgm_napll_reprogram_mnp(&io, &state, 1u, 48u, 1u), TGM_NAPLL_ERR_TIMEDOUT);
	EXPECT_EQ(state.khz, 0u);
	EXPECT_EQ(state.active, 1);
	EXPECT_EQ(state.dvfs.n_int, 34u);
}

TEST(Gm20bNapll, RunningCheckHandlesBothFixedAndNapllModes)
{
	EXPECT_EQ(tgm_napll_cfg_running(TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_PLL_LOCK, 0u), 1);
	EXPECT_EQ(tgm_napll_cfg_running(TGM_GPCPLL_CFG_ENABLE, 0u), 0);
	EXPECT_EQ(tgm_napll_cfg_running(TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_SYNC_MODE,
					TGM_GPCPLL_DVFS1_EN_DFS),
		  1);
	EXPECT_EQ(tgm_napll_cfg_running(TGM_GPCPLL_CFG_ENABLE, TGM_GPCPLL_DVFS1_EN_DFS), 0);
	EXPECT_EQ(tgm_napll_cfg_running(TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_SYNC_MODE |
						TGM_GPCPLL_CFG_IDDQ,
					TGM_GPCPLL_DVFS1_EN_DFS),
		  0);
}

TEST(Gm20bNapll, GpuClockAndSleepWireNapllLifecycle)
{
	const std::string clock = readText(GPU_CLOCK_C);
	EXPECT_NE(clock.find("GPU_NAPLL_TARGET_KHZ 921600u"), std::string::npos);
	EXPECT_NE(clock.find("chipset == TGM_CHIPSET_GM20B_C1"), std::string::npos);
	EXPECT_NE(clock.find("if (!ret && uv != ceil_uv)"), std::string::npos);
	EXPECT_NE(clock.find("tgm_napll_start("), std::string::npos);
	EXPECT_NE(clock.find("tgm_napll_reprogram_mnp("), std::string::npos);
	EXPECT_NE(clock.find("tgm_napll_cfg_running("), std::string::npos);
	EXPECT_NE(clock.find("tgm_clk_suspend("), std::string::npos);

	const std::string power = readText(GPU_POWER_C);
	const size_t susp = power.find("tgm_clk_suspend(device);");
	const size_t fini = power.find("nvkm_device_fini(device, NVKM_SUSPEND)");
	ASSERT_NE(susp, std::string::npos);
	ASSERT_NE(fini, std::string::npos);
	EXPECT_LT(susp, fini);
}
}  // namespace
