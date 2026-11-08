// SPDX-License-Identifier: MIT
/*
 * Linux clk / regulator / reset / PMC / fuse API as used by nvkm's engine/device/tegra.c, backed by
 * platform.c. tegra.c's nvkm_device_tegra_power_up() calls, in order: regulator_enable(vdd),
 * clk_prepare_enable(gpu, ref, pwr), clk_set_rate(pwr, 204 MHz), udelay(10), reset_control_assert,
 * udelay(10), tegra_pmc_powergate_remove_clamping, udelay(10), reset_control_deassert, udelay(10).
 * That is nvgpu's gm20b_tegra_unrailgate() (os/linux/platform_gk20a_tegra.c:368-447) except for
 * nvgpu's last steps, which reset_control_deassert() below adds: gpu_gate clock off around the
 * deassert (434-436), then MC flush / flush-done with 10 us gaps (438-445). Clock names map as
 * nouveau "gpu"/"ref"/"pwr" = mainline tegra210.dtsi gpu@57000000 TEGRA210_CLK_GPU / PLL_G_REF /
 * PLL_P_OUT5 = L4T "gpu_gate"/"gpu_ref"/"pll_p_out5" (l4t49 clk-tegra210.c:2832-2835).
 * regulator_disable() (nouveau power_down, after the clocks are gated) also blocks MC, clamps and
 * asserts reset before the rail drops (platform.c tegra_gm20b_rail_enable(0)), as nvgpu's railgate.
 */
#include <nvif/os.h>

struct clk {
	int id;
};

struct regulator {
	int enabled;
};

struct reset_control {
	int dummy;
};

struct tegra_pmc {
	int dummy;
};

static struct clk clocks[] = {{TEGRA_GM20B_CLK_GPU}, {TEGRA_GM20B_CLK_REF}, {TEGRA_GM20B_CLK_PWR}};
static struct regulator vdd_gpu;
static struct reset_control gpu_rst;
static struct tegra_pmc pmc;

/* VDD_GPU when tegra.c enables the rail: DT tegra210b01-nx-pmic.dtsi:45 regulator-init-microvolt and nvgpu
 * clk_gm20b.c:51 BOOT_GPU_UV_C1; covers 76.8 MHz (610 mV) and L4T's 800 mV floor below 20 C. platform.c clamps every
 * request (nvkm volt) to [610, 800] mV. */
#define GPU_BOOT_UV 800000

struct clk *devm_clk_get(struct device *dev, const char *id)
{
	(void)dev;
	if (!strcmp(id, "gpu"))
		return &clocks[0];
	if (!strcmp(id, "ref"))
		return &clocks[1];
	if (!strcmp(id, "pwr"))
		return &clocks[2];
	return ERR_PTR(-ENOENT);
}

int clk_prepare_enable(struct clk *c)
{
	const char *name = "pll_p_out5";

	if (!c)
		return 0;
	/* pll_p_out5 gets its 204 MHz divider before it is ungated (tegra.c sets it only afterwards), as L4T's init table
	 * (l4t49 clk-tegra210.c:3784) and nvgpu's probe (platform_gk20a_tegra.c:462,498) do before unrailgate enables it */
	if (c->id == TEGRA_GM20B_CLK_PWR)
		tegra_gm20b_clk_set_rate(c->id, 204000000);
	if (c->id == TEGRA_GM20B_CLK_GPU) {
		name = "gpu";
	} else if (c->id == TEGRA_GM20B_CLK_REF) {
		name = "pllg_ref";
	}
	nvxnu_step("tegra-gm20b: M1 step: enable clock %s\n", name);
	return tegra_gm20b_clk_enable(c->id, 1);
}

void clk_disable_unprepare(struct clk *c)
{
	if (c)
		tegra_gm20b_clk_enable(c->id, 0);
}

int clk_enable(struct clk *c)
{
	return clk_prepare_enable(c);
}

void clk_disable(struct clk *c)
{
	clk_disable_unprepare(c);
}

unsigned long clk_get_rate(struct clk *c)
{
	return c ? tegra_gm20b_clk_get_rate(c->id) : 0;
}

int clk_set_rate(struct clk *c, unsigned long rate)
{
	return c ? tegra_gm20b_clk_set_rate(c->id, rate) : 0;
}

long clk_round_rate(struct clk *c, unsigned long rate)
{
	(void)c;
	return (long)rate;
}

struct regulator *devm_regulator_get(struct device *dev, const char *id)
{
	(void)dev;
	return strcmp(id, "vdd") ? ERR_PTR(-ENOENT) : &vdd_gpu;
}

int regulator_enable(struct regulator *r)
{
	const long long uv = GPU_BOOT_UV;
	int ret = 0;
	nvxnu_step("tegra-gm20b: M1 step: VDD_GPU: read MAX77812 over I2C5\n");
	ret = tegra_gm20b_rail_set_uv((int)uv); /* voltage first, as hekate does, then enable */
	if (!ret)
		ret = tegra_gm20b_rail_enable(1);
	if (ret) {
		pr_err("VDD_GPU (MAX77812 M1) enable failed: %d\n", ret);
		return -EIO;
	}
	r->enabled = 1;
	return 0;
}

int regulator_disable(struct regulator *r)
{
	r->enabled = 0;
	return tegra_gm20b_rail_enable(0) ? -EIO : 0;
}

int regulator_is_enabled(struct regulator *r)
{
	return r->enabled;
}

int regulator_get_voltage(struct regulator *r)
{
	int uv = 0;
	(void)r;
	return tegra_gm20b_rail_get_uv(&uv) ? -EIO : uv;
}

int regulator_set_voltage(struct regulator *r, int min_uv, int max_uv)
{
	(void)r;
	(void)max_uv;
	return tegra_gm20b_rail_set_uv(min_uv) ? -EIO : 0;
}

struct reset_control *devm_reset_control_get(struct device *dev, const char *id)
{
	(void)dev;
	return strcmp(id, "gpu") ? ERR_PTR(-ENOENT) : &gpu_rst;
}

int reset_control_assert(struct reset_control *r)
{
	(void)r;
	nvxnu_step("tegra-gm20b: M1 step: assert GPU reset\n");
	tegra_gm20b_reset(1);
	return 0;
}

int reset_control_deassert(struct reset_control *r)
{
	int ret = 0;
	(void)r;
	nvxnu_step("tegra-gm20b: M1 step: deassert GPU reset (gpu clock gated around it)\n");
	tegra_gm20b_clk_enable(TEGRA_GM20B_CLK_GPU, 0);
	tegra_gm20b_reset(0);
	tegra_gm20b_clk_enable(TEGRA_GM20B_CLK_GPU, 1);
	nvxnu_step("tegra-gm20b: M1 step: MC flush/unflush GPU client\n");
	ret = tegra_gm20b_mc_flush(1);
	udelay(10);
	tegra_gm20b_mc_flush(0);
	udelay(10);
	if (ret)
		pr_err("MC GPU flush timed out\n");
	nvxnu_step("tegra-gm20b: M1 step: powered; next the first GPU register read (BOOT_0 in "
		   "nvkm_device_ctor)\n");
	return 0;
}

int reset_control_reset(struct reset_control *r)
{
	reset_control_assert(r);
	udelay(10);
	return reset_control_deassert(r);
}

struct tegra_pmc *devm_tegra_pmc_get(struct device *dev)
{
	(void)dev;
	return &pmc;
}

int tegra_pmc_powergate_remove_clamping(struct tegra_pmc *p, unsigned int id)
{
	(void)p;
	nvxnu_step("tegra-gm20b: M1 step: remove GPU clamp\n");
	return id == TEGRA_POWERGATE_3D ? tegra_gm20b_unclamp() : -EINVAL;
}

int tegra_powergate_remove_clamping(unsigned int id)
{
	return tegra_pmc_powergate_remove_clamping(&pmc, id);
}

int tegra_fuse_readl(unsigned long offset, u32 *value)
{
	*value = tegra_gm20b_fuse_read((u32)offset);
	return 0;
}
