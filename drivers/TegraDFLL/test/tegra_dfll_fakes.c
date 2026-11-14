// SPDX-License-Identifier: GPL-2.0
// The vendored L4T code TegraDFLL calls (speedo, the CVB tables, the fcpu probe, thermal indices), as tegra_dfll_fake.h
// sets it, built with the driver's own Linux prelude so its names are renamed as the driver's are.
#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <soc/tegra/cvb.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/tegra-dfll.h>
#include "../linux/drivers/soc/tegra/fuse/fuse.h"
#include "tegra_dfll_fake.h"

void tegra210_init_speedo_data(struct tegra_sku_info *sku_info)
{
	sku_info->cpu_speedo_id = tegra_dfll_fake.speedo_id;
	sku_info->cpu_speedo_value = tegra_dfll_fake.speedo_value;
	sku_info->cpu_process_id = tegra_dfll_fake.process_id;
}

struct cvb_table tegra210b01_cpu_cvb_tables[4] = {
	{.speedo_id = 0, .process_id = 0, .cvb_version = "fake"},
	{.speedo_id = 1, .process_id = -1, .cvb_version = "fake"},
	{.speedo_id = 2, .process_id = -1, .cvb_version = "fake"},
	{.speedo_id = 3, .process_id = -1, .cvb_version = "fake"},
};

static char fake_td;
static const struct clk_ops dfll_out_ops;
static const struct clk_init_data dfll_out_init = {.name = "dfllCPU_out", .ops = &dfll_out_ops};
static struct clk_hw dfll_out_hw = {.init = &dfll_out_init};

static int fake_probe(struct platform_device *pdev)
{
	tegra_dfll_fake.probes++;
	if (tegra_dfll_fake.probe_answer)
		return tegra_dfll_fake.probe_answer;
	if (IS_ERR(clk_register(NULL, &dfll_out_hw)))
		return -ENOMEM;
	platform_set_drvdata(pdev, &fake_td);
	return 0;
}

static struct platform_driver fake_driver = {.probe = fake_probe,
					     .driver = {.name = "tegra124-dfll"}};
builtin_platform_driver(fake_driver);

int tegra_dfll_update_thermal_index(struct tegra_dfll *td, enum tegra_dfll_thermal_type type,
				    unsigned long new_index)
{
	return 0;
}

void tegra_dfll_resume(struct platform_device *pdev, bool on_dfll)
{
}

const char *tegra_dfll_get_cvb_version(void)
{
	return "fake";
}

u32 tegra_dfll_get_min_millivolts(void)
{
	return 620;
}

u32 tegra_dfll_get_thermal_floor_mv(void)
{
	return 620;
}

u32 tegra_dfll_get_thermal_cap_mv(void)
{
	return 1120;
}
