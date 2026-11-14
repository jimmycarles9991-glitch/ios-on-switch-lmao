// SPDX-License-Identifier: GPL-2.0
// tegra_dfll_landing_model.h's DFLL. clk-dfll.c is included whole so that its request and Vmin calibration paths
// (statics) run as the kernel builds them; shim/linux.c supplies the Linux side, this file kits/SwitchLinux's.
#include "drivers/TegraDFLL/linux/drivers/clk/tegra/clk-dfll.c"

#include "drivers/TegraDFLL/test/tegra_dfll_landing_model.h"

/* libc's, which the shim's kernel.h (abs, min, max as macros) keeps out of this unit's includes */
void *calloc(size_t count, size_t size);
void *malloc(size_t size);
void free(void *block);

#define LUT_BASE_UV 795000
#define LUT_STEP_UV 15000 /* 33 entries from 795 mV past the CVB top, 1235 mV */
#define TUNE_HIGH_MIN_MV 850
#define TUNE1_LOW 0x012207ffu
#define TUNE1_HIGH 0x03fff7ffu
#define LOW_TUNE_EXTRA_HZ 50000000L

static uint32_t regs[0x300 / 4];
static uint64_t now_ns;
static unsigned long irqs_off;
static unsigned int blocking;
static unsigned long floor_hz, per_step_hz;
static unsigned long slew_per_s; /* 0: the output reaches its rest at once */
static unsigned int forced_out;  /* a request's forced output, coming down from forced_at */
static uint64_t forced_at;
static bool forced;

struct call {
	void (*fn)(void *arg);
	void *arg;
	uint64_t due;
	bool armed;
};
static struct call calls[4];
static unsigned int ncalls;

static struct device cpu_dev = {.init_name = "cpu0"};
static struct device dfll_dev = {.init_name = "70110000.clock"};
static const struct thermal_tv floor_table[] = {{.temp = 20, .millivolts = 700}};
static struct tegra_dfll_soc_data soc = {
	.dev = &cpu_dev,
	.alignment = {.step_uv = 5000},
	.min_millivolts = 620,
	.tune_high_min_millivolts = TUNE_HIGH_MIN_MV,
	.tune0_low = 0xffcf,
	.tune1_low = TUNE1_LOW,
	.tune1_high = TUNE1_HIGH,
	.thermal_floor_table = floor_table,
	.thermal_floor_table_size = 1,
};
static struct tegra_dfll model;
static unsigned long rate_floors[2], tune_high_floors[2];

/* The CVB rows of speedo 1659 (MHz at mV), as boot 21's governor table printed them, and the top of the table. */
static const unsigned long opps[][2] = {
	{918, 620},  {1020, 630}, {1122, 655}, {1224, 690}, {1326, 730},
	{1428, 770}, {1581, 835}, {1683, 885}, {1785, 940}, {2295, 1235},
};

static long dvco_at(unsigned int out)
{
	long hz = (long)floor_hz + ((long)out - (long)model.tune_high_out_min) * (long)per_step_hz;

	if (regs[DFLL_TUNE1 / 4] != TUNE1_HIGH)
		hz += LOW_TUNE_EXTRA_HZ;
	return hz > 0 ? hz : 0;
}

struct loop {
	unsigned int out;
	unsigned long dvco;
	bool pinned;
};

static struct loop settle(void)
{
	const uint32_t force = regs[DFLL_OUTPUT_FORCE / 4];
	const uint32_t cfg = regs[DFLL_OUTPUT_CFG / 4];
	const unsigned int lo = (cfg >> DFLL_OUTPUT_CFG_MIN_SHIFT) & OUT_MASK;
	const unsigned int hi = (cfg >> DFLL_OUTPUT_CFG_MAX_SHIFT) & OUT_MASK;
	const long target =
		(long)(regs[DFLL_FREQ_REQ / 4] & DFLL_FREQ_REQ_MULT_MASK) * (REF_CLOCK_RATE / 2);
	struct loop l = {.out = lo};

	if (force & DFLL_OUTPUT_FORCE_ENABLE) {
		l.out = force & OUT_MASK;
		l.dvco = (unsigned long)dvco_at(l.out);
		return l;
	}
	while (l.out < hi && dvco_at(l.out) < target)
		l.out++;
	if (forced) {
		const uint64_t steps = (now_ns - forced_at) * slew_per_s / 1000000000ull;

		if (forced_out > l.out + steps) {
			l.out = forced_out - (unsigned int)steps;
			l.dvco = (unsigned long)dvco_at(l.out);
			return l;
		}
		forced = false;
	}
	l.pinned = l.out == lo && dvco_at(l.out) > target;
	/* between two LUT steps the loop holds its target on average */
	l.dvco = (unsigned long)(l.pinned || dvco_at(l.out) < target ? dvco_at(l.out) : target);
	return l;
}

uint32_t switch_mmio_model_read32(uintptr_t address)
{
	const uintptr_t off = address - (uintptr_t)regs;

	if (off >= sizeof(regs))
		__builtin_trap();
	if (off == DFLL_MONITOR_DATA && regs[DFLL_MONITOR_CTRL / 4] == DFLL_FREQ)
		return DFLL_MONITOR_DATA_NEW_MASK |
		       (uint32_t)(settle().dvco / (REF_CLOCK_RATE / REF_CLK_CYC_PER_DVCO_SAMPLE));
	if (off == DFLL_I2C_STS)
		return settle().out << DFLL_I2C_STS_I2C_LAST_SHIFT;
	return regs[off / 4];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
	const uintptr_t off = address - (uintptr_t)regs;

	if (off >= sizeof(regs))
		__builtin_trap();
	regs[off / 4] = value;
	if (off == DFLL_FREQ_REQ && (value & DFLL_FREQ_REQ_FORCE_ENABLE) && slew_per_s) {
		const int force = (int32_t)(value << 4) >> 20; /* FORCE_VAL, 27:16, signed */
		const int out = (int)model.lut_safe + force * model.cg / 128;

		forced_out = (unsigned int)clamp(out, 0, (int)OUT_MASK);
		forced_at = now_ns;
		forced = true;
	}
}

void _disable_preemption(void)
{
}

void _enable_preemption(void)
{
}

unsigned long switch_linux_irq_save(void)
{
	const unsigned long was = irqs_off;

	irqs_off = 1;
	return was;
}

void switch_linux_irq_restore(unsigned long flags)
{
	irqs_off = flags;
}

void switch_linux_spin_contended(switch_linux_spinlock *lock, uintptr_t self)
{
	__builtin_trap(); /* one thread: a held lock taken again is a deadlock */
}

uintptr_t switch_linux_model_thread(void)
{
	return 1;
}

uint64_t switch_linux_nanotime(void)
{
	return now_ns;
}

void switch_linux_udelay(uint64_t us)
{
	now_ns += us * 1000;
}

void switch_linux_usleep_range(uint64_t min_us, uint64_t max_us)
{
	blocking += irqs_off ? 1 : 0;
	now_ns += min_us * 1000;
}

void *switch_linux_malloc(uint64_t size, int zero)
{
	blocking += irqs_off ? 1 : 0;
	return zero ? calloc(1, size) : malloc(size);
}

void switch_linux_free(const void *block)
{
	free((void *)block);
}

void switch_linux_log(const char *fmt, ...)
{
}

volatile void *switch_linux_ioremap(uint64_t pa, uint64_t size, int write_combined)
{
	return NULL;
}

void *switch_linux_call_alloc(void (*fn)(void *arg), void *arg)
{
	if (ncalls == sizeof(calls) / sizeof(calls[0]))
		__builtin_trap();
	calls[ncalls] = (struct call){.fn = fn, .arg = arg};
	return &calls[ncalls++];
}

bool switch_linux_call_cancel(void *call)
{
	struct call *c = call;
	const bool was = c->armed;

	c->armed = false;
	return was;
}

bool switch_linux_call_cancel_wait(void *call)
{
	blocking += irqs_off ? 1 : 0;
	return switch_linux_call_cancel(call);
}

bool switch_linux_call_enter_after(void *call, uint64_t delay_ns)
{
	struct call *c = call;
	const bool was = c->armed;

	c->armed = true;
	c->due = now_ns + delay_ns;
	return was;
}

struct regulator *tdfll_platform_regulator(struct device *dev, const char *id)
{
	return NULL;
}

struct reset_control *tdfll_platform_reset(struct device *dev, const char *id)
{
	return NULL;
}

int tegra_round_voltage(int mv, const struct rail_alignment *align, int up)
{
	return mv; /* cvb.c's; only the thermal-floor query this model never makes calls it */
}

int tegra_dfll_landing_start(unsigned long floor, unsigned long per_step, unsigned long rate_hz)
{
	struct tegra_dfll *td = &model;
	unsigned int i;

	__builtin_memset(regs, 0, sizeof(regs));
	now_ns = 1000000000ull;
	floor_hz = floor;
	per_step_hz = per_step;
	for (i = 0; i < sizeof(opps) / sizeof(opps[0]); i++)
		dev_pm_opp_add(&cpu_dev, opps[i][0] * 1000000, opps[i][1] * 1000);

	td->dev = &dfll_dev;
	td->soc = &soc;
	td->base = td->i2c_base = (void __iomem *)regs;
	td->i2c_controller_base = (void __iomem *)&regs[0x100 / 4];
	td->lut_base = (void __iomem *)&regs[0x200 / 4];
	td->ref_rate = REF_CLOCK_RATE;
	td->sample_rate = 12500;
	td->force_mode = 1;
	td->cf = 6;
	td->ci = 0;
	td->cg = 2;
	td->pmu_if = TEGRA_DFLL_PMU_I2C;
	td->lut_size = MAX_DFLL_VOLTAGES;
	for (i = 0; i < MAX_DFLL_VOLTAGES; i++)
		td->lut_uv[i] = LUT_BASE_UV + i * LUT_STEP_UV;
	td->tune_high_out_min = find_mv_out_cap(td, TUNE_HIGH_MIN_MV);
	td->tune_high_out_start = td->tune_high_out_min + DFLL_TUNE_HIGH_MARGIN_STEPS;
	td->dvco_rate_floors = rate_floors;
	td->tune_high_dvco_rate_floors = tune_high_floors;
	tune_high_floors[soc.thermal_floor_table_size] =
		get_dvco_rate_above(td, td->tune_high_out_start);
	td->tune_high_target_rate_min = get_dvco_rate_above(td, td->tune_high_out_min);
	td->thermal_floor_index = 0;
	td->thermal_floor_output = find_mv_out_cap(td, floor_table[0].millivolts);
	td->thermal_cap_output = td->lut_size - 1;
	td->out_rate_min = opps[0][0] * 1000000;
	td->out_rate_max = opps[sizeof(opps) / sizeof(opps[0]) - 1][0] * 1000000;
	td->dvco_calibration_max = ROUND_DVCO_MAX_RATE(td->out_rate_max, td->ref_rate);
	spin_lock_init(&td->lock);
	hrtimer_init(&td->tune_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	td->tune_timer.function = dfll_tune_timer_cb;
	td->tune_delay = ktime_set(0, DFLL_TUNE_HIGH_DELAY * 1000);
	td->tune_ramp_delay = ktime_set(0, DFLL_ONE_SHOT_SETTLE_TIME * 1000);
	init_timer_deferrable(&td->calibration_timer);
	td->calibration_timer.function = calibration_timer_cb;
	td->calibration_timer.data = (unsigned long)td;
	td->calibration_delay = usecs_to_jiffies(DFLL_CALIBR_TIME);

	set_dvco_rate_min(td, &td->last_req);
	dfll_tune_low(td);
	dfll_set_output_limits(td, dfll_get_output_min(td), td->thermal_cap_output);
	td->lut_safe = td->lut_min + 1;
	td->mode = DFLL_CLOSED_LOOP;
	return tegra_dfll_landing_request(rate_hz);
}

int tegra_dfll_landing_request(unsigned long rate_hz)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&model.lock, flags);
	err = dfll_request_rate(&model, rate_hz);
	spin_unlock_irqrestore(&model.lock, flags);
	return err;
}

void tegra_dfll_landing_run_ms(unsigned int ms)
{
	const uint64_t end = now_ns + (uint64_t)ms * 1000000;

	for (;;) {
		struct call *next = NULL;
		unsigned int i;

		for (i = 0; i < ncalls; i++) {
			if (calls[i].armed && calls[i].due <= end &&
			    (!next || calls[i].due < next->due))
				next = &calls[i];
		}
		if (!next)
			break;
		if (next->due > now_ns)
			now_ns = next->due;
		next->armed = false;
		next->fn(next->arg);
	}
	now_ns = end;
}

void tegra_dfll_landing_set_floor(unsigned long floor)
{
	floor_hz = floor;
}

void tegra_dfll_landing_set_slew(unsigned long steps_per_s)
{
	slew_per_s = steps_per_s;
}

unsigned long tegra_dfll_landing_cpu_hz(void)
{
	const uint64_t scale = ((regs[DFLL_FREQ_REQ / 4] & DFLL_FREQ_REQ_SCALE_MASK) >>
				DFLL_FREQ_REQ_SCALE_SHIFT) +
			       1;

	return (unsigned long)(settle().dvco * scale / DFLL_FREQ_REQ_SCALE_MAX);
}

bool tegra_dfll_landing_pinned(void)
{
	return settle().pinned;
}

unsigned long tegra_dfll_landing_dvco_rate_min(void)
{
	return model.dvco_rate_min;
}

unsigned int tegra_dfll_landing_irqs_off_blocking(void)
{
	return blocking;
}
