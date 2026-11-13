// SPDX-License-Identifier: MIT
// The stats line's GPU clock (gpu_internal.h TRIM_GPC0_NCGPCCLK_*), with what fed the GPC clock when each count started.
#include "gpu_internal.h"

#if TGM_STATS
/* Read right after a count starts, inside its 21 us: GPCPLL_COEFF (M 7:0, N 15:8, PL 21:16), GPC2CLK_OUT (VCO divider
 * 13:8, bypass divider 5:0), CLK_SLOWDOWN_0 (THERM's slowdown factor 21:16) and THERM_GATE_CTRL(0) (GR's ELCG mode 1:0).
 * A count at a fraction of the PLL's rate names its divider or slowdown here, or neither. */
struct clk_at_count {
	u32 coeff, out, slow, gate;
};

static void clk_sample(struct nvkm_device *device, struct clk_at_count *at)
{
	at->coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	at->out = nvkm_rd32(device, 0x137250);
	at->slow = nvkm_rd32(device, 0x020160);
	at->gate = nvkm_rd32(device, 0x020200);
}

void tgm_clk_measure(char *buf, unsigned int len)
{
	static int started;
	static struct clk_at_count counted;
	struct clk_at_count next = {0};
	struct nvkm_device *device = tgm_gpu.device;
	u32 cnt = nvkm_rd32(device, TRIM_GPC0_NCGPCCLK_CNT);
	u32 ref_khz = gk20a_clk(device->clk)->parent_rate / 1000;
	u32 vco = nvkm_rd32(device, 0x137100);
	u32 out = nvkm_rd32(device, 0x137250);
	u32 slow = nvkm_rd32(device, 0x020160);
	u32 gate = nvkm_rd32(device, 0x020200);
	int pll = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	char meas[40] = "-";
	const char *divider = "";

	if (started && !badf(cnt)) {
		scnprintf(meas, sizeof(meas), "%llu kHz",
			  (unsigned long long)ref_khz * (cnt & 0xfffff) / NCGPCCLK_CYCLES);
	} else if (started) {
		scnprintf(meas, sizeof(meas), "unread (%08x)", cnt);
	}
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG, NCGPCCLK_CFG_RESET);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG,
		  NCGPCCLK_CFG_ENABLE | NCGPCCLK_CFG_WRITE_EN | NCGPCCLK_CYCLES);
	clk_sample(device, &next);
	if (started) {
		nvxnu_stats(
			"tegra-gm20b: clock count %u in %u reference cycles (%s), at its start "
			"GPCPLL_COEFF %08x GPC2CLK_OUT %08x CLK_SLOWDOWN_0 %08x GATE_CTRL_0 %08x",
			cnt & 0xfffff, NCGPCCLK_CYCLES, meas, counted.coeff, counted.out,
			counted.slow, counted.gate);
	}
	counted = next;
	started = 1;
	if (!(vco & 1)) {
		divider = " BYPASSED";
	} else if (out & 0x3f00) {
		divider = " DIVIDED";
	}
	scnprintf(buf, len, "GPU clock %d kHz%s, measured %s (slowdown %u, ELCG %u)", pll, divider,
		  meas, (slow >> 16) & 0x3f, gate & 3);
}
#endif
