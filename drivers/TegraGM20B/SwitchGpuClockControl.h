/* SPDX-License-Identifier: MIT */
#pragma once
#include "SwitchGpuClock.h"

/* Under tgm_idle.clk: a divider change that did not take, which is a defect of ours to find from the line, not a state
 * to wait out. The run rate goes back (the divider, else nouveau's full reprogram of the rate); the idle clock stays in
 * use, so the next idle period drops by the normal path. A run rate that could not be put back leaves the divider
 * marked low, so the next raise runs again. */
static void idle_fail(const char *what, u32 pl)
{
	int back = pl_switch(tgm_idle.run_pl);
	if (back)
		back = clk_prog_khz(tgm_gpu.device, tgm_idle.run_khz);
	WRITE_ONCE(tgm_idle.low, back ? 1 : 0);
	nvxnu_log("tegra-gm20b: idle clock: %s to PL %u did not take (GPCPLL_CFG %08x COEFF %08x "
		  "SEL_VCO %x "
		  "GPC2CLK_OUT %08x); %d kHz now (%d)\n",
		  what, pl, nvkm_rd32(tgm_gpu.device, GPCPLL_CFG_R),
		  nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R), nvkm_rd32(tgm_gpu.device, SEL_VCO_R),
		  nvkm_rd32(tgm_gpu.device, GPC2CLK_OUT_R),
		  nvkm_clk_read(tgm_gpu.device->clk, nv_clk_src_gpc), back);
}

static u32 clock_ceiling_rate(u32 requested)
{
	u32 best = 0, rate = 0, i = 0;
	u32 limit =
		requested && requested < tgm_idle.maximum_khz ? requested : tgm_idle.maximum_khz;
	for (i = 0; (rate = tegra_gm20b_dvfs_khz(i)); ++i)
		if (rate <= limit && rate >= tgm_idle.idle_khz && rate > best &&
		    !(tgm_idle.vco_khz % (2 * rate)) && tgm_idle.vco_khz / (2 * rate) <= 31)
			best = rate;
	return best;
}

int switch_gpu_clock_get(struct switch_gpu_clock_state *state)
{
	if (!state)
		return -EINVAL;
	*state = (struct switch_gpu_clock_state){0, 0, 0, 0};
	if (!tgm_idle.clk)
		return -ENODEV;
	nvxnu_mutex_lock(tgm_idle.clk);
	state->maximum_khz = tgm_idle.maximum_khz;
	state->minimum_khz = tgm_idle.idle_khz;
	state->ceiling_khz = tgm_idle.run_khz;
	state->requested_khz = tgm_idle.requested_khz;
	nvxnu_mutex_unlock(tgm_idle.clk);
	return READ_ONCE(tgm_idle.clock_available) ? 0 : -EIO;
}

int switch_gpu_clock_ceiling(uint32_t requested_khz, uint32_t *applied_khz)
{
	u32 rate = 0, pl = 0, target_pl = 0;
	int result = 0;
	if (!applied_khz)
		return -EINVAL;
	*applied_khz = 0;
	if (!tgm_idle.clk || !READ_ONCE(tgm_idle.clock_available))
		return -ENODEV;
	tgm_gate_enter();
	nvxnu_mutex_lock(tgm_idle.clk);
	*applied_khz = tgm_idle.run_khz;
	rate = clock_ceiling_rate(requested_khz);
	if (!READ_ONCE(tgm_idle.clock_available) || !READ_ONCE(tgm_idle.on)) {
		result = -EIO;
	} else if (!rate) {
		result = -ERANGE;
	} else {
		pl = tgm_idle.vco_khz / (2 * rate);
		target_pl = READ_ONCE(tgm_idle.low) ? tgm_idle.idle_pl : pl;
		if ((nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R) & 0xffff) != tgm_idle.vco_coeff) {
			result = -EIO;
			*applied_khz = (u32)nvkm_clk_read(tgm_gpu.device->clk, nv_clk_src_gpc);
			nvxnu_log(
				"tegra-gm20b: GPU ceiling change refused: the GPCPLL is not on the "
				"idle clock's VCO; current %u kHz\n",
				*applied_khz);
		} else if (pl_switch(target_pl)) {
			idle_fail("ceiling", target_pl);
			result = -EIO;
			*applied_khz = (u32)nvkm_clk_read(tgm_gpu.device->clk, nv_clk_src_gpc);
		} else {
			tgm_idle.run_pl = pl;
			tgm_idle.run_khz = rate;
			tgm_idle.requested_khz = requested_khz;
			*applied_khz = rate;
		}
	}
	nvxnu_mutex_unlock(tgm_idle.clk);
	gate_leave();
	return result;
}

int tgm_clock_ceiling_restore(void)
{
	int result = 0;
	if (!tgm_idle.clk || !READ_ONCE(tgm_idle.clock_available))
		return 0;
	nvxnu_mutex_lock(tgm_idle.clk);
	if ((nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R) & 0xffff) != tgm_idle.vco_coeff ||
	    pl_switch(tgm_idle.run_pl))
		result = -EIO;
	nvxnu_mutex_unlock(tgm_idle.clk);
	return result;
}

/* At a wake (gpu_power.c, tgm_gpu.lock held): the idle clock back in use. A GPCPLL that came back off the run rate (another
 * PL, or another VCO than the idle clock's) is put back on it first, as idle_fail does; one that cannot be leaves the
 * divider marked low, so the first raise tries again. The text is the wake line's. */
const char *tgm_idle_wake(void)
{
	const char *clock = "on";
	nvxnu_mutex_lock(tgm_idle.clk);
	const u32 coeff = nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R);
	if ((coeff & 0xffff) != tgm_idle.vco_coeff || ((coeff >> 16) & 0x3f) != tgm_idle.run_pl) {
		int back =
			(coeff & 0xffff) == tgm_idle.vco_coeff ? pl_switch(tgm_idle.run_pl) : -EIO;
		if (back)
			back = clk_prog_khz(tgm_gpu.device, tgm_idle.run_khz);
		WRITE_ONCE(tgm_idle.low, back ? 1 : 0);
		clock = back ? "on, its run rate not put back (the first raise tries again)"
			     : "on, its run rate put back";
		nvxnu_log(
			"tegra-gm20b: wake: GPCPLL COEFF %08x is not the idle clock's run rate (PL "
			"%u, VCO %04x): %s\n",
			coeff, tgm_idle.run_pl, tgm_idle.vco_coeff, clock);
	}
	tgm_idle.log_start = nvxnu_nanotime();
	__atomic_store_n(&tgm_idle.last_busy, tgm_idle.log_start, __ATOMIC_SEQ_CST);
	WRITE_ONCE(tgm_idle.on, 1);
	nvxnu_mutex_unlock(tgm_idle.clk);
	return clock;
}
