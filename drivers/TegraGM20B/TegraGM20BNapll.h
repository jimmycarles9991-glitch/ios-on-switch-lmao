/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_NAPLL_H
#define TEGRA_GM20B_NAPLL_H

#include "TegraGM20BNapllMath.h"

static inline void tgm_napll_rmw(const struct tgm_napll_io *io, uint32_t reg, uint32_t mask,
				 uint32_t val)
{
	uint32_t cur = io->rd32(io->arg, reg);

	io->wr32(io->arg, reg, (cur & ~mask) | (val & mask));
}

static inline void tgm_napll_set_sel_vco(const struct tgm_napll_io *io, int on)
{
	uint32_t throt = io->rd32(io->arg, TGM_THERM_USE_A_R);

	io->wr32(io->arg, TGM_THERM_USE_A_R, 0u);
	tgm_napll_rmw(io, TGM_SEL_VCO_R, TGM_SEL_VCO_GPC2CLK_OUT,
		      on ? TGM_SEL_VCO_GPC2CLK_OUT : 0u);
	io->wr32(io->arg, TGM_THERM_USE_A_R, throt);
}

static inline void tgm_napll_set_vcodiv(const struct tgm_napll_io *io, uint32_t div)
{
	uint32_t data = io->rd32(io->arg, TGM_GPC2CLK_OUT_R);

	data = (data & ~TGM_GPC2CLK_OUT_VCODIV_MASK) |
	       ((div << TGM_GPC2CLK_OUT_VCODIV_SHIFT) & TGM_GPC2CLK_OUT_VCODIV_MASK);
	/* Two consecutive writes guarantee linear divider operation on GM20B. */
	io->wr32(io->arg, TGM_GPC2CLK_OUT_R, data);
	io->wr32(io->arg, TGM_GPC2CLK_OUT_R, data);
	(void)io->rd32(io->arg, TGM_GPC2CLK_OUT_R);
}

static inline int tgm_napll_slide(const struct tgm_napll_io *io, uint32_t n_int, uint32_t sdm_din)
{
	uint32_t coeff = io->rd32(io->arg, TGM_GPCPLL_COEFF_R);
	uint32_t nold = (coeff & TGM_GPCPLL_COEFF_NDIV_MASK) >> TGM_GPCPLL_COEFF_NDIV_SHIFT;
	uint32_t cfg2 = io->rd32(io->arg, TGM_GPCPLL_CFG2_R);
	uint32_t sdm_old = cfg2 & TGM_GPCPLL_CFG2_SDM_DIN_MASK;
	uint32_t timeout = TGM_NAPLL_RAMP_TIMEOUT_US;
	int done = 0;

	if (nold == n_int && sdm_old == (sdm_din & 0xffu))
		return 0;
	tgm_napll_rmw(io, TGM_GPCPLL_NDIV_SLOWDOWN_R, TGM_GPCPLL_SLOWDOWN_USING_PLL,
		      TGM_GPCPLL_SLOWDOWN_USING_PLL);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG2_R, TGM_GPCPLL_CFG2_SDM_DIN_NEW_MASK,
		      (sdm_din & 0xffu) << TGM_GPCPLL_CFG2_SDM_DIN_NEW_SHIFT);
	coeff = io->rd32(io->arg, TGM_GPCPLL_COEFF_R);
	io->udelay(io->arg, 1u);
	io->wr32(io->arg, TGM_GPCPLL_COEFF_R,
		 (coeff & ~TGM_GPCPLL_COEFF_NDIV_MASK) |
			 ((n_int << TGM_GPCPLL_COEFF_NDIV_SHIFT) & TGM_GPCPLL_COEFF_NDIV_MASK));
	io->udelay(io->arg, 1u);
	tgm_napll_rmw(io, TGM_GPCPLL_NDIV_SLOWDOWN_R, TGM_GPCPLL_SLOWDOWN_EN_DYNRAMP,
		      TGM_GPCPLL_SLOWDOWN_EN_DYNRAMP);
	while (timeout > 0u) {
		io->udelay(io->arg, 1u);
		timeout--;
		if (io->rd32(io->arg, TGM_GPC_BCAST_NDIV_DEBUG_R) & TGM_GPC_BCAST_DYNRAMP_DONE) {
			done = 1;
			break;
		}
	}
	if (done) {
		tgm_napll_rmw(io, TGM_GPCPLL_CFG2_R, TGM_GPCPLL_CFG2_SDM_DIN_MASK, sdm_din & 0xffu);
	}
	tgm_napll_rmw(io, TGM_GPCPLL_NDIV_SLOWDOWN_R,
		      TGM_GPCPLL_SLOWDOWN_USING_PLL | TGM_GPCPLL_SLOWDOWN_EN_DYNRAMP, 0u);
	(void)io->rd32(io->arg, TGM_GPCPLL_NDIV_SLOWDOWN_R);
	return done ? 0 : TGM_NAPLL_ERR_TIMEDOUT;
}

static inline void tgm_napll_write_detection(const struct tgm_napll_io *io,
					     const struct tgm_napll_dvfs *d)
{
	uint32_t data = 0;
	uint32_t ctrl = 0;

	tgm_napll_rmw(io, TGM_GPC_BCAST_DVFS2_R, TGM_GPC_BCAST_DVFS2_EXT_STROBE,
		      TGM_GPC_BCAST_DVFS2_EXT_STROBE);
	tgm_napll_rmw(io, TGM_GPCPLL_DVFS0_R,
		      TGM_GPCPLL_DVFS0_DFS_COEFF_MASK | TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK,
		      (d->dfs_coeff & TGM_GPCPLL_DVFS0_DFS_COEFF_MASK) |
			      ((d->dfs_det_max << TGM_GPCPLL_DVFS0_DFS_DET_MAX_SHIFT) &
			       TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK));
	(void)io->rd32(io->arg, TGM_GPC_BCAST_DVFS2_R);
	io->udelay(io->arg, 1u);
	tgm_napll_rmw(io, TGM_GPC_BCAST_DVFS2_R, TGM_GPC_BCAST_DVFS2_EXT_STROBE, 0u);

	tgm_napll_rmw(io, TGM_GPC_BCAST_DVFS2_R, TGM_GPC_BCAST_DVFS2_EXT_DET_MASK,
		      (uint32_t)d->dfs_ext_cal & TGM_GPC_BCAST_DVFS2_EXT_DET_MASK);
	data = io->rd32(io->arg, TGM_GPCPLL_DVFS1_R);
	io->udelay(io->arg, 1u);
	ctrl = (data & TGM_GPCPLL_DVFS1_DFS_CTRL_MASK) >> TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT;
	if (!(ctrl & TGM_DFS_CTRL_EXT_CAL_EN)) {
		uint32_t shifted = 0;

		ctrl |= TGM_DFS_CTRL_EXT_CAL_EN | TGM_DFS_CTRL_TESTOUT_DET;
		shifted = ctrl << TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT;
		data = (data & ~TGM_GPCPLL_DVFS1_DFS_CTRL_MASK) |
		       (shifted & TGM_GPCPLL_DVFS1_DFS_CTRL_MASK);
		io->wr32(io->arg, TGM_GPCPLL_DVFS1_R, data);
	}
}

static inline void tgm_napll_disable_pll(const struct tgm_napll_io *io)
{
	tgm_napll_set_sel_vco(io, 0);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_SYNC_MODE, 0u);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_ENABLE, 0u);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
}

static inline void tgm_napll_clear_dvfs(const struct tgm_napll_io *io)
{
	tgm_napll_disable_pll(io);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_IDDQ, TGM_GPCPLL_CFG_IDDQ);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	tgm_napll_rmw(io, TGM_GPCPLL_DVFS1_R,
		      TGM_GPCPLL_DVFS1_EN_DFS | TGM_GPCPLL_DVFS1_EN_DFS_CAL |
			      TGM_GPCPLL_DVFS1_DFS_CTRL_MASK,
		      0u);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG3_R, TGM_GPCPLL_CFG3_VCO_CTRL_MASK, 0u);
	tgm_napll_rmw(io, TGM_GPCPLL_DVFS0_R,
		      TGM_GPCPLL_DVFS0_DFS_COEFF_MASK | TGM_GPCPLL_DVFS0_DFS_DET_MAX_MASK, 0u);
	tgm_napll_rmw(io, TGM_GPC_BCAST_DVFS2_R,
		      TGM_GPC_BCAST_DVFS2_EXT_DET_MASK | TGM_GPC_BCAST_DVFS2_EXT_STROBE, 0u);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG2_R,
		      TGM_GPCPLL_CFG2_SDM_DIN_MASK | TGM_GPCPLL_CFG2_SDM_DIN_NEW_MASK, 0u);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_IDDQ, 0u);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	io->udelay(io->arg, TGM_NAPLL_IDDQ_EXIT_US);
	tgm_napll_set_vcodiv(io, TGM_GPC2CLK_OUT_VCODIV_BY1);
}

static inline int tgm_napll_enable_dvfs(const struct tgm_napll_io *io,
					const struct tgm_napll_params *p, int32_t poweron_uv,
					int32_t *uvdet_slope, int32_t *uvdet_offs)
{
	int calibrated = *uvdet_slope > 0 && *uvdet_offs > 0;
	uint32_t delay = TGM_NAPLL_IDDQ_EXIT_US;
	uint32_t testout = 0;
	int cal_done = 0;

	/* Hardware requires EN_DFS, VCO_CTRL, DFS_CTRL and fused EN_DFS_CAL set in IDDQ. */
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_IDDQ | TGM_GPCPLL_CFG_ENB_LCKDET_OFF,
		      TGM_GPCPLL_CFG_IDDQ | TGM_GPCPLL_CFG_ENB_LCKDET_OFF);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);

	tgm_napll_rmw(io, TGM_GPCPLL_DVFS1_R, TGM_GPCPLL_DVFS1_EN_DFS, TGM_GPCPLL_DVFS1_EN_DFS);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG3_R, TGM_GPCPLL_CFG3_VCO_CTRL_MASK, p->vco_ctrl);
	tgm_napll_rmw(io, TGM_GPCPLL_DVFS1_R, TGM_GPCPLL_DVFS1_DFS_CTRL_MASK,
		      (p->dfs_ctrl << TGM_GPCPLL_DVFS1_DFS_CTRL_SHIFT) &
			      TGM_GPCPLL_DVFS1_DFS_CTRL_MASK);
	tgm_napll_rmw(io, TGM_GPCPLL_DVFS1_R, TGM_GPCPLL_DVFS1_EN_DFS_CAL,
		      calibrated ? TGM_GPCPLL_DVFS1_EN_DFS_CAL : 0u);

	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_IDDQ, 0u);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	io->udelay(io->arg, TGM_NAPLL_IDDQ_EXIT_US);

	tgm_napll_rmw(io, TGM_GPCPLL_CFG2_R, TGM_GPCPLL_CFG2_PLL_STEPA_MASK,
		      TGM_NAPLL_STEPA_38400 << TGM_GPCPLL_CFG2_PLL_STEPA_SHIFT);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG3_R, TGM_GPCPLL_CFG3_PLL_STEPB_MASK,
		      TGM_NAPLL_STEPB_38400 << TGM_GPCPLL_CFG3_PLL_STEPB_SHIFT);

	if (calibrated)
		return 0;

	tgm_napll_rmw(io, TGM_GPCPLL_DVFS1_R, TGM_GPCPLL_DVFS1_EN_DFS_CAL,
		      TGM_GPCPLL_DVFS1_EN_DFS_CAL);
	/* C1 PLL must be enabled to latch internal calibration results. */
	if (p->chipset == TGM_CHIPSET_GM20B_C1)
		tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_ENABLE, TGM_GPCPLL_CFG_ENABLE);

	while (delay > 0u) {
		if (io->rd32(io->arg, TGM_GPCPLL_DVFS1_R) & TGM_GPCPLL_DVFS1_DFS_CAL_DONE) {
			cal_done = 1;
			break;
		}
		io->udelay(io->arg, 1u);
		delay--;
	}
	testout = (io->rd32(io->arg, TGM_GPCPLL_CFG3_R) & TGM_GPCPLL_CFG3_DFS_TESTOUT_MASK) >>
		  TGM_GPCPLL_CFG3_DFS_TESTOUT_SHIFT;

	if (p->chipset == TGM_CHIPSET_GM20B_C1) {
		tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_ENABLE, 0u);
		(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	}
	if (!cal_done)
		return TGM_NAPLL_ERR_TIMEDOUT;
	*uvdet_offs = poweron_uv - (int32_t)testout * TGM_NAPLL_ADC_SLOPE_UV;
	*uvdet_slope = TGM_NAPLL_ADC_SLOPE_UV;
	return 0;
}

static inline int tgm_napll_program(const struct tgm_napll_io *io, const struct tgm_napll_params *p,
				    int32_t uvdet_slope, int32_t uvdet_offs, int32_t mv,
				    const struct tgm_napll_target *target,
				    struct tgm_napll_dvfs *dvfs_out, int *hw_touched)
{
	struct tgm_napll_dvfs dvfs_lo = {0, 0, 0, 0, 0, 0, 0};
	struct tgm_napll_dvfs dvfs_hi = {0, 0, 0, 0, 0, 0, 0};
	uint32_t coeff = 0;
	uint32_t old_pl = 0;
	uint32_t cfg = 0;
	int ret = 0;

	if (hw_touched)
		*hw_touched = 0;
	ret = tgm_napll_calc_detection(p, uvdet_slope, uvdet_offs, mv, &dvfs_lo);
	if (ret)
		return ret;
	dvfs_hi = dvfs_lo;
	ret = tgm_napll_calc_ndiv(p, uvdet_slope, uvdet_offs, mv, target->n_lo, &dvfs_lo);
	if (ret)
		return ret;
	ret = tgm_napll_calc_ndiv(p, uvdet_slope, uvdet_offs, mv, target->n, &dvfs_hi);
	if (ret)
		return ret;

	cfg = io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	if ((cfg & TGM_GPCPLL_CFG_ENABLE) &&
	    (io->rd32(io->arg, TGM_GPCPLL_DVFS1_R) & TGM_GPCPLL_DVFS1_EN_DFS)) {
		if (hw_touched)
			*hw_touched = 1;
		ret = tgm_napll_slide(io, dvfs_lo.n_int, dvfs_lo.sdm_din);
		if (ret)
			return ret;
	}

	if (hw_touched)
		*hw_touched = 1;
	old_pl = (io->rd32(io->arg, TGM_GPCPLL_COEFF_R) & TGM_GPCPLL_COEFF_PLDIV_MASK) >>
		 TGM_GPCPLL_COEFF_PLDIV_SHIFT;
	if (target->pl < 2u || old_pl < 2u) {
		tgm_napll_set_vcodiv(io, TGM_GPC2CLK_OUT_VCODIV_BY2);
		io->udelay(io->arg, 2u);
	}

	tgm_napll_disable_pll(io);
	io->udelay(io->arg, 1u);

	tgm_napll_write_detection(io, &dvfs_lo);
	tgm_napll_rmw(io, TGM_GPCPLL_CFG2_R, TGM_GPCPLL_CFG2_SDM_DIN_MASK, dvfs_lo.sdm_din & 0xffu);
	coeff = (target->m & TGM_GPCPLL_COEFF_MDIV_MASK) |
		((dvfs_lo.n_int << TGM_GPCPLL_COEFF_NDIV_SHIFT) & TGM_GPCPLL_COEFF_NDIV_MASK) |
		((target->pl << TGM_GPCPLL_COEFF_PLDIV_SHIFT) & TGM_GPCPLL_COEFF_PLDIV_MASK);
	io->wr32(io->arg, TGM_GPCPLL_COEFF_R, coeff);

	/* Hardware lock detect is unusable in NA mode: wait na_lock_delay then assert SYNC_MODE. */
	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_ENABLE, TGM_GPCPLL_CFG_ENABLE);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	io->udelay(io->arg, TGM_NAPLL_LOCK_DELAY_US);

	tgm_napll_rmw(io, TGM_GPCPLL_CFG_R, TGM_GPCPLL_CFG_SYNC_MODE, TGM_GPCPLL_CFG_SYNC_MODE);
	(void)io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	tgm_napll_set_sel_vco(io, 1);

	if ((io->rd32(io->arg, TGM_GPC2CLK_OUT_R) & TGM_GPC2CLK_OUT_VCODIV_MASK) !=
	    TGM_GPC2CLK_OUT_VCODIV_BY1) {
		io->udelay(io->arg, 2u);
		tgm_napll_set_vcodiv(io, TGM_GPC2CLK_OUT_VCODIV_BY1);
	}
	if (dvfs_out)
		*dvfs_out = dvfs_lo;

	ret = tgm_napll_slide(io, dvfs_hi.n_int, dvfs_hi.sdm_din);
	if (ret)
		return ret;
	if (dvfs_out)
		*dvfs_out = dvfs_hi;
	return 0;
}

static inline int tgm_napll_start(const struct tgm_napll_io *io, uint32_t chipset,
				  uint32_t fuse_calib0, int32_t poweron_uv, uint32_t ref_khz,
				  uint32_t target_khz, struct tgm_napll_state *state)
{
	struct tgm_napll_params p = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	struct tgm_napll_target target = {0, 0, 0, 0, 0, 0};
	struct tgm_napll_dvfs dvfs = {0, 0, 0, 0, 0, 0, 0};
	int32_t slope = 0;
	int32_t offs = 0;
	int fused = 0;
	int ret = 0;

	if (!io || !state || poweron_uv <= 0)
		return TGM_NAPLL_ERR_INVAL;
	ret = tgm_napll_params_for_chipset(chipset, &p);
	if (ret)
		return ret;
	ret = tgm_napll_calc_mnp(&p, ref_khz, target_khz, &target);
	if (ret)
		return ret;

	fused = tgm_napll_decode_calib_fuse(fuse_calib0, &slope, &offs) == 0;

	tgm_napll_disable_pll(io);
	ret = tgm_napll_enable_dvfs(io, &p, poweron_uv, &slope, &offs);
	if (!ret)
		ret = tgm_napll_program(io, &p, slope, offs, poweron_uv / 1000, &target, &dvfs, 0);
	if (ret) {
		tgm_napll_clear_dvfs(io);
		state->active = 0;
		state->khz = 0u;
		return ret;
	}

	state->params = p;
	state->dvfs = dvfs;
	state->uvdet_slope = slope;
	state->uvdet_offs = offs;
	state->mv = poweron_uv / 1000;
	state->ref_khz = ref_khz;
	state->khz = target.khz;
	state->fused = fused;
	state->active = 1;
	return 0;
}

static inline int tgm_napll_reprogram_target(const struct tgm_napll_io *io,
					     struct tgm_napll_state *state,
					     const struct tgm_napll_target *target)
{
	struct tgm_napll_dvfs dvfs = state->dvfs;
	int hw_touched = 0;
	int ret = 0;

	ret = tgm_napll_program(io, &state->params, state->uvdet_slope, state->uvdet_offs,
				state->mv, target, &dvfs, &hw_touched);
	if (ret) {
		if (hw_touched) {
			uint32_t cfg = io->rd32(io->arg, TGM_GPCPLL_CFG_R);
			uint32_t dvfs1 = io->rd32(io->arg, TGM_GPCPLL_DVFS1_R);

			state->dvfs = dvfs;
			state->khz = 0u;
			if (!tgm_napll_cfg_running(cfg, dvfs1))
				state->active = 0;
		}
		return ret;
	}
	state->dvfs = dvfs;
	state->khz = target->khz;
	return 0;
}

static inline int tgm_napll_reprogram_mnp(const struct tgm_napll_io *io,
					  struct tgm_napll_state *state, uint32_t m, uint32_t n,
					  uint32_t pl)
{
	struct tgm_napll_target target = {0, 0, 0, 0, 0, 0};
	int ret = 0;

	if (!io || !state || !state->active)
		return TGM_NAPLL_ERR_INVAL;
	ret = tgm_napll_target_from_mnp(&state->params, state->ref_khz, m, n, pl, &target);
	if (ret)
		return ret;
	return tgm_napll_reprogram_target(io, state, &target);
}

static inline int tgm_napll_reprogram(const struct tgm_napll_io *io, struct tgm_napll_state *state,
				      uint32_t target_khz)
{
	struct tgm_napll_target target = {0, 0, 0, 0, 0, 0};
	int ret = 0;

	if (!io || !state || !state->active)
		return TGM_NAPLL_ERR_INVAL;
	ret = tgm_napll_calc_mnp(&state->params, state->ref_khz, target_khz, &target);
	if (ret)
		return ret;
	return tgm_napll_reprogram_target(io, state, &target);
}

static inline void tgm_napll_suspend(const struct tgm_napll_io *io, struct tgm_napll_state *state)
{
	uint32_t cfg = 0;

	if (!io || !state || !state->active)
		return;
	cfg = io->rd32(io->arg, TGM_GPCPLL_CFG_R);
	if (cfg & TGM_GPCPLL_CFG_ENABLE) {
		struct tgm_napll_dvfs dvfs_lo = state->dvfs;
		uint32_t n_lo = (state->params.min_vco + state->ref_khz - 1u) / state->ref_khz;

		if (tgm_napll_calc_ndiv(&state->params, state->uvdet_slope, state->uvdet_offs,
					state->mv, n_lo, &dvfs_lo) == 0)
			(void)tgm_napll_slide(io, dvfs_lo.n_int, dvfs_lo.sdm_din);
	}
	tgm_napll_disable_pll(io);
	state->active = 0;
}

#endif
