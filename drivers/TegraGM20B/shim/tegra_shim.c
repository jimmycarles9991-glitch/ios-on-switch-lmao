// SPDX-License-Identifier: MIT
/*
 * Linux clk / regulator / reset / PMC / fuse API as used by nvkm's engine/device/tegra.c and its clk and volt subdevs,
 * backed by platform.c. tegra.c's power-up and power-down are platform.c's rail ungate and gate (Tegra X1 TRM
 * 12.2.7.17), so only the lookups, rates and voltages are here. Clock names map as nouveau "gpu"/"ref"/"pwr" = mainline
 * tegra210.dtsi gpu@57000000 TEGRA210_CLK_GPU / PLL_G_REF / PLL_P_OUT5 = L4T "gpu_gate"/"gpu_ref"/"pll_p_out5"
 * (l4t49 clk-tegra210.c:2832-2835).
 */
#include <nvif/os.h>

struct clk {
	int id;
};

struct regulator {
	int dummy;
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

struct tegra_pmc *devm_tegra_pmc_get(struct device *dev)
{
	(void)dev;
	return &pmc;
}

int tegra_fuse_readl(unsigned long offset, u32 *value)
{
	*value = tegra_gm20b_fuse_read((u32)offset);
	return 0;
}
