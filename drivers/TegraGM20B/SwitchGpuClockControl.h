/* SPDX-License-Identifier: MIT */
#pragma once
#include "SwitchGpuClock.h"

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
		if ((nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R) & 0xffff) != tgm_idle.vco_coeff ||
		    pl_switch(target_pl)) {
			WRITE_ONCE(tgm_idle.clock_available, 0);
			WRITE_ONCE(tgm_idle.on, 0);
			result = -EIO;
			*applied_khz = (u32)nvkm_clk_read(tgm_gpu.device->clk, nv_clk_src_gpc);
			nvxnu_log("tegra-gm20b: GPU ceiling change failed; current %u kHz\n",
				  *applied_khz);
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
