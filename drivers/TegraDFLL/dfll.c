// SPDX-License-Identifier: GPL-2.0
/*
 * The TegraDFLL.h API: the sequences L4T's CPU frequency driver and clock framework run around the vendored DFLL
 * driver, on the shim's clock tree. Linux-side C (shim prelude); XNU only through kits/SwitchLinux and TegraI2C.
 *
 * Enable follows the T210 cpufreq driver L4T runs on this SoC (CTCaer switch-l4t-kernel-nvidia linux-dev
 * drivers/cpufreq/tegra210-cpufreq.c enable_cpu_clk: clk_set_rate(dfll, rate of cpu_g), clk_set_parent(cpu_g, dfll)),
 * then the cpufreq target. Disable follows L4T 4.9 drivers/cpufreq/tegra124-cpufreq.c tegra124_cpu_switch_to_pllx
 * (cpu to PLLP, DFLL off, regulator_sync_voltage, cpu to PLLX); L4T's T210 driver never leaves the DFLL at runtime.
 * The open loop / closed loop transitions themselves are clk-dfll.c's cclk_g notifier (cclk_g_parent_event) and its
 * prepare/unprepare ops, driven by shim/linux.c clk_set_parent exactly as L4T's clk_core_set_parent drives them.
 */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <tdfll_platform.h>
#include <soc/tegra/cvb.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/tegra-dfll.h>
#include "TegraDFLL.h"
#include "../TegraI2C/TegraI2C.h"

/* clk-dfll.c register offsets used for status (DFLL_CTRL, DFLL_FREQ_REQ, DFLL_MONITOR_*, DFLL_I2C_STS, LUT RAM) */
#define DFLL_CTRL 0x00
#define DFLL_CTRL_CLOSED_LOOP 2 /* DFLL_CLOSED_LOOP - 1, as dfll_set_mode writes it */
#define DFLL_FREQ_REQ 0x14
#define DFLL_MONITOR_CTRL 0x28
#define DFLL_MONITOR_CTRL_FREQ 6
#define DFLL_MONITOR_DATA 0x2c
#define DFLL_I2C_STS 0x48
#define DFLL_LUT 0x200
#define DFLL_REF_RATE 51000000UL
#define MAX77812_UV(code) (250000u + 5000u * (unsigned)(code))

/* clk-dfll.c (clk-dfll.h is not on this file's include path) */
void tegra_dfll_resume(struct platform_device *pdev, bool on_dfll);

/* clk-tegra124-dfll-fcpu.c: the T210B01 CPU CVB tables (global there, no header declares them) */
extern struct cvb_table tegra210b01_cpu_cvb_tables[4];

/* bounded waits (no interrupt to wait on): loop settle before the one check after enable; MAX77812 slew after a
 * VDD_CPU write (<= 1275 mV at the DT's 5 mV/us = 255 us; SwitchThermal waits the same 1 ms) */
#define ENABLE_SETTLE_MS 10
#define VOUT_SLEW_MS 1

static void *api_lock;

static struct {
	bool prepared, enabled, thermal_set;
	int prepare_err;
	char prepare_why[96];
	struct tegra_dfll *td;
	struct clk *dfll, *cclk_g, *pllx, *pllp;
	unsigned int floor_index, cap_index;
	u8 kept_vout; /* VDD_CPU code found at enable, restored by disable */
} st;

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

static void lock(void)
{
	switch_linux_mutex_lock(switch_linux_mutex_of(&api_lock));
}

static void unlock(void)
{
	switch_linux_mutex_unlock(api_lock);
}

static int apply_thermal(void)
{
	int a = tegra_dfll_update_thermal_index(st.td, TEGRA_DFLL_THERMAL_FLOOR, st.floor_index);
	int b = tegra_dfll_update_thermal_index(st.td, TEGRA_DFLL_THERMAL_CAP, st.cap_index);

	return a ? a : b;
}

static int prepare_locked(char *why, size_t len)
{
	struct platform_device *pdev = NULL;
	struct platform_driver *drv = NULL;
	int ret = 0;

	if (st.prepared)
		return 0;
	if (st.prepare_err)
		return fail(why, len, st.prepare_err, "%s", st.prepare_why);
	ret = tdfll_platform_init(st.prepare_why, sizeof(st.prepare_why));
	if (!ret) {
		/* what the driver core does for a DT platform device: bind it, then the driver's probe (tegra124_dfll_fcpu_
		 * probe -> tegra_cvb_add_opp_table, tegra_dfll_register) */
		pdev = tdfll_platform_dfll_device();
		drv = tdfll_builtin_platform_driver();
		pdev->dev.driver = &drv->driver;
		ret = drv->probe(pdev);
		if (ret) {
			fail(st.prepare_why, sizeof(st.prepare_why), ret,
			     "the L4T DFLL probe failed (%d), see above", ret);
		}
	}
	if (!ret) {
		st.td = platform_get_drvdata(pdev);
		st.dfll = clk_get(NULL, "dfllCPU_out");
		st.cclk_g = clk_get(NULL, "cclk_g");
		st.pllx = clk_get(NULL, "pll_x");
		st.pllp = clk_get(NULL, "pll_p");
		if (!st.td || IS_ERR(st.dfll) || IS_ERR(st.cclk_g) || IS_ERR(st.pllx) ||
		    IS_ERR(st.pllp)) {
			ret = fail(st.prepare_why, sizeof(st.prepare_why), -ENODEV,
				   "the DFLL probe left no clock");
		}
	}
	if (!ret && st.thermal_set) {
		ret = apply_thermal();
		if (ret) {
			fail(st.prepare_why, sizeof(st.prepare_why), ret,
			     "thermal indices %u/%u out of range", st.floor_index, st.cap_index);
		}
	}
	if (ret) {
		if (ret !=
		    -EAGAIN) /* see tdfll_platform_init: a transient PMIC probe failure is retried */
			st.prepare_err = ret;
		printk("not available: %s\n", st.prepare_why);
		return fail(why, len, ret, "%s", st.prepare_why);
	}
	st.prepared = true;
	return 0;
}

int tegra_dfll_cpu_prepare(char *why, size_t len)
{
	int ret = 0;

	lock();
	ret = prepare_locked(why, len);
	unlock();
	return ret;
}

/* tegra124_cpu_switch_to_pllx, with regulator_sync_voltage = the VDD_CPU code found at enable */
static int to_pllx_locked(char *why, size_t len)
{
	int ret = 0;

	/* PRE_PARENT_CHANGE unlocks the DFLL (open loop), the mux moves to PLLP, the DFLL is unprepared (disabled) */
	ret = clk_set_parent(st.cclk_g, st.pllp);
	if (ret) {
		return fail(why, len, ret,
			    "cannot move the CPU off the DFLL (%d); it stays on the DFLL", ret);
	}
	st.enabled = false;
	ret = tdfll_platform_vout_write(st.kept_vout);
	if (ret) {
		return fail(why, len, -EIO,
			    "VDD_CPU not restored to %u mV (TegraI2C %d); the CPU stays on PLLP "
			    "408 MHz",
			    MAX77812_UV(st.kept_vout) / 1000, ret);
	}
	msleep(VOUT_SLEW_MS);
	if (!tdfll_platform_pllx_locked())
		return fail(why, len, -EIO, "PLLX is not locked; the CPU stays on PLLP 408 MHz");
	ret = clk_set_parent(st.cclk_g, st.pllx);
	if (ret) {
		return fail(why, len, ret,
			    "cannot move the CPU to PLLX (%d); it stays on PLLP 408 MHz", ret);
	}
	return 0;
}

static u32 dfll_last_sent(u32 sts)
{
	return tdfll_platform_dfll_readl(DFLL_LUT + 4 * ((sts >> 1) & 0x3f)) &
	       0xff; /* LUT[DFLL_I2C_STS last] */
}

/* After the settle: the loop is closed and the PMIC holds what the DFLL's I2C master last sent. The DFLL may send one
 * more step while TegraI2C reads the PMIC back, so the value it sent just before or just after that read both count. */
static int check_closed_loop(char *why, size_t len)
{
	u32 ctrl = tdfll_platform_dfll_readl(DFLL_CTRL);
	u32 before = tdfll_platform_dfll_readl(DFLL_I2C_STS);
	u32 after = 0;
	u8 vout = 0;
	int err = 0;

	if ((ctrl & 3) != DFLL_CTRL_CLOSED_LOOP) {
		return fail(why, len, -EIO, "the DFLL did not close the loop (DFLL_CTRL %#x)",
			    ctrl);
	}
	err = tdfll_platform_vout_read(&vout);
	after = tdfll_platform_dfll_readl(DFLL_I2C_STS);
	if (err) {
		return fail(why, len, -EIO, "VDD_CPU unreadable after the switch (TegraI2C %d)",
			    err);
	}
	if (vout != dfll_last_sent(before) && vout != dfll_last_sent(after)) {
		return fail(why, len, -EIO,
			    "MAX77812 holds %u mV but the DFLL last sent %u mV (DFLL_I2C_STS %#x)",
			    MAX77812_UV(vout) / 1000, MAX77812_UV(dfll_last_sent(after)) / 1000,
			    after);
	}
	return 0;
}

int tegra_dfll_cpu_enable(unsigned long rate_hz, char *why, size_t len)
{
	char undo[96];
	unsigned long now = 0;
	int ret = 0;

	if (rate_hz > TEGRA_DFLL_CPU_MAX_HZ) {
		return fail(why, len, -EINVAL, "%lu kHz is above Horizon's %lu kHz CPU maximum",
			    rate_hz / 1000, TEGRA_DFLL_CPU_MAX_HZ / 1000);
	}
	lock();
	if (st.enabled) {
		ret = clk_set_rate(st.dfll, rate_hz);
		if (ret)
			fail(why, len, ret, "the DFLL refused %lu kHz (%d)", rate_hz / 1000, ret);
		goto out;
	}
	ret = prepare_locked(why, len);
	if (!ret)
		ret = tdfll_platform_cpu_on_pllx(why, len);
	if (!ret)
		ret = tdfll_platform_i2c5_shareable(why, len);
	if (!ret && tdfll_platform_vout_read(&st.kept_vout)) {
		ret = fail(why, len, -EIO, "VDD_CPU (MAX77812 %#x) does not answer",
			   tdfll_platform_pmic_address());
	}
	if (ret)
		goto out;

	now = clk_get_rate(st.cclk_g);
	ret = clk_set_rate(st.dfll, now);
	if (ret) {
		fail(why, len, ret, "the DFLL refused the current CPU rate %lu kHz (%d)",
		     now / 1000, ret);
		goto out;
	}
	ret = clk_set_parent(st.cclk_g, st.dfll);
	if (ret) {
		fail(why, len, ret,
		     "switching cclk_g to the DFLL failed (%d); the CPU stays on PLLX", ret);
		goto out;
	}
	st.enabled = true;
	if ((tdfll_platform_dfll_readl(DFLL_CTRL) & 3) != DFLL_CTRL_CLOSED_LOOP) {
		ret = fail(why, len, -EIO, "the DFLL did not close the loop (DFLL_CTRL %#x)",
			   tdfll_platform_dfll_readl(DFLL_CTRL));
	}
	if (!ret) {
		ret = clk_set_rate(st.dfll, rate_hz);
		if (ret)
			fail(why, len, ret, "the DFLL refused %lu kHz (%d)", rate_hz / 1000, ret);
	}
	if (!ret) {
		msleep(ENABLE_SETTLE_MS);
		ret = check_closed_loop(why, len);
	}
	if (ret) {
		if (to_pllx_locked(undo, sizeof(undo)))
			printk("could not return to PLLX: %s\n", undo);
		goto out;
	}
	printk("CPU on the DFLL, closed loop at %lu kHz (from PLLX %lu kHz, VDD_CPU was %u mV)\n",
	       rate_hz / 1000, now / 1000, MAX77812_UV(st.kept_vout) / 1000);
out:
	unlock();
	return ret;
}

int tegra_dfll_cpu_set_rate(unsigned long rate_hz)
{
	int ret = -EPERM;

	if (rate_hz > TEGRA_DFLL_CPU_MAX_HZ)
		return -EINVAL;
	lock();
	if (st.enabled)
		ret = clk_set_rate(st.dfll, rate_hz);
	unlock();
	return ret;
}

int tegra_dfll_cpu_set_thermal(unsigned floor_index, unsigned cap_index)
{
	int ret = 0;

	lock();
	if (floor_index >= TEGRA_DFLL_FLOOR_STATES || cap_index >= TEGRA_DFLL_CAP_STATES) {
		ret = -ERANGE;
	} else {
		st.floor_index = floor_index;
		st.cap_index = cap_index;
		st.thermal_set = true;
		if (st.prepared)
			ret = apply_thermal();
	}
	unlock();
	return ret;
}

int tegra_dfll_cpu_disable_to_pllx(char *why, size_t len)
{
	int ret = 0;

	lock();
	if (st.enabled) {
		ret = to_pllx_locked(why, len);
		if (!ret) {
			printk("CPU back on PLLX, VDD_CPU %u mV\n",
			       MAX77812_UV(st.kept_vout) / 1000);
		}
	}
	unlock();
	return ret;
}

int tegra_dfll_cpu_opps(unsigned long max_hz, unsigned int *khz, unsigned int *mv, unsigned int cap)
{
	struct device *cpu = get_cpu_device(0);
	struct dev_pm_opp *opp = NULL;
	unsigned long rate = 0;
	unsigned int n = 0;
	int ret = -EPERM;

	if (max_hz > TEGRA_DFLL_CPU_MAX_HZ)
		max_hz = TEGRA_DFLL_CPU_MAX_HZ;
	lock();
	if (st.prepared) {
		/* cvb.c added the rows in table order, ascending; walk them with the OPP lookups dfll.c uses */
		while (n < cap && !IS_ERR(opp = dev_pm_opp_find_freq_ceil(cpu, &rate)) &&
		       rate <= max_hz) {
			khz[n] = rate / 1000;
			mv[n] = dev_pm_opp_get_voltage(opp) / 1000;
			n++;
			rate++;
		}
		ret = n;
	}
	unlock();
	return ret;
}

int tegra_dfll_cpu_thermal_mv(unsigned int *floor_mv, unsigned int *cap_mv)
{
	int ret = -EPERM;

	lock();
	if (st.prepared) {
		*floor_mv = tegra_dfll_get_thermal_floor_mv();
		*cap_mv = tegra_dfll_get_thermal_cap_mv();
		ret = 0;
	}
	unlock();
	return ret;
}

/* clk-dfll.c dfll_calc_monitored_rate + dfll_scale_dvco_rate, one read (the monitor is in FREQ mode) */
static unsigned long measured_hz(void)
{
	u32 scale = (tdfll_platform_dfll_readl(DFLL_FREQ_REQ) >> 8) & 0xff;

	if (tdfll_platform_dfll_readl(DFLL_MONITOR_CTRL) != DFLL_MONITOR_CTRL_FREQ)
		return 0;
	return (u64)(tdfll_platform_dfll_readl(DFLL_MONITOR_DATA) & 0xffff) * (DFLL_REF_RATE / 4) *
	       (scale + 1) / 256;
}

void tegra_dfll_cpu_status(unsigned long *target_hz, unsigned long *measured, unsigned int *vdd_mv)
{
	*target_hz = *measured = 0;
	*vdd_mv = 0;
	lock();
	if (st.enabled) {
		*target_hz = clk_get_rate(st.dfll);
		*measured = measured_hz();
		*vdd_mv =
			MAX77812_UV(dfll_last_sent(tdfll_platform_dfll_readl(DFLL_I2C_STS))) / 1000;
	}
	unlock();
}

/* cvb.c tegra_cvb_add_opp_table's choice, repeated for the description (the driver keeps it in td->soc) */
static const struct cvb_table *chosen_cvb(void)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(tegra210b01_cpu_cvb_tables); i++) {
		const struct cvb_table *t = &tegra210b01_cpu_cvb_tables[i];

		if ((t->speedo_id == -1 || t->speedo_id == tegra_sku_info.cpu_speedo_id) &&
		    (t->process_id == -1 || t->process_id == tegra_sku_info.cpu_process_id))
			return t;
	}
	return NULL;
}

void tegra_dfll_cpu_describe(unsigned long rate_hz, char *buf, size_t len)
{
	struct device *cpu = get_cpu_device(0);
	const struct cvb_table *cvb = NULL;
	struct dev_pm_opp *opp = NULL;
	unsigned long top = ULONG_MAX;
	unsigned long at = 0;
	size_t n = 0;

	if (!buf || !len)
		return;
	lock();
	tdfll_platform_describe_chip(buf, len);
	n = strlen(buf);
	if (!st.prepared) {
		snprintf(buf + n, len - n, "; DFLL %s",
			 st.prepare_err ? st.prepare_why : "not prepared");
		goto out;
	}
	cvb = chosen_cvb();
	opp = dev_pm_opp_find_freq_floor(cpu, &top);
	if (!rate_hz)
		rate_hz = clk_get_rate(st.dfll);
	at = rate_hz;
	opp = IS_ERR(opp) ? opp : dev_pm_opp_find_freq_ceil(cpu, &at);
	snprintf(buf + n, len - n,
		 "; CVB '%s' (speedo_id %d process_id %d, %u..%d mV), top %lu MHz; %lu MHz -> %lu "
		 "mV",
		 tegra_dfll_get_cvb_version(), cvb ? cvb->speedo_id : -2,
		 cvb ? cvb->process_id : -2, tegra_dfll_get_min_millivolts(),
		 cvb ? cvb->max_millivolts : 0, top / 1000000, rate_hz / 1000000,
		 IS_ERR(opp) ? 0 : dev_pm_opp_get_voltage(opp) / 1000);
	n = strlen(buf);
	if (st.enabled) {
		u32 ctrl = tdfll_platform_dfll_readl(DFLL_CTRL);
		u32 sent = dfll_last_sent(tdfll_platform_dfll_readl(DFLL_I2C_STS));
		u64 measured = measured_hz();
		snprintf(buf + n, len - n,
			 "; %s, target %lu MHz, measured %llu MHz, VDD_CPU sent %u mV (MAX77812 "
			 "%#x)",
			 (ctrl & 3) == DFLL_CTRL_CLOSED_LOOP ? "closed loop" : "NOT closed loop",
			 clk_get_rate(st.dfll) / 1000000, (unsigned long long)(measured / 1000000),
			 MAX77812_UV(sent) / 1000, tdfll_platform_pmic_address());
		n = strlen(buf);
	}
	if (tdfll_poll_timeouts())
		snprintf(buf + n, len - n, "; monitor waits missed %u", tdfll_poll_timeouts());
out:
	unlock();
}

/* L4T's path after context loss (clk-dfll.c tegra_dfll_resume, from the T210 clock syscore resume): the DFLL's own
 * registers, LUT and I2C output interface programmed again, left disabled. SwitchThermal leaves the DFLL for PLLX
 * before SC7 and enables it again afterwards, so on_dfll is false. */
int tegra_dfll_cpu_resume(char *why, size_t len)
{
	int ret = 0;

	lock();
	if (!st.prepared)
		goto out;
	if (st.enabled) {
		ret = fail(why, len, -EBUSY, "the CPU was on the DFLL through SC7");
		goto out;
	}
	ret = tdfll_platform_resume(why, len);
	if (!ret)
		tegra_dfll_resume(tdfll_platform_dfll_device(), false);
	if (!ret && st.thermal_set) {
		ret = apply_thermal();
		if (ret) {
			fail(why, len, ret, "thermal indices %u/%u refused after SC7",
			     st.floor_index, st.cap_index);
		}
	}
out:
	unlock();
	return ret;
}
