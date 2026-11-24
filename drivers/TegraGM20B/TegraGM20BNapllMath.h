/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_NAPLL_MATH_H
#define TEGRA_GM20B_NAPLL_MATH_H

#include <stdint.h>

#define TGM_CHIPSET_GM20B_B1 0x12bu
#define TGM_CHIPSET_GM20B_C1 0x12eu

#define TGM_THERM_USE_A_R 0x020798u
#define TGM_GPC_BCAST_DVFS2_R 0x132820u
#define TGM_GPC_BCAST_NDIV_DEBUG_R 0x1328a0u
#define TGM_GPCPLL_CFG_R 0x137000u
#define TGM_GPCPLL_COEFF_R 0x137004u
#define TGM_GPCPLL_CFG2_R 0x13700cu
#define TGM_GPCPLL_DVFS0_R 0x137010u
#define TGM_GPCPLL_DVFS1_R 0x137014u
#define TGM_GPCPLL_CFG3_R 0x137018u
#define TGM_GPCPLL_NDIV_SLOWDOWN_R 0x13701cu
#define TGM_SEL_VCO_R 0x137100u
#define TGM_GPC2CLK_OUT_R 0x137250u
#define TGM_FUSE_RESERVED_CALIB0_OFF 0x204u

#define TGM_GPCPLL_CFG_ENABLE (1u << 0)
#define TGM_GPCPLL_CFG_IDDQ (1u << 1)
#define TGM_GPCPLL_CFG_SYNC_MODE (1u << 2)
#define TGM_GPCPLL_CFG_ENB_LCKDET_OFF (1u << 4)
#define TGM_GPCPLL_CFG_PLL_LOCK (1u << 17)

#define TGM_GPCPLL_COEFF_MDIV_MASK 0x000000ffu
#define TGM_GPCPLL_COEFF_NDIV_SHIFT 8u
#define TGM_GPCPLL_COEFF_NDIV_MASK (0xffu << 8)
#define TGM_GPCPLL_COEFF_PLDIV_SHIFT 16u
#define TGM_GPCPLL_COEFF_PLDIV_MASK (0x3fu << 16)

#define TGM_GPCPLL_CFG2_SDM_DIN_MASK 0x000000ffu
#define TGM_GPCPLL_CFG2_SDM_DIN_NEW_SHIFT 8u
#define TGM_GPCPLL_CFG2_SDM_DIN_NEW_MASK (0xffu << 8)
#define TGM_GPCPLL_CFG2_PLL_STEPA_SHIFT 24u
#define TGM_GPCPLL_CFG2_PLL_STEPA_MASK (0xffu << 24)

#define TGM_GPCPLL_DVFS0_DFS_COEFF_MASK 0x0000007fu
#define TGM_GPCPLL_DVFS0_DFS_DET_MAX_SHIFT 8u
#define TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK (0x7fu << 8)

#define TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT 16u
#define TGM_GPCPLL_DVFS1_DFS_CTRL_MASK (0xfffu << 16)
#define TGM_GPCPLL_DVFS1_EN_DFS (1u << 29)
#define TGM_GPCPLL_DVFS1_EN_DFS_CAL (1u << 30)
#define TGM_GPCPLL_DVFS1_DFS_CAL_DONE (1u << 31)

#define TGM_DFS_CTRL_TESTOUT_DET (1u << 0)
#define TGM_DFS_CTRL_EXT_CAL_EN (1u << 9)
#define TGM_DFS_CTRL_C1_INIT (0x3u << 10)

#define TGM_GPCPLL_CFG3_VCO_CTRL_MASK 0x000001ffu
#define TGM_GPCPLL_CFG3_PLL_STEPB_SHIFT 16u
#define TGM_GPCPLL_CFG3_PLL_STEPB_MASK (0xffu << 16)
#define TGM_GPCPLL_CFG3_DFS_TESTOUT_SHIFT 24u
#define TGM_GPCPLL_CFG3_DFS_TESTOUT_MASK (0x7fu << 24)

#define TGM_GPCPLL_SLOWDOWN_USING_PLL (1u << 22)
#define TGM_GPCPLL_SLOWDOWN_EN_DYNRAMP (1u << 31)

#define TGM_GPC_BCAST_DVFS2_EXT_DET_MASK 0x0000007fu
#define TGM_GPC_BCAST_DVFS2_EXT_STROBE (1u << 16)
#define TGM_GPC_BCAST_DYNRAMP_DONE (1u << 24)

#define TGM_SEL_VCO_GPC2CLK_OUT (1u << 0)
#define TGM_GPC2CLK_OUT_BYPDIV_MASK 0x0000003fu
#define TGM_GPC2CLK_OUT_VCODIV_SHIFT 8u
#define TGM_GPC2CLK_OUT_VCODIV_MASK (0x3fu << 8)
#define TGM_GPC2CLK_OUT_VCODIV_BY1 0u
#define TGM_GPC2CLK_OUT_VCODIV_BY2 2u

#define TGM_NAPLL_REF_KHZ 38400u
#define TGM_NAPLL_DFS_DET_RANGE 6u
#define TGM_NAPLL_SDM_DIN_RANGE 12u
#define TGM_NAPLL_ADC_SLOPE_UV 10000
#define TGM_NAPLL_STEPA_38400 0x04u
#define TGM_NAPLL_STEPB_38400 0x05u
#define TGM_NAPLL_IDDQ_EXIT_US 5u
#define TGM_NAPLL_LOCK_DELAY_US 40u
#define TGM_NAPLL_RAMP_TIMEOUT_US 500u

#define TGM_NAPLL_ERR_IO (-5)
#define TGM_NAPLL_ERR_NODEV (-19)
#define TGM_NAPLL_ERR_INVAL (-22)
#define TGM_NAPLL_ERR_RANGE (-34)
#define TGM_NAPLL_ERR_TIMEDOUT (-110)

struct tgm_napll_params {
	uint32_t chipset;
	uint32_t min_vco;
	uint32_t max_vco;
	uint32_t min_n;
	uint32_t max_n;
	uint32_t min_pl;
	uint32_t max_pl;
	int32_t coeff_slope;
	int32_t coeff_offs;
	uint32_t vco_ctrl;
	uint32_t dfs_ctrl;
};

struct tgm_napll_dvfs {
	int32_t mv;
	int32_t uv_cal;
	uint32_t dfs_coeff;
	uint32_t dfs_det_max;
	int32_t dfs_ext_cal;
	uint32_t n_int;
	uint32_t sdm_din;
};

struct tgm_napll_target {
	uint32_t khz;
	uint32_t m;
	uint32_t n;
	uint32_t pl;
	uint32_t vco_khz;
	uint32_t n_lo;
};

struct tgm_napll_state {
	struct tgm_napll_params params;
	struct tgm_napll_dvfs dvfs;
	int32_t uvdet_slope;
	int32_t uvdet_offs;
	int32_t mv;
	uint32_t ref_khz;
	uint32_t khz;
	int fused;
	int active;
};

struct tgm_napll_io {
	void *arg;
	uint32_t (*rd32)(void *arg, uint32_t reg);
	void (*wr32)(void *arg, uint32_t reg, uint32_t val);
	void (*udelay)(void *arg, uint32_t us);
};

static inline int32_t tgm_napll_div_round_closest(int64_t x, int32_t d)
{
	return (int32_t)(x >= 0 ? (x + d / 2) / d : (x - d / 2) / d);
}

static inline int tgm_napll_cfg_running(uint32_t cfg, uint32_t dvfs1)
{
	if (dvfs1 & TGM_GPCPLL_DVFS1_EN_DFS) {
		const uint32_t mask =
			TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_IDDQ | TGM_GPCPLL_CFG_SYNC_MODE;

		return (cfg & mask) == (TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_SYNC_MODE);
	}
	return (cfg & (TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_PLL_LOCK)) ==
	       (TGM_GPCPLL_CFG_ENABLE | TGM_GPCPLL_CFG_PLL_LOCK);
}

static inline int tgm_napll_params_for_chipset(uint32_t chipset, struct tgm_napll_params *out)
{
	if (!out)
		return TGM_NAPLL_ERR_INVAL;
	out->chipset = chipset;
	out->min_vco = 1300000u;
	out->max_vco = 2600000u;
	out->min_n = 8u;
	out->max_n = 255u;
	out->min_pl = 1u;
	out->max_pl = 31u;
	if (chipset == TGM_CHIPSET_GM20B_C1) {
		out->coeff_slope = -172550;
		out->coeff_offs = 195374;
		out->vco_ctrl = (0x1u << 3) | 0x7u;
		out->dfs_ctrl = TGM_DFS_CTRL_C1_INIT;
		return 0;
	}
	if (chipset == TGM_CHIPSET_GM20B_B1) {
		out->coeff_slope = -165230;
		out->coeff_offs = 214007;
		out->vco_ctrl = 0x7u << 3;
		out->dfs_ctrl = 0u;
		return 0;
	}
	return TGM_NAPLL_ERR_NODEV;
}

static inline int tgm_napll_decode_calib_fuse(uint32_t fuse_val, int32_t *slope_uv,
					      int32_t *offs_uv)
{
	uint32_t rev = (fuse_val >> 30) & 0x3u;
	int32_t slope = (int32_t)(((fuse_val >> 24) & 0x3fu) * 1000u + ((fuse_val >> 14) & 0x3ffu));
	int32_t offs = (int32_t)(((fuse_val >> 4) & 0x3ffu) * 1000u + (fuse_val & 0xfu) * 100u);

	if (!slope_uv || !offs_uv || !rev || slope <= 0 || offs <= 0)
		return TGM_NAPLL_ERR_INVAL;
	*slope_uv = slope;
	*offs_uv = offs;
	return 0;
}

static inline int tgm_napll_calc_detection(const struct tgm_napll_params *p, int32_t uvdet_slope,
					   int32_t uvdet_offs, int32_t mv, struct tgm_napll_dvfs *d)
{
	int32_t coeff = 0;
	int32_t ext_cal = 0;
	const int32_t limit = 1 << TGM_NAPLL_DFS_DET_RANGE;

	if (!p || !d || uvdet_slope <= 0 || mv <= 0)
		return TGM_NAPLL_ERR_INVAL;
	coeff = tgm_napll_div_round_closest((int64_t)mv * p->coeff_slope, 1000) + p->coeff_offs;
	coeff = tgm_napll_div_round_closest(coeff, 1000);
	if (coeff < 0)
		coeff = 0;
	if (coeff > (int32_t)TGM_GPCPLL_DVFS0_DFS_COEFF_MASK)
		coeff = (int32_t)TGM_GPCPLL_DVFS0_DFS_COEFF_MASK;
	ext_cal = tgm_napll_div_round_closest((int64_t)mv * 1000 - uvdet_offs, uvdet_slope);
	if (ext_cal <= -limit || ext_cal >= limit)
		return TGM_NAPLL_ERR_RANGE;
	d->mv = mv;
	d->dfs_coeff = (uint32_t)coeff;
	d->dfs_ext_cal = ext_cal;
	d->uv_cal = uvdet_offs + ext_cal * uvdet_slope;
	d->dfs_det_max = 0u;
	return 0;
}

static inline int tgm_napll_calc_ndiv(const struct tgm_napll_params *p, int32_t uvdet_slope,
				      int32_t uvdet_offs, int32_t mv, uint32_t n_eff,
				      struct tgm_napll_dvfs *d)
{
	int32_t det_delta = 0;
	int32_t n = 0;
	uint32_t rem = 0;
	int32_t sdm = 0;
	int32_t sdm_shifted = 0;

	if (!p || !d || uvdet_slope <= 0 || n_eff < p->min_n || n_eff > p->max_n)
		return TGM_NAPLL_ERR_INVAL;
	det_delta = tgm_napll_div_round_closest((int64_t)mv * 1000 - uvdet_offs, uvdet_slope) -
		    d->dfs_ext_cal;
	if (det_delta > (int32_t)d->dfs_det_max)
		det_delta = (int32_t)d->dfs_det_max;
	det_delta *= (int32_t)d->dfs_coeff;
	n = (int32_t)(n_eff << TGM_NAPLL_DFS_DET_RANGE) - det_delta;
	if (n < 0 || n > (int32_t)(p->max_n << TGM_NAPLL_DFS_DET_RANGE))
		return TGM_NAPLL_ERR_RANGE;
	d->n_int = (uint32_t)n >> TGM_NAPLL_DFS_DET_RANGE;
	rem = (uint32_t)n & ((1u << TGM_NAPLL_DFS_DET_RANGE) - 1u);
	sdm = (int32_t)(rem << (TGM_NAPLL_SDM_DIN_RANGE + 1u - TGM_NAPLL_DFS_DET_RANGE)) -
	      (1 << TGM_NAPLL_SDM_DIN_RANGE);
	sdm_shifted = sdm >= 0 ? (sdm / 256) : -(((-sdm) + 255) / 256);
	d->sdm_din = (uint32_t)sdm_shifted & 0xffu;
	return 0;
}

static inline int tgm_napll_calc_mnp(const struct tgm_napll_params *p, uint32_t ref_khz,
				     uint32_t gpc_khz, struct tgm_napll_target *out)
{
	uint32_t gpc2_khz = 0;
	uint32_t n_lo = 0;
	uint32_t pl = 0;

	if (!p || !out || ref_khz != TGM_NAPLL_REF_KHZ || !gpc_khz)
		return TGM_NAPLL_ERR_INVAL;
	gpc2_khz = gpc_khz * 2u;
	n_lo = (p->min_vco + ref_khz - 1u) / ref_khz;
	for (pl = p->min_pl; pl <= p->max_pl; pl++) {
		uint32_t vco = gpc2_khz * pl;
		uint32_t n = 0;

		if (vco < p->min_vco)
			continue;
		if (vco > p->max_vco)
			break;
		if (vco % ref_khz)
			continue;
		n = vco / ref_khz;
		if (n >= n_lo && n <= p->max_n) {
			out->khz = gpc_khz;
			out->m = 1u;
			out->n = n;
			out->pl = pl;
			out->vco_khz = vco;
			out->n_lo = n_lo;
			return 0;
		}
	}
	return TGM_NAPLL_ERR_RANGE;
}

static inline int tgm_napll_target_from_mnp(const struct tgm_napll_params *p, uint32_t ref_khz,
					    uint32_t m, uint32_t n, uint32_t pl,
					    struct tgm_napll_target *out)
{
	uint32_t n_lo = 0;
	uint32_t vco = 0;

	if (!p || !out || ref_khz != TGM_NAPLL_REF_KHZ || m != 1u || pl < p->min_pl ||
	    pl > p->max_pl)
		return TGM_NAPLL_ERR_INVAL;
	n_lo = (p->min_vco + ref_khz - 1u) / ref_khz;
	if (n < n_lo || n > p->max_n)
		return TGM_NAPLL_ERR_RANGE;
	vco = ref_khz * n;
	if (vco < p->min_vco || vco > p->max_vco)
		return TGM_NAPLL_ERR_RANGE;
	out->khz = vco / (2u * pl);
	out->m = m;
	out->n = n;
	out->pl = pl;
	out->vco_khz = vco;
	out->n_lo = n_lo;
	return 0;
}

static inline uint32_t tgm_napll_select_rate_khz(uint32_t target_khz, int ceil_uv, int margin_pct,
						 uint32_t (*row_khz)(int),
						 int (*row_na_uv)(uint32_t, int), int speedo,
						 uint32_t *highest_fit_khz)
{
	uint32_t k = 0;
	uint32_t best = 0;
	uint32_t selected = 0;
	int fits = 1;
	int i = 0;

	if (highest_fit_khz)
		*highest_fit_khz = 0u;
	if (!row_khz || !row_na_uv || ceil_uv <= 0 || margin_pct < 0 || speedo <= 0)
		return 0u;
	for (i = 0; (k = row_khz(i)) != 0u; i++) {
		int v = row_na_uv(k, speedo);
		int need = v > 0 ? (int)(((int64_t)v * (100 + margin_pct) + 99) / 100) : 0;

		fits = fits && v > 0 && need <= ceil_uv;
		if (!fits)
			continue;
		best = k;
		if (k <= target_khz)
			selected = k;
	}
	if (highest_fit_khz)
		*highest_fit_khz = best;
	return selected;
}

#endif
