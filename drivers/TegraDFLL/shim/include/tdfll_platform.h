/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch: the seam between the generic Linux shim (shim/linux.c) and the T210B01 board pieces (platform.c), and what
 * dfll.c needs from both. Linux-side only (force-included prelude assumed).
 */
#ifndef TDFLL_PLATFORM_H
#define TDFLL_PLATFORM_H
#include <linux/clk-provider.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

/*
 * A regulator as the DFLL sees it: a linear selector map with constraints, its vsel register, and the I2C client that
 * owns it (Linux 4.9 regulator_desc + regulation_constraints + the max77812 i2c_client, flattened).
 */
struct regulator {
	const char *supply;
	int min_uV, uV_step, n_voltages;
	int constraint_min_uV, constraint_max_uV;
	unsigned vsel_reg, vsel_mask;
	struct i2c_client client;
	struct regmap regmap;
};

struct reset_control {
	const char *name;
	void (*set)(bool assert);
};

/* platform.c: lookups behind devm_regulator_get / devm_reset_control_get (NULL if this board has no such supply) */
struct regulator *tdfll_platform_regulator(struct device *dev, const char *id);
struct reset_control *tdfll_platform_reset(struct device *dev, const char *id);

/*
 * platform.c, for dfll.c. tdfll_platform_init() maps the blocks, identifies the chip and reads its fuses (L4T
 * speedo-tegra210.c), finds VDD_CPU's MAX77812, programs the DFLL reference clocks as L4T's clock init table does and
 * registers the clock tree; it never touches the CPU clock or VDD_CPU. Idempotent: -ENODEV (the chip's revision or
 * its CPU speedo fuses) is remembered, any other failure is tried again by the next call. Each returns 0 or a negative
 * errno with a one-line reason in `why`.
 */
int tdfll_platform_init(char *why, size_t len);
/* After SC7, before tegra_dfll_resume(): the DFLL's CAR gate, reset and sources as init left them. 0 if never inited. */
int tdfll_platform_resume(char *why, size_t len);
struct platform_device *tdfll_platform_dfll_device(void);
/* The CPU runs from PLLX in the RUN state through CCLKG_BURST_POLICY, undivided (what enable moves away from). */
int tdfll_platform_cpu_on_pllx(char *why, size_t len);
/* PLLX is enabled, not bypassed and locked (what disable moves back to). */
bool tdfll_platform_pllx_locked(void);
/* I2C5 is clocked, out of reset and in multi-master mode, so the DFLL's own I2C master may share PWR_I2C with it. */
int tdfll_platform_i2c5_shareable(char *why, size_t len);
/* VDD_CPU's MAX77812 M4_VOUT code over TegraI2C; write reads back. 0 or TEGRA_I2C_E* */
int tdfll_platform_vout_read(u8 *code);
int tdfll_platform_vout_write(u8 code);
u8 tdfll_platform_pmic_address(void);
/* a DFLL register (offsets as clk-dfll.c names them; the LUT RAM is at 0x200), for status only */
u32 tdfll_platform_dfll_readl(u32 offset);
/* "SKU 0x83 A01, CPU speedo 0/1612 process 0, ..." */
void tdfll_platform_describe_chip(char *buf, size_t len);

/* shim/linux.c: a registered clock by its name (clock-output-names / the CAR clock names), or NULL */
struct clk *tdfll_clk_lookup(const char *name);
/* shim/linux.c: how often a register poll ran out its bounded wait (see <linux/iopoll.h>) */
unsigned int tdfll_poll_timeouts(void);
#endif
