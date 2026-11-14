// SPDX-License-Identifier: GPL-2.0
/*
 * Tegra210B01 (Switch OLED) board pieces behind the vendored L4T DFLL driver: the device tree node, the CAR clocks
 * and reset it asks for, VDD_CPU's MAX77812 as a regulator, the fuses and chip revision the speedo code reads, and the
 * read-only checks dfll.c makes before it moves the CPU clock.
 *
 * Sources ("L4T" = CTCaer/switch-l4t-kernel-4.9 linux-dev 2d0059fd; "DT-soc" = CTCaer/l4t-hardware-soc-t210
 * l4t/l4t-r32.3.1 kernel-dts; "DT-nx" = CTCaer/switch-l4t-platform-t210-nx linux-dev kernel-dts/nx-platforms; hekate =
 * CTCaer/hekate bdk):
 *   DFLL node   DT-soc tegra210-soc/tegra210-soc-base.dtsi:2330-2347 (reg, clocks soc/ref/i2c = DFLL_SOC, DFLL_REF,
 *               I2C5, reset "dvco", clock-output-names "dfllCPU_out"), tegra210b01-soc/tegra210b01-soc-base.dtsi:96-98
 *               (compatible "nvidia,tegra210b01-dfll", "nvidia,tegra210-dfll"), DT-nx tegra210b01-nx-common.dtsi:
 *               1282-1295 (status okay, vdd-cpu-supply = MAX77812 m4vout, align-step-uv 5000, sample-rate 12500,
 *               droop-ctrl 0xf00, force-mode 1, cf 6, ci 0, cg 2, i2c-fs-rate 400000, pmic-undershoot-gb 0;
 *               dfll-max-freq-khz commented out). The OLED (fric) dts adds nothing to it. Not set, so off:
 *               nvidia,pwm-to-pmic, cg-scale, calibrate-force-vmin, defer-force-calibrate, one-shot-calibrate,
 *               idle-override, align-offset-uv, dfll-override-*.
 *   CAR 0x60006000 (hekate t210.h): CL-DVFS gate = periph clock 155 = bank W bit 27 (L4T clk-tegra-periph.c:723-724),
 *               CLK_OUT_ENB_W 0x364 / SET 0x448, RST_DEVICES_W 0x35c / CLR 0x43c (L4T clk.c:38-72);
 *               CLK_SOURCE_DFLL_REF 0x62c, _SOC 0x630 (TRM CLK_SOURCE_DVFS_REF/_SOC, 5.2.245-246): source 31:29
 *               (0 pll_p, 6 clk_m), bit 28 reserved and reset 1, 7.1 divider 7:0; reset 0xd0000000 = clk_m /1.
 *               Both 51 MHz from PLL_P and left enabled by L4T's init table (clk-tegra210.c:3746-3747), the rate
 *               clk-dfll.c insists on (REF_CLOCK_RATE).
 *               RST_DFLL_DVCO 0x2f4 bit 0 (clk-tegra210.c:266-267,4232-4256).
 *               CLK_SOURCE_I2C5 0x128: source 31:29 as above (TRM 5.2.60), 16-bit integer divider
 *               (clk-tegra-periph.c:229-234,659); I2C5 = bank H bit 15 (hekate clock.h:516), CLK_OUT_ENB_H 0x14,
 *               RST_DEVICES_H 0x8.
 *               CCLKG_BURST_POLICY 0x368 is the register L4T's cclk_g mux drives (clk-tegra-super-gen4.c:33,85-89,
 *               203-208: gen5 parents, 4-bit sources, RUN source bits 7:4); hekate and SwitchThermal use the
 *               CCLK_BURST_POLICY alias at 0x20, so the two are compared before anything is switched.
 *               PLLX_BASE 0xe0 / OSC_CTRL 0x50 / SPARE_REG0 0x55c decode as drivers/SwitchPower/SwitchClocks.h does.
 *   FUSE 0x7000f800 (hekate t210.h:65): L4T offsets are relative to +0x100 (fuse-tegra30.c:32,45-48); spare bits at
 *               0x280 (fuse-tegra30.c:195-200); readable once switch_car_fuse_readable (kits/SwitchCAR) ran.
 *   HIDREV 0x70000804 (APB_MISC 0x70000800 + 4: tegra-apbmisc.c:58-80,287); T210B01 = chip 0x21 major 2 minor 1
 *               (tegra-apbmisc.c:180-190).
 *   MAX77812 on I2C5: 0x31 if !(FUSE_RESERVED_ODM28_B01 (hekate 0x240) & 1) else 0x33 (hekate max7762x.c:98-109);
 *               M4 = vdd-cpu, M4_VOUT 0x26, 250000 + 5000 uV * code, 0xff selectors, regmap vsel (L4T
 *               max77812-regulator.c:56,97-101,386-411); constraints 250..1525 mV (DT-nx tegra210b01-nx-pmic.dtsi:
 *               77-80).
 *   I2C5 controller 0x7000d000: I2C_CNFG bit 17 MULTI_MASTER_MODE, which L4T's i2c-tegra.c sets on every T210
 *               controller (i2c-tegra.c:56,1046-1047, tegra210_i2c_hw.has_multi_master_mode = true at 2335-2344).
 */
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <tdfll_platform.h>
#include <soc/tegra/fuse.h>
#include "linux/drivers/soc/tegra/fuse/fuse.h"
#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

#define CAR_PA 0x60006000ULL
#define FUSE_PA 0x7000f800ULL
#define APBMISC_PA 0x70000800ULL
#define I2C5_PA 0x7000d000ULL
#define DFLL_PA 0x70110000ULL

#define RST_DEVICES_H 0x008
#define CLK_OUT_ENB_H 0x014
#define CCLK_BURST_POLICY 0x020
#define SUPER_CCLK_DIVIDER 0x024
#define OSC_CTRL 0x050
#define PLLX_BASE 0x0e0
#define CLK_SOURCE_I2C5 0x128
#define RST_DFLL_DVCO 0x2f4
#define RST_DEVICES_W 0x35c
#define CLK_OUT_ENB_W 0x364
#define CCLKG_BURST_POLICY 0x368
#define RST_DEV_W_CLR 0x43c
#define CLK_ENB_W_SET 0x448
#define SPARE_REG0 0x55c
#define CLK_SOURCE_DFLL_REF 0x62c
#define CLK_SOURCE_DFLL_SOC 0x630
#define H_I2C5 BIT(15)
#define W_DVFS BIT(27)
#define DVCO_RESET BIT(0)
#define PLL_ENABLE BIT(30)
#define PLL_BYPASS BIT(31)
#define PLL_LOCK BIT(27)
#define CLK_SRC_SHIFT 29 /* a CLK_SOURCE register's source field, 31:29 (TRM 5.2.60, 5.2.245-246) */
#define SRC_PLLP 0u      /* mux_pllp_clkm_idx[0] */
#define SRC_CLKM 6u      /* mux_pllp_clkm_idx[1] */
#define DVFS_MASTER_CLKEN BIT(28) /* reserved, reset 1: written as reset (TRM 1.4) */
#define DVFS_DIVISOR 0xffu
#define DFLL_SRC_51MHZ 14u /* 7.1 divider: 408 MHz * 2 / (14 + 2) = 51 MHz */
#define PLLP_RATE \
	408000000UL /* hekate pll_p, the fixed 408 MHz everything on the APB hangs off (SwitchClocks.h) */
#define CCLK_SRC_PLLP 4u
#define CCLK_SRC_PLLX 8u
#define SUPER_STATE_RUN_BIT \
	BIT(29) /* L4T clk-super.c:27-39: state field 31:28 is one-hot, RUN = bit 1 */
#define SUPER_STATE_IDLE_BIT BIT(28)

#define FUSE_CACHE 0x100
#define FUSE_SPARE 0x280
#define FUSE_OPT_SUBREVISION_OFF 0x148
#define FUSE_ODM28_B01 0x240 /* hekate offset, already including +0x100 */
#define I2C_CNFG 0x000
#define I2C_CNFG_MULTI_MASTER_MODE BIT(17)

#define PMIC_BUS 5
#define MAX77812_PHASE31 0x31
#define MAX77812_PHASE211 0x33
#define MAX77812_M4_VOUT 0x26

static volatile u8 *car, *fuse, *apbmisc, *i2c5, *dfll;
static int init_err = 1; /* 1: not done yet, the next call runs it */
static char init_why[96];
static u32 hidrev;

static int fail(char *why, size_t len, int err, const char *fmt, ...) __printf(4, 5);

static int fail(char *why, size_t len, int err, const char *fmt, ...)
{
	va_list ap; /* NOLINT(cppcoreguidelines-init-variables): va_start initializes it */

	if (why && len) {
		va_start(ap, fmt);
		vsnprintf(why, len, fmt, ap);
		va_end(ap);
	}
	return err;
}

static struct device_node max77812_m4 = {
	.name = "m4vout",
	.full_name = "/i2c@7000d000/max77812@33/regulators/m4vout",
};
static const char *const dfll_compatible[] = {"nvidia,tegra210b01-dfll", "nvidia,tegra210-dfll"};
static const char *const dfll_clock_names[] = {"soc", "ref", "i2c"};
/* <&tegra_car TEGRA210_CLK_DFLL_SOC>, <... DFLL_REF>, <... I2C5> as the names this shim registers them under */
static const char *const dfll_clocks[] = {"dfll_soc", "dfll_ref", "i2c5"};
static const char *const dfll_reset_names[] = {"dvco"};
static const char *const dfll_output_names[] = {"dfllCPU_out"};
static const char *const dfll_out_clock_name[] = {"dfll_cpu"};
static const char *const dfll_status[] = {"okay"};
static const u32 v_align_step_uv[] = {5000};
static const u32 v_sample_rate[] = {12500};
static const u32 v_droop_ctrl[] = {0x00000f00};
static const u32 v_force_mode[] = {1};
static const u32 v_cf[] = {6};
static const u32 v_ci[] = {0};
static const u32 v_cg[] = {2};
static const u32 v_i2c_fs_rate[] = {400000};
static const u32 v_pmic_undershoot_gb[] = {0};
#define PROP_U32(n, v) {.name = (n), .cells = (v), .ncells = ARRAY_SIZE(v)}
#define PROP_STR(n, v) {.name = (n), .strings = (v), .nstrings = ARRAY_SIZE(v)}
static const struct tdfll_property dfll_props[] = {
	PROP_STR("compatible", dfll_compatible),
	PROP_STR("clocks", dfll_clocks),
	PROP_STR("clock-names", dfll_clock_names),
	PROP_STR("reset-names", dfll_reset_names),
	PROP_STR("clock-output-names", dfll_output_names),
	PROP_STR("out-clock-name", dfll_out_clock_name),
	PROP_STR("status", dfll_status),
	{.name = "vdd-cpu-supply", .phandle = &max77812_m4},
	PROP_U32("nvidia,align-step-uv", v_align_step_uv),
	PROP_U32("nvidia,sample-rate", v_sample_rate),
	PROP_U32("nvidia,droop-ctrl", v_droop_ctrl),
	PROP_U32("nvidia,force-mode", v_force_mode),
	PROP_U32("nvidia,cf", v_cf),
	PROP_U32("nvidia,ci", v_ci),
	PROP_U32("nvidia,cg", v_cg),
	PROP_U32("nvidia,i2c-fs-rate", v_i2c_fs_rate),
	PROP_U32("nvidia,pmic-undershoot-gb", v_pmic_undershoot_gb),
	{.name = NULL},
};
static struct device_node dfll_node = {
	.name = "clock",
	.full_name = "/clock@70110000",
	.properties = dfll_props,
};

/* reg: control, I2C output control (same window), integrated I2C controller, LUT RAM; interrupts SPI 62 is unused */
static struct resource dfll_resources[] = {
	{.start = DFLL_PA + 0x000,
	 .end = DFLL_PA + 0x0ff,
	 .name = "control",
	 .flags = IORESOURCE_MEM},
	{.start = DFLL_PA + 0x000,
	 .end = DFLL_PA + 0x0ff,
	 .name = "i2c output",
	 .flags = IORESOURCE_MEM},
	{.start = DFLL_PA + 0x100,
	 .end = DFLL_PA + 0x1ff,
	 .name = "i2c controller",
	 .flags = IORESOURCE_MEM},
	{.start = DFLL_PA + 0x200, .end = DFLL_PA + 0x2ff, .name = "lut", .flags = IORESOURCE_MEM},
};
static struct platform_device dfll_pdev = {
	.name = "70110000.clock",
	.id = -1,
	.dev = {.init_name = "70110000.clock", .of_node = &dfll_node},
	.num_resources = ARRAY_SIZE(dfll_resources),
	.resource = dfll_resources,
};

struct platform_device *tdfll_platform_dfll_device(void)
{
	return &dfll_pdev;
}

/* the CPU OPP table's device (fcpu probe: soc->dev = get_cpu_device(0)) */
static struct device cpu0 = {.init_name = "cpu0"};

struct device *get_cpu_device(unsigned cpu)
{
	return cpu == 0 ? &cpu0 : NULL;
}

static struct regulator vdd_cpu = {
	.supply = "vdd-cpu",
	.min_uV = 250000,
	.uV_step = 5000,
	.n_voltages = 0xff,
	.constraint_min_uV = 250000,
	.constraint_max_uV = 1525000,
	.vsel_reg = MAX77812_M4_VOUT,
	.vsel_mask = 0xff,
	.client = {.name = "max77812", .dev = {.init_name = "max77812"}},
	.regmap = {.dev = &vdd_cpu.client.dev},
};

struct regulator *tdfll_platform_regulator(struct device *dev, const char *id)
{
	if (!id || strcmp(id, "vdd-cpu") || !dev ||
	    of_parse_phandle(dev->of_node, "vdd-cpu-supply", 0) != &max77812_m4)
		return NULL;
	return &vdd_cpu;
}

u8 tdfll_platform_pmic_address(void)
{
	return (u8)vdd_cpu.client.addr;
}

int tdfll_platform_vout_read(u8 *code)
{
	int err = tegra_i2c_init(PMIC_BUS);

	if (!err)
		err = tegra_i2c_read(PMIC_BUS, (u8)vdd_cpu.client.addr, MAX77812_M4_VOUT, code, 1);
	return err;
}

int tdfll_platform_vout_write(u8 code)
{
	u8 check = 0;
	int err = tegra_i2c_write(PMIC_BUS, (u8)vdd_cpu.client.addr, MAX77812_M4_VOUT, &code, 1);

	if (!err)
		err = tdfll_platform_vout_read(&check);
	if (!err && check != code)
		err = TEGRA_I2C_EIO;
	return err;
}

static void dvco_reset_set(bool assert)
{
	u32 v = switch_mmio_read32(car, RST_DFLL_DVCO);

	switch_mmio_write32_flush(car, RST_DFLL_DVCO,
				  assert ? (v | DVCO_RESET) : (v & ~DVCO_RESET));
}

static struct reset_control dvco_reset = {.name = "dvco", .set = dvco_reset_set};

struct reset_control *tdfll_platform_reset(struct device *dev, const char *id)
{
	return (dev == &dfll_pdev.dev && id && !strcmp(id, "dvco")) ? &dvco_reset : NULL;
}

/* fuses and chip revision: a subset of L4T's fuse code */
struct tegra_sku_info tegra_sku_info = {
	/* fuse-tegra.c:33-38 */
	.cpu_iddq_value = -ENOTSUPP,
	.gpu_iddq_value = -ENOTSUPP,
	.soc_iddq_value = -ENOTSUPP,
	.speedo_rev = -ENOTSUPP,
};

u32 tegra_fuse_read_early(unsigned int offset)
{
	return switch_mmio_read32(fuse,
				  FUSE_CACHE + offset); /* fuse-tegra30.c tegra30_fuse_read_early */
}

bool tegra_fuse_read_spare(unsigned int spare)
{
	return tegra_fuse_read_early(FUSE_SPARE + spare * 4) &
	       1; /* fuse-tegra.c tegra_fuse_read_spare */
}

u32 tegra_read_chipid(void)
{
	return switch_mmio_read32(apbmisc, 4);
}

/* tegra_fuse_get_subrevision's letter (1 p, 2 q, 3 r), 0 for none. */
static char subrev_type(u32 subrev)
{
	switch (subrev) {
	case 1:
		return 'p';
	case 2:
		return 'q';
	case 3:
		return 'r';
	default:
		return 0;
	}
}

/* tegra-apbmisc.c tegra_init_revision (172-266), for the chips L4T 4.9 lists there that matter on this SoC family */
static void init_revision(void)
{
	static const struct {
		u32 chipid, major, minor;
		char sub_type;
		enum tegra_revision revision, id_and_rev;
	} revs[] = {
		{TEGRA210, 1, 1, 0, TEGRA_REVISION_A01, TEGRA210_REVISION_A01},
		{TEGRA210, 1, 1, 'q', TEGRA_REVISION_A01q, TEGRA210_REVISION_A01q},
		{TEGRA210, 1, 2, 0, TEGRA_REVISION_A02, TEGRA210_REVISION_A02},
		{TEGRA210B01, 2, 1, 0, TEGRA_REVISION_A01, TEGRA210B01_REVISION_A01},
	};
	enum tegra_revision revision = TEGRA_REVISION_UNKNOWN;
	enum tegra_revision id_and_rev = TEGRA_REVISION_UNKNOWN;
	u32 chipid = 0;
	u32 major = 0;
	u32 minor = 0;
	u32 subrev = 0;
	char sub_type = 0;
	unsigned int i = 0;
	unsigned int pass = 0;

	hidrev = tegra_read_chipid();
	chipid = tegra_hidrev_get_chipid(hidrev);
	major = tegra_hidrev_get_majorrev(hidrev);
	minor = tegra_hidrev_get_minorrev(hidrev);
	if (major) {
		subrev = switch_mmio_read32(fuse, FUSE_CACHE + FUSE_OPT_SUBREVISION_OFF) &
			 0xf; /* tegra_fuse_get_subrevision */
		sub_type = subrev_type(subrev);
		for (pass = sub_type ? 0 : 1; pass < 2 && revision == TEGRA_REVISION_UNKNOWN;
		     pass++) {
			for (i = 0; i < ARRAY_SIZE(revs); i++) {
				if (revs[i].chipid == chipid && revs[i].major == major &&
				    revs[i].minor == minor &&
				    (pass || revs[i].sub_type == sub_type)) {
					revision = revs[i].revision;
					id_and_rev = revs[i].id_and_rev;
					break;
				}
			}
		}
	}
	tegra_sku_info.revision = revision;
	tegra_sku_info.id_and_rev = id_and_rev;
	tegra_sku_info.sku_id = (int)tegra_fuse_read_early(FUSE_SKU_INFO);
}

void tdfll_platform_describe_chip(char *buf, size_t len)
{
	const struct tegra_sku_info *s = &tegra_sku_info;

	snprintf(buf, len, "SKU %#x%s, CPU speedo %d/%d process %d (fuse rev %d, IDDQ %d)",
		 s->sku_id,
		 s->id_and_rev == TEGRA210B01_REVISION_A01 ? " T210B01 A01" : " not T210B01",
		 s->cpu_speedo_id, s->cpu_speedo_value, s->cpu_process_id, s->speedo_rev,
		 s->cpu_iddq_value);
}

static unsigned long pllp_recalc(struct clk_hw *hw, unsigned long parent_rate)
{
	return PLLP_RATE;
}

static const struct clk_ops pllp_ops = {.recalc_rate = pllp_recalc};
static const struct clk_init_data pllp_init = {.name = "pll_p", .ops = &pllp_ops};
static struct clk_hw pllp_hw = {.init = &pllp_init};

/* PLLX as the caller leaves it: m/n/p from PLLX_BASE over pll_ref (SwitchClocks.h refKHz/qlinDivider/pllKHz) */
static unsigned long pllx_recalc(struct clk_hw *hw, unsigned long parent_rate)
{
	static const u8 qlin[15] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32};
	u32 base = switch_mmio_read32(car, PLLX_BASE);
	u32 m = base & 0xff;
	u32 n = (base >> 8) & 0xff;
	u32 pcode = (base >> 20) & 0x1f;
	unsigned long ref = 38400000UL >> ((switch_mmio_read32(car, OSC_CTRL) >> 26) & 3);

	if (!(base & PLL_ENABLE))
		return 0;
	if (base & PLL_BYPASS)
		return ref;
	if (!m || pcode >= ARRAY_SIZE(qlin))
		return 0;
	return (unsigned long)((u64)ref * n / m / qlin[pcode]);
}

static const struct clk_ops pllx_ops = {.recalc_rate = pllx_recalc};
static const struct clk_init_data pllx_init = {.name = "pll_x", .ops = &pllx_ops};
static struct clk_hw pllx_hw = {.init = &pllx_init};

bool tdfll_platform_pllx_locked(void)
{
	return (switch_mmio_read32(car, PLLX_BASE) & (PLL_ENABLE | PLL_BYPASS | PLL_LOCK)) ==
	       (PLL_ENABLE | PLL_LOCK);
}

static unsigned long clkm_rate(void)
{
	return 38400000UL / (((switch_mmio_read32(car, SPARE_REG0) >> 2) & 3) + 1);
}

/* dfll_soc / dfll_ref: one CAR source each, one shared gate (W27, refcounted as L4T's periph_clk_enb_refcnt) */
struct cl_dvfs_clk {
	struct clk_hw hw;
	u32 source;
};

static unsigned int cl_dvfs_gate_refcnt;

static void cl_dvfs_gate_on(void)
{
	switch_mmio_write32_flush(car, CLK_ENB_W_SET, W_DVFS);
	udelay(2);
}

/* Both sources to PLL_P / 8, divisor first, each step 2 us (TRM 5.1.7, "Method 2": from the reset value's clk_m / 1 a
 * source-first move would run the module at PLL_P / 1 in between). */
static void cl_dvfs_sources(void)
{
	static const u32 sources[] = {CLK_SOURCE_DFLL_SOC, CLK_SOURCE_DFLL_REF};
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(sources); i++) {
		u32 v = switch_mmio_read32(car, sources[i]);

		switch_mmio_write32_flush(car, sources[i], (v & ~DVFS_DIVISOR) | DFLL_SRC_51MHZ);
		udelay(2);
		switch_mmio_write32_flush(car, sources[i],
					  (SRC_PLLP << CLK_SRC_SHIFT) | DVFS_MASTER_CLKEN |
						  DFLL_SRC_51MHZ);
		udelay(2);
	}
}

/* The CL-DVFS module (bank W bit 27) up with both sources at 51 MHz, as TRM 5.1.7 sets up a device: in reset, its
 * clock on, the sources, then out of reset. TRM 35.4: CL-DVFS arbitrates PWR_I2C between its own master and I2C5, and
 * I2C5 hangs while CL-DVFS is out of reset with its clock stopped, so the clock goes on before anything else and
 * nothing here stops it. -EBUSY for a module already out of reset whose DFLL runs (its sources change only while it
 * is idle). */
static int cl_dvfs_bring_up(char *why, size_t len)
{
	cl_dvfs_gate_on();
	if (switch_mmio_read32(car, RST_DEVICES_W) & W_DVFS) {
		cl_dvfs_sources();
		switch_mmio_write32_flush(car, RST_DEV_W_CLR, W_DVFS);
		udelay(2);
		return 0;
	}
	if (switch_mmio_read32(dfll, 0x00) & 3) {
		return fail(why, len, -EBUSY, "the DFLL is already running (DFLL_CTRL %#x)",
			    switch_mmio_read32(dfll, 0x00));
	}
	cl_dvfs_sources();
	return 0;
}

/* Counts only: the gate went on in cl_dvfs_bring_up, before the clock tree exists, and L4T's init table holds a
 * reference on both for good (clk-tegra210.c:3746-3747). Never gated either, so CL-DVFS is never out of reset with its
 * clock stopped (TRM 35.4); hekate enables it "to avoid issues with I2C5 sharing" (hw_init.c:413-414). */
static int cl_dvfs_enable(struct clk_hw *hw)
{
	cl_dvfs_gate_refcnt++;
	return 0;
}

static void cl_dvfs_disable(struct clk_hw *hw)
{
	if (!WARN_ON(cl_dvfs_gate_refcnt == 0))
		cl_dvfs_gate_refcnt--;
}

/* The rate of a PLLP/CLK_M source field (31:29); 0 for the inputs we do not use. */
static unsigned long pllp_clkm_rate(u32 src)
{
	if (src == SRC_PLLP)
		return PLLP_RATE;
	return src == SRC_CLKM ? clkm_rate() : 0;
}

static unsigned long cl_dvfs_recalc(struct clk_hw *hw, unsigned long parent_rate)
{
	u32 v = switch_mmio_read32(car, container_of(hw, struct cl_dvfs_clk, hw)->source);
	u32 src = v >> CLK_SRC_SHIFT;
	unsigned long parent = pllp_clkm_rate(src);

	return (unsigned long)((u64)parent * 2 / ((v & 0xff) + 2));
}

static const struct clk_ops cl_dvfs_ops = {
	.enable = cl_dvfs_enable,
	.disable = cl_dvfs_disable,
	.recalc_rate = cl_dvfs_recalc,
};
static const struct clk_init_data dfll_soc_init = {.name = "dfll_soc", .ops = &cl_dvfs_ops};
static const struct clk_init_data dfll_ref_init = {.name = "dfll_ref", .ops = &cl_dvfs_ops};
static struct cl_dvfs_clk dfll_soc_clk = {.hw = {.init = &dfll_soc_init},
					  .source = CLK_SOURCE_DFLL_SOC};
static struct cl_dvfs_clk dfll_ref_clk = {.hw = {.init = &dfll_ref_init},
					  .source = CLK_SOURCE_DFLL_REF};

/* I2C5 belongs to TegraI2C, which adopts it as U-Boot left it and never resets or re-clocks it: the DFLL only needs it
 * running (its integrated I2C master divides this clock, clk-dfll.c dfll_init_i2c_if) and never gates it. */
static bool i2c5_running(void)
{
	return ((switch_mmio_read32(car, CLK_OUT_ENB_H) & H_I2C5) &&
		!(switch_mmio_read32(car, RST_DEVICES_H) & H_I2C5)) != 0;
}

static int i2c5_enable(struct clk_hw *hw)
{
	if (i2c5_running())
		return 0;
	printk("I2C5 is not clocked (CLK_OUT_ENB_H %#x, RST_DEVICES_H %#x)\n",
	       switch_mmio_read32(car, CLK_OUT_ENB_H), switch_mmio_read32(car, RST_DEVICES_H));
	return -ENODEV;
}

static unsigned long i2c5_recalc(struct clk_hw *hw, unsigned long parent_rate)
{
	u32 v = switch_mmio_read32(car, CLK_SOURCE_I2C5);
	u32 src = v >> CLK_SRC_SHIFT;
	unsigned long parent = pllp_clkm_rate(src);

	return parent / ((v & 0xffff) + 1);
}

static const struct clk_ops i2c5_ops = {.enable = i2c5_enable, .recalc_rate = i2c5_recalc};
static const struct clk_init_data i2c5_init = {.name = "i2c5", .ops = &i2c5_ops};
static struct clk_hw i2c5_hw = {.init = &i2c5_init};

/* cclk_g: L4T clk-super.c clk_super_get_parent / clk_super_set_parent (43-125) for the gen5 mux (width 4, no
 * LP divide-by-2), on CCLKG_BURST_POLICY. Only the source of the current state (RUN, or IDLE) changes. */
static const char *const cclk_g_parents[] = {
	"clk_m", "unused", "clk_32k", "unused", "pll_p",  "pll_p_out4", "unused", "unused",
	"pll_x", "unused", "unused",  "unused", "unused", "unused",     "unused", "dfllCPU_out"};

static u8 cclk_g_get_parent(struct clk_hw *hw)
{
	u32 val = switch_mmio_read32(car, CCLKG_BURST_POLICY);
	u32 state = val & (0xfu << 28);

	BUG_ON(state != SUPER_STATE_RUN_BIT && state != SUPER_STATE_IDLE_BIT);
	return (val >> (state == SUPER_STATE_IDLE_BIT ? 0u : 4u)) & 0xf;
}

static int cclk_g_set_parent(struct clk_hw *hw, u8 index)
{
	u32 val = switch_mmio_read32(car, CCLKG_BURST_POLICY);
	u32 state = val & (0xfu << 28);
	unsigned int shift = state == SUPER_STATE_IDLE_BIT ? 0 : 4;

	BUG_ON(state != SUPER_STATE_RUN_BIT && state != SUPER_STATE_IDLE_BIT);
	val &= ~(0xfu << shift);
	val |= (u32)(index & 0xf) << shift;
	switch_mmio_write32_flush(car, CCLKG_BURST_POLICY, val);
	udelay(2);
	return 0;
}

static const struct clk_ops cclk_g_ops = {.get_parent = cclk_g_get_parent,
					  .set_parent = cclk_g_set_parent};
static const struct clk_init_data cclk_g_init = {
	.name = "cclk_g",
	.ops = &cclk_g_ops,
	.parent_names = cclk_g_parents,
	.num_parents = ARRAY_SIZE(cclk_g_parents),
	.flags = CLK_SET_RATE_PARENT,
};
static struct clk_hw cclk_g_hw = {.init = &cclk_g_init};

int tdfll_platform_cpu_on_pllx(char *why, size_t len)
{
	u32 policy = switch_mmio_read32(car, CCLKG_BURST_POLICY);
	u32 alias = switch_mmio_read32(car, CCLK_BURST_POLICY);
	u32 div = switch_mmio_read32(car, SUPER_CCLK_DIVIDER);

	if (policy != alias) {
		return fail(why, len, -ENODEV, "CCLKG_BURST_POLICY %#x != CCLK_BURST_POLICY %#x",
			    policy, alias);
	}
	if ((policy & (0xfu << 28)) != SUPER_STATE_RUN_BIT) {
		return fail(why, len, -EBUSY, "the CPU super clock is not in the RUN state (%#x)",
			    policy);
	}
	if (((policy >> 4) & 0xf) != CCLK_SRC_PLLX) {
		return fail(why, len, -EBUSY,
			    "the CPU does not run from PLLX (CCLKG_BURST_POLICY %#x)", policy);
	}
	if (!tdfll_platform_pllx_locked()) {
		return fail(why, len, -EBUSY, "PLLX is not locked (PLLX_BASE %#x)",
			    switch_mmio_read32(car, PLLX_BASE));
	}
	if ((div & BIT(31)) && ((div >> 8) & 0xff) != (div & 0xff)) {
		return fail(why, len, -EBUSY, "the CCLK divider is in use (SUPER_CCLK_DIVIDER %#x)",
			    div);
	}
	return 0;
}

int tdfll_platform_i2c5_shareable(char *why, size_t len)
{
	if (!i2c5_running())
		return fail(why, len, -ENODEV, "I2C5 is not clocked");
	if (!(switch_mmio_read32(i2c5, I2C_CNFG) & I2C_CNFG_MULTI_MASTER_MODE)) {
		return fail(why, len, -EBUSY,
			    "I2C5 is not in multi-master mode (I2C_CNFG %#x), the DFLL would "
			    "collide with it",
			    switch_mmio_read32(i2c5, I2C_CNFG));
	}
	return 0;
}

u32 tdfll_platform_dfll_readl(u32 offset)
{
	return switch_mmio_read32(dfll, offset);
}

static int map_blocks(char *why, size_t len)
{
	if (!car)
		car = (volatile u8 *)switch_linux_ioremap(CAR_PA, 0x1000, 0);
	if (!fuse)
		fuse = (volatile u8 *)switch_linux_ioremap(FUSE_PA, 0x400, 0);
	if (!apbmisc)
		apbmisc = (volatile u8 *)switch_linux_ioremap(APBMISC_PA, 0x100, 0);
	if (!i2c5)
		i2c5 = (volatile u8 *)switch_linux_ioremap(I2C5_PA, 0x100, 0);
	if (!dfll)
		dfll = (volatile u8 *)switch_linux_ioremap(DFLL_PA, 0x400, 0);
	if (!car || !fuse || !apbmisc || !i2c5 || !dfll)
		return fail(why, len, -ENOMEM, "cannot map CAR/FUSE/APB_MISC/I2C5/DFLL");
	return 0;
}

/* L4T's clock init table, once: the clock tree registered, dfll_soc/dfll_ref and cclk_g held for good. */
static int register_clocks(char *why, size_t len)
{
	struct clk_hw *hws[] = {&pllp_hw,         &pllx_hw, &dfll_soc_clk.hw,
				&dfll_ref_clk.hw, &i2c5_hw, &cclk_g_hw};
	static bool held;
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(hws); i++) {
		if (!hws[i]->clk && IS_ERR(clk_register(NULL, hws[i]))) {
			return fail(why, len, -ENOMEM, "cannot register clock %s",
				    hws[i]->init->name);
		}
	}
	if (held)
		return 0;
	/* the init-table references: cl_dvfs's taken in platform_init (counted once more for dfll_ref), cclk_g running */
	if (clk_prepare_enable(cclk_g_hw.clk)) {
		return fail(why, len, -EAGAIN,
			    "cclk_g could not be enabled (CCLKG_BURST_POLICY %#x)",
			    switch_mmio_read32(car, CCLKG_BURST_POLICY));
	}
	cl_dvfs_gate_refcnt++;
	held = true;
	return 0;
}

/* Bring-up: no CPU clock or VDD_CPU change. -ENODEV only for what no retry changes: this chip's revision and its CPU
 * speedo fuses. Every other failure leaves what it did in place for the next call to go on from. */
static int platform_init(char *why, size_t len)
{
	static bool gate_held;
	u8 vout = 0;
	int err = map_blocks(why, len);

	if (err)
		return err;

	/* fuses: identity, speedo (vendored speedo-tegra210.c, as fuse-tegra30.c tegra30_fuse_init runs it) */
	switch_car_fuse_readable();
	init_revision();
	if (tegra_sku_info.id_and_rev != TEGRA210B01_REVISION_A01)
		return fail(why, len, -ENODEV, "not a T210B01 A01 (HIDREV %#x)", hidrev);
	tegra210_init_speedo_data(&tegra_sku_info);
	if (tegra_sku_info.cpu_speedo_value <= 0 || tegra_sku_info.cpu_process_id < 0) {
		return fail(why, len, -ENODEV, "CPU speedo not fused (value %d, process %d)",
			    tegra_sku_info.cpu_speedo_value, tegra_sku_info.cpu_process_id);
	}
	vdd_cpu.client.addr = (switch_mmio_read32(fuse, FUSE_ODM28_B01) & 1) ? MAX77812_PHASE211
									     : MAX77812_PHASE31;

	/* L4T init table: dfll_soc and dfll_ref from PLL_P at 51 MHz, enabled for good, before the first I2C5 transfer
	 * here (TRM 35.4). The table's reference is taken once; the module is brought up on every call, as an SC7 since the
	 * last one reset it. */
	err = cl_dvfs_bring_up(why, len);
	if (err)
		return err;
	if (!gate_held)
		cl_dvfs_gate_refcnt++;
	gate_held = true;

	/* VDD_CPU's PMIC must answer where the fuse says it is: the DFLL will address it blindly. Nothing above has
	 * changed any state the next call does not set again. */
	err = tdfll_platform_vout_read(&vout);
	if (err) {
		return fail(why, len, -EAGAIN, "MAX77812 at I2C5 %#x does not answer (TegraI2C %d)",
			    vdd_cpu.client.addr, err);
	}

	/* the DFLL's I2C master divides I2C5's clock: dfll_init_i2c_if BUG()s on a zero divisor */
	if (!i2c5_running() || !i2c5_recalc(&i2c5_hw, 0)) {
		return fail(why, len, -EAGAIN,
			    "I2C5 clock is off or unreadable (CLK_SOURCE_I2C5 %#x)",
			    switch_mmio_read32(car, CLK_SOURCE_I2C5));
	}

	/* L4T's cclk_g mux BUG()s on a super clock state other than RUN or IDLE: refuse here instead of panicking */
	if ((switch_mmio_read32(car, CCLKG_BURST_POLICY) & (0xfu << 28)) != SUPER_STATE_RUN_BIT &&
	    (switch_mmio_read32(car, CCLKG_BURST_POLICY) & (0xfu << 28)) != SUPER_STATE_IDLE_BIT) {
		return fail(why, len, -EBUSY,
			    "CCLKG_BURST_POLICY %#x is in neither RUN nor IDLE state",
			    switch_mmio_read32(car, CCLKG_BURST_POLICY));
	}
	return register_clocks(why, len);
}

/* SC7 reset the CAR: the DFLL's gate, its reset and its two 51 MHz sources come back at their reset values (clock off,
 * in reset, clk_m / 1) while the clock tree's counts still say they are on. I2C5 must be running again (TegraI2C's
 * resume) before this. */
int tdfll_platform_resume(char *why, size_t len)
{
	int err = 0;

	if (init_err)
		return 0;
	err = cl_dvfs_bring_up(why, len);
	if (err)
		return err;
	if (!i2c5_running() || !i2c5_recalc(&i2c5_hw, 0)) {
		return fail(
			why, len, -ENODEV,
			"I2C5 is not clocked after SC7 (CLK_SOURCE_I2C5 %#x, CLK_OUT_ENB_H %#x)",
			switch_mmio_read32(car, CLK_SOURCE_I2C5),
			switch_mmio_read32(car, CLK_OUT_ENB_H));
	}
	return 0;
}

int tdfll_platform_init(char *why, size_t len)
{
	int err = init_err;

	if (err == 1) {
		static char printed[sizeof(init_why)];

		err = platform_init(init_why, sizeof(init_why));
		if (err && strcmp(init_why, printed)) {
			printk("%s\n", init_why);
			memcpy(printed, init_why, sizeof(printed));
		}
		init_err = err && err != -ENODEV ? 1 : err;
	}
	if (err)
		fail(why, len, err, "%s", init_why);
	return err;
}
