// SPDX-License-Identifier: MIT
/*
 * Tegra210 / T210B01 (Switch OLED) SoC glue for the GM20B: VDD_GPU rail, CAR clocks and reset, PMC
 * clamp, MC hot-reset flush, fuses. nvkm's tegra.c reaches these through the Linux clk / regulator /
 * reset / PMC shims (shim/tegra_shim.c), so nvkm stays verbatim.
 *
 * Sources for every register (paths relative to scratchpad/; "L4T" = CTCaer switch-l4t-kernel-4.9 linux-dev, in
 * gpu/kernel/src/l4t49 and gpu/power-src/gh/k49; "DT" = CTCaer switch-l4t-platform-t210-nx linux-dev
 * kernel-dts/nx-platforms, in gpu/power-src/gh/dts; nvgpu = gpu/kernel/src/nvgpu/drivers/gpu/nvgpu):
 *   CAR   0x60006000  hekate bdk/soc/t210.h:47
 *         CLK_OUT_ENB_X 0x280 / SET 0x284 / CLR 0x288, RST_DEVICES_X 0x28c / SET 0x290 / CLR 0x294 (hekate clock.h:88-91)
 *         gpu = periph clock/reset 184 -> bank X bit 24, pllg_ref = 189 -> bank X bit 29, both gates fed by pll_ref
 *           (L4T clk-tegra-periph.c:858-859; hekate clock.h:633,638); pll_ref = 38.4 MHz OSC / 1 (hekate hw_init.c:75
 *           writes OSC_CTRL 0x50000071: OSC_FREQ 5 = 38.4 MHz, PLL_REF_DIV 0)
 *         PLLP_OUTC 0x67c, pll_p_out5: 7.1 divider bits 31:24, RSTN bit 16, CLKEN bit 17
 *           (L4T clk-tegra-periph.c:286,894-913,1028-1035; clk-pll-out.c:42-62)
 *   PMC   0x7000e400  hekate t210.h:64; GPU_RG_CNTRL 0x2d4 = GPU clamp (hekate pmc_t210.h:278; nvgpu
 *         os/linux/platform_gk20a_tegra.c:429-430), the only PMC step for the GPU: T210/B01 have no GPU powergate
 *         partition (L4T powergate-t21x.c:579-606) and B01 has_gpu_clamps (L4T pmc.c:777-794,4438-4445). Writable
 *         from the non-secure world: hekate l4t.c:1117 hands ATF FLAGS_PMC_NON_SECURE (atf plat_setup.c:323-344)
 *   MC    0x70019000  hekate t210.h:70; GPU = MC client 34 -> CLIENT_HOTRESET_CTRL_1 0x970 / STATUS_1 0x974 bit 2
 *         (nvgpu platform_gk20a_tegra.c:64; L4T kernel-nvidia mc.c:158-259 + mc-regs-t21x.h:122-125, in
 *         gpu/power-src/gh/knv; linux drivers/memory/tegra/tegra210.c TEGRA210_MC_RESET(GPU, 0x970, 0x974, 2))
 *   FUSE  0x7000f800  hekate t210.h:65; readable with CLK_ENB_H_SET 0x328 bit 7 (fuse clock) and MISC_CLK_ENB 0x48
 *         bit 28 (fuse register visibility), which hekate hw_init sets and leaves on (bdk/soc/hw_init.c:274,399;
 *         clock.c:199-203); set again before reading, idempotent. Linux tegra_fuse_readl() offsets are relative to +0x100
 *         (L4T fuse-tegra30.c:32,47,61; hekate FUSE() offsets already include it); ODM28 = hekate 0x240 (fuse.h:177)
 *   MAX77812 on I2C5: 0x31 if !(ODM28 & 1) else 0x33 (hekate bdk/power/max7762x.c:98-109; U-Boot
 *         board/nintendo/switch/nintendo-switch.c:25,57-66: retail = 0x33, as DT tegra210b01-nx-pmic.dtsi:26-28).
 *         M1 = vdd-gpu in both phase configs (DT nx-pmic.dtsi:39-50; L4T max77812-regulator.c:507-521; hekate
 *         max77812.h:20-21,57); M3 = vddio-ddr on 0x33 only, M4 = vdd-cpu (DT nx-pmic.dtsi:62-89; hekate max7762x.c:93-94).
 *         EN_CTRL 0x06 bits 0/4/6 = M1/M3/M4, M1_VOUT 0x23, uV = 250000 + 5000 * code
 *         (L4T max77812-regulator.c:34,53,97-101,397-422; hekate max77812.h:29-38)
 *   T210B01 VDD_GPU: vmin 610 mV (76.8 MHz), cvb max 1050 mV, rail max 1125 mV (L4T tegra210-dvfs.c:200-213,850-876,
 *         922-928), 800 mV at any rate below 20 C (DT nx-common.dtsi:1358-1367 gpu_scaling_cdev: trip0 -25 C 800 mV
 *         holds up to trip1, tegra210b01-trips.h:15-16; tegra210-dvfs.c:1988-1992,2056-2058). switchroot caps it at
 *         800 mV (DT tegra210b01-nx-common.dtsi:530-548, applied by tegra210-dvfs.c:2116-2119) and boots it at 800 mV
 *         (DT nx-pmic.dtsi:45; nvgpu gm20b/clk_gm20b.c:51 BOOT_GPU_UV_C1).
 */
#include <stddef.h>
#include <stdint.h>
#include "shim/include/nvxnu.h"
#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

#define CAR_PA 0x60006000ULL
#define PMC_PA 0x7000e400ULL
#define FUSE_PA 0x7000f800ULL
#define MC_PA 0x70019000ULL

#define CLK_OUT_ENB_X 0x280
#define CLK_ENB_X_SET 0x284
#define CLK_ENB_X_CLR 0x288
#define RST_DEVICES_X 0x28c
#define RST_DEV_X_SET 0x290
#define RST_DEV_X_CLR 0x294
#define X_GPU (1u << 24)
#define X_PLLG_REF (1u << 29)
#define PLLP_OUTC 0x67c
#define PLLP_OUT5_DIV_SHIFT 24
#define PLLP_OUT5_RSTN (1u << 16)
#define PLLP_OUT5_CLKEN (1u << 17)
#define PLLP_RATE 408000000UL
#define OSC_RATE 38400000UL /* T210 pll_ref, the GPCPLL reference */

#define PMC_GPU_RG_CNTRL 0x2d4
#define MC_HOTRESET_CTRL_1 0x970
#define MC_HOTRESET_STATUS_1 0x974
#define MC_HOTRESET_GPU (1u << 2)
#define CLK_ENB_H_SET 0x328
#define CLK_H_FUSE (1u << 7)
#define MISC_CLK_ENB 0x48
#define MISC_FUSE_VISIBLE (1u << 28)
#define FUSE_CACHE 0x100
#define FUSE_ODM28_B01 0x240

#define MAX77812_BUS 5
#define MAX77812_PHASE211 0x33
#define MAX77812_EN_CTRL 0x06
#define MAX77812_EN_M1 0x01u /* vdd-gpu */
#define MAX77812_EN_M3 0x10  /* vddio-ddr (0x33 part) */
#define MAX77812_EN_M4 0x40  /* vdd-cpu */
#define MAX77812_M1_VOUT 0x23
#define MAX77812_UV_MIN 250000
#define MAX77812_UV_STEP 5000
#define GPU_UV_FLOOR 610000
#define GPU_UV_CEIL 800000
#define GPU_VOUT_RAMP_US \
	1000 /* hekate max7762x.c:209-210; 5 mV/us slew (DT nx-pmic.dtsi:29-32,46) */
#define GPU_ENABLE_RAMP_US \
	5000 /* DT nx-pmic.dtsi:47 regulator-enable-ramp-delay, what the L4T regulator core waits */

static volatile uint8_t *car, *pmc, *fuse, *mc;
static uint8_t max77812_addr;

int tegra_gm20b_platform_map(void)
{
	if (car)
		return 0;
	car = (volatile uint8_t *)nvxnu_ioremap(CAR_PA, 0x1000, 0);
	pmc = (volatile uint8_t *)nvxnu_ioremap(PMC_PA, 0x400, 0);
	fuse = (volatile uint8_t *)nvxnu_ioremap(FUSE_PA, 0x400, 0);
	mc = (volatile uint8_t *)nvxnu_ioremap(MC_PA, 0x1000, 0);
	if (!car || !pmc || !fuse || !mc) {
		car = NULL;
		return -12;
	}
	switch_mmio_write32_flush(car, CLK_ENB_H_SET, CLK_H_FUSE);
	switch_mmio_write32_flush(car, MISC_CLK_ENB,
				  switch_mmio_read32(car, MISC_CLK_ENB) | MISC_FUSE_VISIBLE);
	max77812_addr = (switch_mmio_read32(fuse, FUSE_ODM28_B01) & 1) ? MAX77812_PHASE211 : 0x31;
	return 0;
}

uint32_t tegra_gm20b_fuse_read(uint32_t offset)
{
	return switch_mmio_read32(fuse, FUSE_CACHE + offset);
}

/* VDD_GPU: MAX77812 M1 on I2C5. I2C5 is shared with firmware (see TegraI2C.h): one register per transaction,
 * nothing else. */
static int pmic_rd(uint8_t reg, uint8_t *v)
{
	int ret = tegra_i2c_init(
		MAX77812_BUS); /* idempotent; the first PMIC access may precede every other I2C5 user */
	if (ret)
		return ret;
	ret = tegra_i2c_read(MAX77812_BUS, max77812_addr, reg, v, 1);
	if (ret) {
		ret = tegra_i2c_read(MAX77812_BUS, max77812_addr, reg, v,
				     1); /* one retry: arbitration */
	}
	return ret;
}

static int pmic_wr(uint8_t reg, uint8_t v)
{
	int ret = tegra_i2c_write(MAX77812_BUS, max77812_addr, reg, &v, 1);
	if (ret)
		ret = tegra_i2c_write(MAX77812_BUS, max77812_addr, reg, &v, 1);
	return ret;
}

/* EN_CTRL also switches vdd-cpu (M4) and, on the 0x33 part, vddio-ddr (M3); every write keeps their bits as read.
 * Nothing is written to this PMIC unless two EN_CTRL reads agree and show M4 on: hekate sets it before starting the CPU
 * we run on (ccplex.c:47-52), so anything else is the wrong device or a bad read. M3 is not required: it may be held on
 * by its force pin (MAX77620 GPIO2, DT nx-pmic.dtsi:150-158) with its EN bit clear; hekate never sets that bit. */
static int pmic_en_ctrl(uint8_t *en)
{
	uint8_t again = 0;
	int ret = pmic_rd(MAX77812_EN_CTRL, en);
	if (!ret)
		ret = pmic_rd(MAX77812_EN_CTRL, &again);
	if (ret)
		return ret;
	if (again != *en || !(*en & MAX77812_EN_M4)) {
		nvxnu_log("TegraGM20B: MAX77812@0x%02x EN_CTRL %02x/%02x, expected bit 6 (vdd-cpu) "
			  "set: PMIC left untouched\n",
			  max77812_addr, *en, again);
		return -71;
	}
	return 0;
}

int tegra_gm20b_rail_get_uv(int *uv)
{
	uint8_t code = 0;
	int ret = pmic_rd(MAX77812_M1_VOUT, &code);
	if (!ret)
		*uv = MAX77812_UV_MIN + MAX77812_UV_STEP * code;
	return ret;
}

/* Clamped to the B01 window: nvkm's gm20b volt tables are T210's (speedo 0 vmin 950 mV, volt/gm20b.c:61-63), above
 * what switchroot allows this rail. */
int tegra_gm20b_rail_set_uv(int uv)
{
	uint8_t en = 0;
	uint8_t code = 0;
	uint8_t now = 0;
	int ret = 0;
	if (uv < GPU_UV_FLOOR)
		uv = GPU_UV_FLOOR;
	if (uv > GPU_UV_CEIL)
		uv = GPU_UV_CEIL;
	code = (uint8_t)((uv - MAX77812_UV_MIN + MAX77812_UV_STEP - 1) / MAX77812_UV_STEP);
	ret = pmic_en_ctrl(&en);
	if (!ret)
		ret = pmic_rd(MAX77812_M1_VOUT, &now);
	nvxnu_step("tegra-gm20b: M1 step: MAX77812@0x%02x EN_CTRL %02x M1_VOUT %02x (ret %d), "
		   "write M1_VOUT %02x (%d uV)\n",
		   max77812_addr, en, now, ret, code, uv);
	if (!ret)
		ret = pmic_wr(MAX77812_M1_VOUT, code);
	if (!ret)
		ret = pmic_rd(MAX77812_M1_VOUT, &now);
	if (!ret && now != code) {
		nvxnu_log("TegraGM20B: MAX77812 M1_VOUT wrote %02x, reads %02x\n", code, now);
		ret = -71;
	}
	if (!ret)
		nvxnu_usleep(GPU_VOUT_RAMP_US);
	return ret;
}

int tegra_gm20b_rail_enable(int on)
{
	uint8_t en = 0;
	uint8_t now = 0;
	int ret = pmic_en_ctrl(&en);
	if (ret)
		return ret;
	if (!!(en & MAX77812_EN_M1) == !!on)
		return 0;
	if (!on) {
		/* nvgpu gm20b_tegra_railgate() (platform_gk20a_tegra.c:315-325): block the GPU's MC client, clamp and hold
		 * the GPU in reset before its rail goes away. tegra.c has already gated the clocks, so no drain wait. */
		switch_mmio_write32_flush(mc, MC_HOTRESET_CTRL_1,
					  switch_mmio_read32(mc, MC_HOTRESET_CTRL_1) |
						  MC_HOTRESET_GPU);
		switch_mmio_write32_flush(pmc, PMC_GPU_RG_CNTRL, 1);
		nvxnu_udelay(10);
		switch_mmio_write32_flush(car, RST_DEV_X_SET, X_GPU);
		nvxnu_udelay(10);
	}
	nvxnu_step("tegra-gm20b: M1 step: MAX77812 EN_CTRL %02x -> %02x (vdd-gpu %s)\n", en,
		   on ? (en | MAX77812_EN_M1) : (en & ~MAX77812_EN_M1), on ? "on" : "off");
	ret = pmic_wr(MAX77812_EN_CTRL, on ? (en | MAX77812_EN_M1) : (en & ~MAX77812_EN_M1));
	if (!ret)
		ret = pmic_rd(MAX77812_EN_CTRL, &now);
	if (!ret && (now ^ en) != MAX77812_EN_M1) {
		nvxnu_log("TegraGM20B: MAX77812 EN_CTRL %02x -> %02x, expected only bit 0 to "
			  "change\n",
			  en, now);
		ret = -71;
	}
	if (!ret && on)
		nvxnu_usleep(GPU_ENABLE_RAMP_US);
	return ret;
}

int tegra_gm20b_rail_max_uv(void)
{
	return GPU_UV_CEIL;
}

/* B01 GPU DVFS table: L4T's only B01 GPU table, GPUB01_NA_CVB_TABLE "NAPLL En - p4v3" (tegra210-dvfs.c:850-877), the
 * gpub01_cvb_dvfs_table row for speedo ID -1 (:922-929) that every B01 takes (gpu_speedo_id 1,
 * speedo-tegra210.c:189). These are voltages for the noise-aware GPCPLL. set_gpu_dvfs_data() (:1954-2092) evaluates
 * each row for the fused GPU speedo value at the gpu_scaling_cdev trip temperatures (DT nx-common.dtsi:1358-1367,
 * tegra210b01-trips.h:15-20), rounds up to the 5 mV rail step, floors at cvb_vmin 610 mV, and keeps a rate only if
 * every range fits the cap (DT nvidia,gpu-max-volt-mv 800, nx-common.dtsi:548). tegra_gm20b_dvfs_na_uv() is that
 * voltage, worst case over the trips (each range takes the max of its two trip ends, :2003-2031); the 800 mV floor
 * below 20 C is the caller's. */
static const struct {
	uint32_t khz;
	int c0, c1, c2, c3, c4, c5;
} gpu_b01_cvb[] = {
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

static const int gpu_b01_trips_c[] = {-25, 20, 30, 50, 70, 90};
#define GPU_B01_CVB_VMIN_UV 610000 /* .cvb_vmin */

static int div_round_closest(int x, int d) /* Linux DIV_ROUND_CLOSEST, d > 0 */
{
	return x > 0 ? (x + d / 2) / d : (x - d / 2) / d;
}

uint32_t tegra_gm20b_dvfs_khz(int row)
{
	return row >= 0 && row < (int)(sizeof(gpu_b01_cvb) / sizeof(gpu_b01_cvb[0]))
		       ? gpu_b01_cvb[row].khz
		       : 0;
}

int tegra_gm20b_dvfs_na_uv(uint32_t khz, int speedo)
{
	unsigned int i = 0;
	unsigned int j = 0;
	int worst = GPU_B01_CVB_VMIN_UV;

	for (i = 0; i < sizeof(gpu_b01_cvb) / sizeof(gpu_b01_cvb[0]) && gpu_b01_cvb[i].khz != khz;
	     i++)
		;
	if (i == sizeof(gpu_b01_cvb) / sizeof(gpu_b01_cvb[0]) || speedo <= 0)
		return -22;
	for (j = 0; j < sizeof(gpu_b01_trips_c) / sizeof(gpu_b01_trips_c[0]); j++) {
		/* tegra_get_cvb_voltage() + tegra_get_cvb_t_voltage() (L4T cvb.c:22-57): speedo scale 100, thermal scale 10,
		 * in uV (voltage scale 1000); then tegra_round_cvb_voltage() to the 5 mV step */
		int t = gpu_b01_trips_c[j];
		int uv = 0;
		int tv = 0;
		uv = div_round_closest(gpu_b01_cvb[i].c2 * speedo, 100);
		uv = div_round_closest((uv + gpu_b01_cvb[i].c1) * speedo, 100) + gpu_b01_cvb[i].c0;
		tv = div_round_closest(gpu_b01_cvb[i].c3 * speedo, 100) + gpu_b01_cvb[i].c4 +
		     div_round_closest(gpu_b01_cvb[i].c5 * t, 10);
		uv += div_round_closest(tv * t, 10);
		uv = uv > 0 ? (uv + MAX77812_UV_STEP - 1) / MAX77812_UV_STEP * MAX77812_UV_STEP : 0;
		if (uv > worst)
			worst = uv;
	}
	return worst;
}

int tegra_gm20b_clk_enable(int id, int on)
{
	uint32_t v = 0;
	switch (id) {
	case TEGRA_GM20B_CLK_GPU:
		switch_mmio_write32_flush(car, on ? CLK_ENB_X_SET : CLK_ENB_X_CLR, X_GPU);
		return 0;
	case TEGRA_GM20B_CLK_REF:
		if (on) /* "Only enable" (hekate clock.h:638) */
			switch_mmio_write32_flush(car, CLK_ENB_X_SET, X_PLLG_REF);
		return 0;
	case TEGRA_GM20B_CLK_PWR:
		v = switch_mmio_read32(car, PLLP_OUTC);
		v = on ? (v | PLLP_OUT5_CLKEN | PLLP_OUT5_RSTN) : (v & ~PLLP_OUT5_CLKEN);
		switch_mmio_write32_flush(car, PLLP_OUTC, v);
		nvxnu_udelay(2); /* clk_pll_out_enable() */
		return 0;
	default:
		break;
	}
	return -22;
}

unsigned long tegra_gm20b_clk_get_rate(int id)
{
	uint32_t div = 0;
	switch (id) {
	case TEGRA_GM20B_CLK_GPU:
	case TEGRA_GM20B_CLK_REF:
		return OSC_RATE;
	case TEGRA_GM20B_CLK_PWR:
		div = (switch_mmio_read32(car, PLLP_OUTC) >> PLLP_OUT5_DIV_SHIFT) & 0xff;
		return PLLP_RATE * 2 / (div + 2);
	default:
		break;
	}
	return 0;
}

int tegra_gm20b_clk_set_rate(int id, unsigned long rate)
{
	uint32_t v = 0;
	uint32_t div = 0;
	if (id != TEGRA_GM20B_CLK_PWR || !rate)
		return 0; /* gpu/ref are fixed; GPCPLL is programmed by nvkm's clk subdev */
	div = (uint32_t)((PLLP_RATE * 2 + rate - 1) /
			 rate); /* round up like TEGRA_DIVIDER_ROUND_UP */
	div = div < 2 ? 0 : div - 2;
	if (div > 0xff)
		div = 0xff;
	v = switch_mmio_read32(car, PLLP_OUTC) & ~(0xffu << PLLP_OUT5_DIV_SHIFT);
	switch_mmio_write32_flush(car, PLLP_OUTC, v | (div << PLLP_OUT5_DIV_SHIFT));
	return 0;
}

void tegra_gm20b_reset(int assert)
{
	switch_mmio_write32_flush(car, assert ? RST_DEV_X_SET : RST_DEV_X_CLR, X_GPU);
}

int tegra_gm20b_unclamp(void)
{
	switch_mmio_write32_flush(pmc, PMC_GPU_RG_CNTRL, 0);
	return 0;
}

/* Linux drivers/memory/tegra/mc.h MC_INTSTATUS 0x00, MC_ERR_STATUS 0x08, MC_ERR_ADR 0x0c. Nothing else on this kernel
 * handles the MC interrupt, so the bits read are cleared again (write 1 to clear, as tegra_mc_irq does): the next read shows
 * only what the MC refused since, and an error status latched with them is free to record the next refusal. */
void tegra_gm20b_mc_errors(uint32_t *intstatus, uint32_t *status, uint32_t *addr)
{
	*intstatus = mc ? switch_mmio_read32(mc, 0x000) : 0;
	*status = mc ? switch_mmio_read32(mc, 0x008) : 0;
	*addr = mc ? switch_mmio_read32(mc, 0x00c) : 0;
	if (*intstatus)
		switch_mmio_write32_flush(mc, 0x000, *intstatus);
}

/* hekate bdk/mem/mc_t210.h; hekate l4t.c makes carveout 2 the GPU's WPR1 (ACR's) and saves its BOM, SIZE_128KB and CFG0 for
 * the SC7 exit firmware to restore, not its CLIENT_ACCESS registers */
#define MC_SECURITY_CARVEOUT2_CFG0 0xc58
#define MC_SECURITY_CARVEOUT2_BOM 0xc5c
#define MC_SECURITY_CARVEOUT2_SIZE_128KB 0xc64
#define MC_SECURITY_CARVEOUT2_CLIENT_ACCESS2 0xc70
#define MC_SECURITY_CARVEOUT2_CLIENT_ACCESS4 0xc78

void tegra_gm20b_mc_gpu_wpr(uint32_t v[5])
{
	static const uint32_t regs[5] = {MC_SECURITY_CARVEOUT2_CFG0, MC_SECURITY_CARVEOUT2_BOM,
					 MC_SECURITY_CARVEOUT2_SIZE_128KB,
					 MC_SECURITY_CARVEOUT2_CLIENT_ACCESS2,
					 MC_SECURITY_CARVEOUT2_CLIENT_ACCESS4};
	for (int i = 0; i < 5; i++)
		v[i] = mc ? switch_mmio_read32(mc, regs[i]) : 0;
}

/* flush=1: block GPU memory traffic and wait until it drained; flush=0: unblock. No interrupt reports the drain: a
 * bounded spin like tegra_mc_flush(), which takes the status only after 5 identical reads (L4T mc.c:158-176,208-230). */
int tegra_gm20b_mc_flush(int flush)
{
	uint32_t v = switch_mmio_read32(mc, MC_HOTRESET_CTRL_1);
	uint32_t s = 0;
	int i = 0;
	int j = 0;
	if (!flush) {
		switch_mmio_write32_flush(mc, MC_HOTRESET_CTRL_1, v & ~MC_HOTRESET_GPU);
		return 0;
	}
	switch_mmio_write32_flush(mc, MC_HOTRESET_CTRL_1, v | MC_HOTRESET_GPU);
	for (i = 0; i < 1000; i++) {
		nvxnu_udelay(10);
		s = switch_mmio_read32(mc, MC_HOTRESET_STATUS_1);
		for (j = 0; j < 5 && switch_mmio_read32(mc, MC_HOTRESET_STATUS_1) == s; j++)
			;
		if (j == 5 && (s & MC_HOTRESET_GPU))
			return 0;
	}
	return -110;
}
