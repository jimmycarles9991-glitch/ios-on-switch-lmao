// SPDX-License-Identifier: GPL-2.0
/*
 * Switch: the Linux 4.9 kernel services the vendored L4T DFLL sources call, on XNU (through kits/SwitchLinux only).
 * Board specifics (which clocks, which regulator, the device tree node) live in platform.c.
 *
 * Concurrency: every clock-framework, OPP, of_*, pm_runtime, regulator and devm call here comes from the public API
 * in dfll.c, which serializes on its own mutex, so the clock tree needs no prepare/enable locks of its own (L4T's
 * prepare_lock/enable_lock). The DFLL's timer callbacks (thread calls) never enter the clock framework: they only
 * take clk-dfll.c's td->lock and touch DFLL registers, as in Linux.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <tdfll_platform.h>
#include <soc/tegra/cvb.h>

/* printk: one kernel log line per call */
int printk(const char *fmt, ...)
{
	char line[256];
	va_list ap; /* NOLINT(cppcoreguidelines-init-variables): va_start initializes it */
	int n = 0;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return n;
	n = (int)strlen(line);
	/* L4T messages mostly end in "\n", a few do not ("unexpected ref clk rate ..."): one line each either way */
	if (n && line[n - 1] == '\n')
		line[n - 1] = 0;
	switch_linux_log("TegraDFLL: %s\n", line);
	return n;
}

/* devm_* memory is never released (see device.h) */
void *kmalloc(size_t size, gfp_t flags)
{
	return switch_linux_malloc(size, 1);
}

void *kzalloc(size_t size, gfp_t flags)
{
	return switch_linux_malloc(size, 1);
}

void kfree(const void *p)
{
	switch_linux_free(p);
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags)
{
	return switch_linux_malloc(size, 1);
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t flags)
{
	return switch_linux_malloc(size, 1);
}

void devm_kfree(struct device *dev, void *p)
{
	switch_linux_free(p);
}

/* pinctrl: only clk-dfll.c's PWM-to-PMIC path asks; the OLED drives its MAX77812 over I2C (no nvidia,pwm-to-pmic) */
struct pinctrl *devm_pinctrl_get(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name)
{
	return ERR_PTR(-ENODEV);
}

int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *s)
{
	return -ENODEV;
}

static unsigned int poll_timeouts;

int tdfll_poll_timed_out(void)
{
	__atomic_add_fetch(&poll_timeouts, 1u, __ATOMIC_RELAXED);
	return -ETIMEDOUT;
}

unsigned int tdfll_poll_timeouts(void)
{
	return __atomic_load_n(&poll_timeouts, __ATOMIC_RELAXED);
}

static void *call_alloc(void (*fn)(void *), void *arg)
{
	void *call = switch_linux_call_alloc(fn, arg);

	if (!call)
		panic("TegraDFLL: cannot allocate a thread call for a DFLL timer");
	return call;
}

static void timer_list_fire(void *arg)
{
	struct timer_list *timer = arg;

	timer->function(timer->data);
}

void init_timer(struct timer_list *timer)
{
	timer->tdfll_call = call_alloc(timer_list_fire, timer);
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
	unsigned long now = jiffies;

	timer->expires = expires;
	switch_linux_call_enter_after(timer->tdfll_call, (u64)(expires > now ? expires - now : 0) *
								 (u64)(NSEC_PER_SEC / HZ));
	return 0;
}

int del_timer(struct timer_list *timer)
{
	return timer->tdfll_call && switch_linux_call_cancel(timer->tdfll_call) ? 1 : 0;
}

int del_timer_sync(struct timer_list *timer)
{
	return timer->tdfll_call && switch_linux_call_cancel_wait(timer->tdfll_call) ? 1 : 0;
}

static void hrtimer_fire(void *arg)
{
	struct hrtimer *timer = arg;

	(void)timer->function(
		timer); /* HRTIMER_RESTART is not used by the vendored code (see hrtimer.h) */
}

void hrtimer_init(struct hrtimer *timer, int which_clock, enum hrtimer_mode mode)
{
	timer->tdfll_call = call_alloc(hrtimer_fire, timer);
}

void hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode)
{
	if (mode == HRTIMER_MODE_ABS)
		tim -= ktime_get();
	switch_linux_call_enter_after(timer->tdfll_call, tim > 0 ? (u64)tim : 0);
}

int hrtimer_cancel(struct hrtimer *timer)
{
	return timer->tdfll_call && switch_linux_call_cancel_wait(timer->tdfll_call) ? 1 : 0;
}

/* device tree: static nodes (see of.h) */
static const struct tdfll_property *of_find_prop(const struct device_node *np, const char *name)
{
	const struct tdfll_property *p = NULL;

	if (!np)
		return NULL;
	for (p = np->properties; p && p->name; p++) {
		if (!strcmp(p->name, name))
			return p;
	}
	return NULL;
}

int of_property_read_u32(const struct device_node *np, const char *propname, u32 *out_value)
{
	const struct tdfll_property *p = of_find_prop(np, propname);

	if (!p)
		return -EINVAL;
	if (!p->cells || !p->ncells)
		return -ENODATA;
	*out_value = p->cells[0];
	return 0;
}

int of_property_read_string(const struct device_node *np, const char *propname,
			    const char **out_string)
{
	const struct tdfll_property *p = of_find_prop(np, propname);

	if (!p)
		return -EINVAL;
	if (!p->strings || !p->nstrings)
		return -ENODATA;
	*out_string = p->strings[0];
	return 0;
}

bool of_property_read_bool(const struct device_node *np, const char *propname)
{
	return of_find_prop(np, propname) != NULL;
}

struct device_node *of_parse_phandle(const struct device_node *np, const char *phandle_name,
				     int index)
{
	const struct tdfll_property *p = of_find_prop(np, phandle_name);

	return (p && index == 0) ? p->phandle : NULL;
}

int of_device_is_compatible(const struct device_node *device, const char *compat)
{
	const struct tdfll_property *p = of_find_prop(device, "compatible");
	unsigned int i = 0;

	for (i = 0; p && p->strings && i < p->nstrings; i++) {
		if (!strcmp(p->strings[i], compat))
			return 1;
	}
	return 0;
}

bool of_device_is_available(const struct device_node *device)
{
	const char *status = NULL;

	if (of_property_read_string(device, "status", &status))
		return true;
	return (!strcmp(status, "okay") || !strcmp(status, "ok")) != 0;
}

/* Linux __of_match_node: the node's first compatible string that any entry matches wins (so a node with
 * "nvidia,tegra210b01-dfll", "nvidia,tegra210-dfll" gets the B01 data even though the T210 entry comes first) */
const struct of_device_id *of_match_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	const struct tdfll_property *p = of_find_prop(node, "compatible");
	const struct of_device_id *m = NULL;
	unsigned int i = 0;

	for (i = 0; p && p->strings && i < p->nstrings; i++) {
		for (m = matches; m->compatible[0] || m->name[0] || m->type[0]; m++) {
			if (m->compatible[0] && !strcmp(m->compatible, p->strings[i]))
				return m;
		}
	}
	return NULL;
}

struct resource *platform_get_resource(struct platform_device *pdev, unsigned int type,
				       unsigned int num)
{
	u32 i = 0;

	for (i = 0; i < pdev->num_resources; i++) {
		struct resource *r = &pdev->resource[i];

		if ((r->flags & type) && num-- == 0)
			return r;
	}
	return NULL;
}

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size)
{
	return (void __iomem *)(uintptr_t)switch_linux_ioremap(offset, size, 0);
}

void __iomem *devm_ioremap_resource(struct device *dev, struct resource *res)
{
	void __iomem *va = NULL;

	if (!res || !(res->flags & IORESOURCE_MEM))
		return ERR_PTR(-EINVAL);
	va = devm_ioremap(dev, res->start, resource_size(res));
	return va ? va : ERR_PTR(-ENOMEM);
}

void pm_runtime_enable(struct device *dev)
{
	dev->tdfll_rpm_enabled = true;
}

void pm_runtime_disable(struct device *dev)
{
	dev->tdfll_rpm_enabled = false;
}

int pm_runtime_get_sync(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	int ret = 0;

	if (dev->tdfll_rpm_usage++ == 0 && dev->tdfll_rpm_enabled && !dev->tdfll_rpm_active) {
		if (pm && pm->runtime_resume)
			ret = pm->runtime_resume(dev);
		dev->tdfll_rpm_active = ret == 0;
	}
	return ret < 0 ? ret : 1;
}

int pm_runtime_put_sync(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (WARN_ON(dev->tdfll_rpm_usage <= 0))
		return -EINVAL;
	if (--dev->tdfll_rpm_usage == 0 && dev->tdfll_rpm_enabled && dev->tdfll_rpm_active) {
		if (pm && pm->runtime_suspend)
			(void)pm->runtime_suspend(dev);
		dev->tdfll_rpm_active = false;
	}
	return 0;
}

/* reset controls and regulators come from platform.c */
struct reset_control *devm_reset_control_get(struct device *dev, const char *id)
{
	struct reset_control *rstc = tdfll_platform_reset(dev, id);

	return rstc ? rstc : ERR_PTR(-ENOENT);
}

int reset_control_assert(struct reset_control *rstc)
{
	rstc->set(true);
	return 0;
}

int reset_control_deassert(struct reset_control *rstc)
{
	rstc->set(false);
	return 0;
}

int reset_control_reset(struct reset_control *rstc)
{
	rstc->set(true);
	udelay(2);
	rstc->set(false);
	return 0;
}

struct regulator *devm_regulator_get(struct device *dev, const char *id)
{
	struct regulator *reg = tdfll_platform_regulator(dev, id);

	return reg ? reg : ERR_PTR(-EPROBE_DEFER);
}

void devm_regulator_put(struct regulator *regulator)
{
}

struct regmap *regulator_get_regmap(struct regulator *regulator)
{
	return &regulator->regmap;
}

/* the MAX77812 driver uses regulator_set_voltage_sel_regmap, so the hardware vsel is the selector (core.c 4.9) */
int regulator_get_hardware_vsel_register(struct regulator *regulator, unsigned *vsel_reg,
					 unsigned *vsel_mask)
{
	*vsel_reg = regulator->vsel_reg;
	*vsel_mask = regulator->vsel_mask;
	return 0;
}

int regulator_list_hardware_vsel(struct regulator *regulator, unsigned selector)
{
	if (selector >= (unsigned)regulator->n_voltages)
		return -EINVAL;
	return (int)selector;
}

int regulator_count_voltages(struct regulator *regulator)
{
	return regulator->n_voltages ? regulator->n_voltages : -EINVAL;
}

/* regulator_list_voltage_linear, then _regulator_list_voltage's constraint filter (0 = not usable) */
int regulator_list_voltage(struct regulator *regulator, unsigned selector)
{
	int uV = 0;

	if (selector >= (unsigned)regulator->n_voltages)
		return -EINVAL;
	uV = regulator->min_uV + regulator->uV_step * (int)selector;
	if (uV < regulator->constraint_min_uV || uV > regulator->constraint_max_uV)
		return 0;
	return uV;
}

int regulator_get_constraint_voltages(struct regulator *regulator, int *min_uV, int *max_uV)
{
	*min_uV = regulator->constraint_min_uV;
	*max_uV = regulator->constraint_max_uV;
	return 0;
}

unsigned int regulator_get_linear_step(struct regulator *regulator)
{
	return (unsigned int)regulator->uV_step;
}

/* OPP: one sorted table per device */
struct dev_pm_opp {
	unsigned long rate;
	unsigned long u_volt;
};

struct opp_table {
	struct device *dev;
	int count;
	struct dev_pm_opp opp[MAX_DVFS_FREQS];
};

static struct opp_table opp_tables[2];

static struct opp_table *opp_table_get(struct device *dev, bool create)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(opp_tables); i++) {
		if (opp_tables[i].dev == dev)
			return &opp_tables[i];
	}
	if (!create)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(opp_tables); i++) {
		if (!opp_tables[i].dev) {
			opp_tables[i].dev = dev;
			opp_tables[i].count = 0;
			return &opp_tables[i];
		}
	}
	return NULL;
}

int dev_pm_opp_add(struct device *dev, unsigned long freq, unsigned long u_volt)
{
	struct opp_table *t = opp_table_get(dev, true);
	int i = 0;
	int j = 0;

	if (!t)
		return -ENOMEM;
	for (i = 0; i < t->count && t->opp[i].rate < freq; i++)
		;
	if (i < t->count &&
	    t->opp[i].rate == freq) /* drivers/base/power/opp/core.c _opp_add: duplicate */
		return t->opp[i].u_volt == u_volt ? 0 : -EEXIST;
	if (t->count == MAX_DVFS_FREQS)
		return -ENOMEM;
	for (j = t->count; j > i; j--)
		t->opp[j] = t->opp[j - 1];
	t->opp[i].rate = freq;
	t->opp[i].u_volt = u_volt;
	t->count++;
	return 0;
}

void dev_pm_opp_remove(struct device *dev, unsigned long freq)
{
	struct opp_table *t = opp_table_get(dev, false);
	int i = 0;

	for (i = 0; t && i < t->count; i++) {
		if (t->opp[i].rate == freq) {
			for (; i + 1 < t->count; i++)
				t->opp[i] = t->opp[i + 1];
			if (--t->count == 0)
				t->dev = NULL;
			return;
		}
	}
}

struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *dev, unsigned long *freq)
{
	struct opp_table *t = opp_table_get(dev, false);
	int i = 0;

	for (i = 0; t && i < t->count; i++) {
		if (t->opp[i].rate >= *freq) {
			*freq = t->opp[i].rate;
			return &t->opp[i];
		}
	}
	return ERR_PTR(-ERANGE);
}

struct dev_pm_opp *dev_pm_opp_find_freq_floor(struct device *dev, unsigned long *freq)
{
	struct opp_table *t = opp_table_get(dev, false);
	int i = 0;

	for (i = t ? t->count - 1 : -1; i >= 0; i--) {
		if (t->opp[i].rate <= *freq) {
			*freq = t->opp[i].rate;
			return &t->opp[i];
		}
	}
	return ERR_PTR(-ERANGE);
}

unsigned long dev_pm_opp_get_voltage(struct dev_pm_opp *opp)
{
	return IS_ERR_OR_NULL(opp) ? 0 : opp->u_volt;
}

unsigned long dev_pm_opp_get_freq(struct dev_pm_opp *opp)
{
	return IS_ERR_OR_NULL(opp) ? 0 : opp->rate;
}

int dev_pm_opp_get_opp_count(struct device *dev)
{
	struct opp_table *t = opp_table_get(dev, false);

	return t ? t->count : -ENODEV;
}

/* clocks: clk.h says which L4T semantics hold */
struct clk {
	const char *name;
	struct clk_hw *hw;
	const struct clk_ops *ops;
	unsigned long flags;
	const char *const *parent_names;
	u8 num_parents;
	struct clk *parent;
	unsigned int prepare_count;
	unsigned int enable_count;
	struct notifier_block *notifiers;
	struct clk *next;
};

static struct clk *clk_list;

struct clk *tdfll_clk_lookup(const char *name)
{
	struct clk *c = NULL;

	for (c = clk_list; c && name; c = c->next) {
		if (!strcmp(c->name, name))
			return c;
	}
	return NULL;
}

static int clk_fetch_parent_index(struct clk *clk, struct clk *parent)
{
	int i = 0;

	for (i = 0; parent && i < clk->num_parents; i++) {
		if (clk->parent_names[i] && !strcmp(clk->parent_names[i], parent->name))
			return i;
	}
	return -EINVAL;
}

static struct clk *clk_parent_by_index(struct clk *clk, unsigned int index)
{
	return index < clk->num_parents ? tdfll_clk_lookup(clk->parent_names[index]) : NULL;
}

struct clk *clk_register(struct device *dev, struct clk_hw *hw)
{
	const struct clk_init_data *init = hw->init;
	struct clk *clk = NULL;

	if (!init || !init->name || !init->ops)
		return ERR_PTR(-EINVAL);
	clk = switch_linux_malloc(sizeof(*clk), 1);
	if (!clk)
		return ERR_PTR(-ENOMEM);
	clk->name = init->name;
	clk->hw = hw;
	clk->ops = init->ops;
	clk->flags = init->flags;
	clk->parent_names = init->parent_names;
	clk->num_parents = init->num_parents;
	if (clk->num_parents == 1) {
		clk->parent = clk_parent_by_index(clk, 0);
	} else if (clk->num_parents > 1 && clk->ops->get_parent) {
		clk->parent = clk_parent_by_index(clk, clk->ops->get_parent(hw));
	}
	hw->clk = clk;
	if (clk->ops->init)
		clk->ops->init(hw);
	clk->next = clk_list;
	clk_list = clk;
	return clk;
}

void clk_unregister(struct clk *clk)
{
	struct clk **pp = NULL;

	for (pp = &clk_list; *pp; pp = &(*pp)->next) {
		if (*pp == clk) {
			*pp = clk->next;
			return;
		}
	}
}

const char *__clk_get_name(const struct clk *clk)
{
	return clk ? clk->name : NULL;
}

struct clk_hw *__clk_get_hw(struct clk *clk)
{
	return clk ? clk->hw : NULL;
}

/* No rate cache: every clock here recalculates from hardware or driver state (clk-dfll.c: last_unrounded_rate). */
/* NOLINTNEXTLINE(misc-no-recursion): walks up the clock tree, a few parents deep */
unsigned long clk_get_rate(struct clk *clk)
{
	unsigned long parent_rate = 0;

	if (!clk)
		return 0;
	parent_rate = clk->parent ? clk_get_rate(clk->parent) : 0;
	if (clk->num_parents > 1 && !clk->parent)
		return 0;
	return clk->ops->recalc_rate ? clk->ops->recalc_rate(clk->hw, parent_rate) : parent_rate;
}

unsigned long clk_hw_get_rate(const struct clk_hw *hw)
{
	return clk_get_rate(hw->clk);
}

struct clk *clk_get_parent(struct clk *clk)
{
	return clk ? clk->parent : NULL;
}

/* Consumers name clocks by the node's clock-names (resolved through its "clocks", stored as CAR clock names), or,
 * like clkdev's con_id fallback (clk-dfll.c asks for "cclk_g"), by the clock's own name. */
struct clk *clk_get(struct device *dev, const char *id)
{
	const struct tdfll_property *names = dev ? of_find_prop(dev->of_node, "clock-names") : NULL;
	const struct tdfll_property *clocks = dev ? of_find_prop(dev->of_node, "clocks") : NULL;
	struct clk *clk = NULL;
	unsigned int i = 0;

	for (i = 0; names && clocks && id && i < names->nstrings && i < clocks->nstrings; i++) {
		if (!strcmp(names->strings[i], id))
			clk = tdfll_clk_lookup(clocks->strings[i]);
	}
	if (!clk)
		clk = tdfll_clk_lookup(id);
	return clk ? clk : ERR_PTR(-ENOENT);
}

struct clk *devm_clk_get(struct device *dev, const char *id)
{
	return clk_get(dev, id);
}

struct clk *of_clk_get_by_name(struct device_node *np, const char *name)
{
	struct clk *clk = tdfll_clk_lookup(name);

	return clk ? clk : ERR_PTR(-ENOENT);
}

void clk_put(struct clk *clk)
{
}

/* NOLINTNEXTLINE(misc-no-recursion): walks up the clock tree, a few parents deep */
int clk_prepare(struct clk *clk)
{
	int ret = 0;

	if (!clk)
		return 0;
	if (clk->prepare_count == 0) {
		ret = clk_prepare(clk->parent);
		if (ret)
			return ret;
		if (clk->ops->prepare) {
			ret = clk->ops->prepare(clk->hw);
			if (ret) {
				clk_unprepare(clk->parent);
				return ret;
			}
		}
	}
	clk->prepare_count++;
	return 0;
}

/* NOLINTNEXTLINE(misc-no-recursion): walks up the clock tree, a few parents deep */
void clk_unprepare(struct clk *clk)
{
	if (!clk || WARN_ON(clk->prepare_count == 0))
		return;
	if (--clk->prepare_count == 0) {
		if (clk->ops->unprepare)
			clk->ops->unprepare(clk->hw);
		clk_unprepare(clk->parent);
	}
}

/* NOLINTNEXTLINE(misc-no-recursion): walks up the clock tree, a few parents deep */
int clk_enable(struct clk *clk)
{
	int ret = 0;

	if (!clk)
		return 0;
	if (clk->enable_count == 0) {
		ret = clk_enable(clk->parent);
		if (ret)
			return ret;
		if (clk->ops->enable) {
			ret = clk->ops->enable(clk->hw);
			if (ret) {
				clk_disable(clk->parent);
				return ret;
			}
		}
	}
	clk->enable_count++;
	return 0;
}

/* NOLINTNEXTLINE(misc-no-recursion): walks up the clock tree, a few parents deep */
void clk_disable(struct clk *clk)
{
	if (!clk || WARN_ON(clk->enable_count == 0))
		return;
	if (--clk->enable_count == 0) {
		if (clk->ops->disable)
			clk->ops->disable(clk->hw);
		clk_disable(clk->parent);
	}
}

static bool notify_stopped(int ret)
{
	return ((unsigned int)ret & NOTIFY_STOP_MASK) != 0;
}

static int clk_notify(struct clk *clk, unsigned long msg, unsigned long old_rate,
		      unsigned long new_rate)
{
	struct clk_notifier_data cnd = {.clk = clk, .old_rate = old_rate, .new_rate = new_rate};
	struct notifier_block *nb = NULL;
	int ret = NOTIFY_DONE;

	for (nb = clk->notifiers; nb; nb = nb->next) {
		ret = nb->notifier_call(nb, msg, &cnd);
		if (notify_stopped(ret))
			break;
	}
	return ret;
}

int clk_notifier_register(struct clk *clk, struct notifier_block *nb)
{
	if (!clk || !nb)
		return -EINVAL;
	nb->next = clk->notifiers;
	clk->notifiers = nb;
	return 0;
}

int clk_notifier_unregister(struct clk *clk, struct notifier_block *nb)
{
	struct notifier_block **pp = NULL;

	for (pp = clk ? &clk->notifiers : NULL; pp && *pp; pp = &(*pp)->next) {
		if (*pp == nb) {
			*pp = nb->next;
			return 0;
		}
	}
	return -ENOENT;
}

long clk_round_rate(struct clk *clk, unsigned long rate)
{
	struct clk_rate_request req = {.rate = rate, .max_rate = ULONG_MAX};
	unsigned long parent_rate = 0;

	if (!clk)
		return 0;
	parent_rate = clk->parent ? clk_get_rate(clk->parent) : 0;
	if (clk->ops->determine_rate) {
		req.best_parent_rate = parent_rate;
		return clk->ops->determine_rate(clk->hw, &req) ? -EINVAL : (long)req.rate;
	}
	if (clk->ops->round_rate)
		return clk->ops->round_rate(clk->hw, rate, &parent_rate);
	return (long)clk_get_rate(clk);
}

/*
 * L4T clk.c clk_set_rate -> clk_core_set_rate_nolock -> clk_calc_new_rates / clk_change_rate for a clock that sets
 * its own rate (the DFLL output): skip a rate equal to the current one, validate with determine_rate (round_rate),
 * PRE_RATE_CHANGE, set_rate, POST_RATE_CHANGE. SWITCH difference: L4T ignores set_rate's return value; here it is
 * returned, after the POST notification (clk-dfll.c's notifier ignores rate events either way).
 */
int clk_set_rate(struct clk *clk, unsigned long rate)
{
	unsigned long old_rate = 0;
	unsigned long new_rate = 0;
	unsigned long parent_rate = 0;
	int ret = 0;

	if (!clk)
		return 0;
	old_rate = clk_get_rate(clk);
	if (!(clk->flags & CLK_SET_RATE_NOCACHE) && rate == old_rate)
		return 0;
	if ((clk->flags & CLK_SET_RATE_GATE) && clk->prepare_count)
		return -EBUSY;
	if (!clk->ops->set_rate)
		return -EINVAL;
	parent_rate = clk->parent ? clk_get_rate(clk->parent) : 0;
	new_rate = rate;
	if (clk->ops->determine_rate) {
		struct clk_rate_request req = {
			.rate = rate,
			.max_rate = ULONG_MAX,
			.best_parent_rate = parent_rate,
			.best_parent_hw = clk->parent ? clk->parent->hw : NULL,
		};

		if (clk->ops->determine_rate(clk->hw, &req))
			return -EINVAL;
		new_rate = req.rate;
	} else if (clk->ops->round_rate) {
		long r = clk->ops->round_rate(clk->hw, rate, &parent_rate);

		if (r < 0)
			return -EINVAL;
		new_rate = (unsigned long)r;
	}
	if (notify_stopped(clk_notify(clk, PRE_RATE_CHANGE, old_rate, new_rate))) {
		clk_notify(clk, ABORT_RATE_CHANGE, old_rate, new_rate);
		return -EBUSY;
	}
	ret = clk->ops->set_rate(clk->hw, new_rate, parent_rate);
	clk_notify(clk, POST_RATE_CHANGE, old_rate, clk_get_rate(clk));
	return ret;
}

/*
 * L4T clk.c clk_core_set_parent + __clk_set_parent(_before/_after), for the CPU super mux:
 *   PRE_RATE_CHANGE, PRE_PARENT_CHANGE (clk-dfll.c: refuses a DFLL with no rate, or unlocks a DFLL being left),
 *   a prepared clock first prepares+enables its new parent (clk-dfll.c: prepare = open loop), the mux write,
 *   the old parent is disabled+unprepared (clk-dfll.c: unprepare = disable a DFLL being left),
 *   POST_PARENT_CHANGE (clk-dfll.c: closes the loop on a DFLL just switched to), POST_RATE_CHANGE.
 * SWITCH difference: L4T ignores a failure to prepare/enable the new parent and switches anyway; switching the CPU to
 * a DFLL that did not start would stop its clock, so here that aborts the switch (ABORT_PARENT_CHANGE, error).
 */
int clk_set_parent(struct clk *clk, struct clk *parent)
{
	struct clk *old = NULL;
	unsigned long old_p_rate = 0;
	unsigned long p_rate = 0;
	unsigned long rate = 0;
	int index = 0;
	int ret = 0;

	if (!clk)
		return 0;
	if (clk->parent == parent)
		return 0;
	if (clk->num_parents > 1 && !clk->ops->set_parent)
		return -ENOSYS;
	if ((clk->flags & CLK_SET_PARENT_GATE) && clk->prepare_count)
		return -EBUSY;
	index = clk_fetch_parent_index(clk, parent);
	if (index < 0)
		return index;
	old = clk->parent;
	p_rate = clk_get_rate(parent);
	old_p_rate = old ? clk_get_rate(old) : 0;
	rate = clk_get_rate(clk);

	if (notify_stopped(
		    clk_notify(clk, PRE_RATE_CHANGE, rate, clk->ops->recalc_rate ? rate : p_rate)))
		return -EBUSY;
	if (notify_stopped(clk_notify(clk, PRE_PARENT_CHANGE, old_p_rate, p_rate))) {
		ret = -EBUSY;
		goto abort;
	}
	if (clk->prepare_count) {
		ret = clk_prepare(parent);
		if (ret)
			goto abort;
		ret = clk_enable(parent);
		if (ret) {
			clk_unprepare(parent);
			goto abort;
		}
	}
	clk->parent = parent;
	ret = clk->ops->set_parent(clk->hw, (u8)index);
	if (ret) {
		clk->parent = old;
		if (clk->prepare_count) {
			clk_disable(parent);
			clk_unprepare(parent);
		}
		goto abort;
	}
	if (clk->prepare_count) {
		clk_disable(old);
		clk_unprepare(old);
	}
	clk_notify(clk, POST_PARENT_CHANGE, old_p_rate, p_rate);
	clk_notify(clk, POST_RATE_CHANGE, rate, clk_get_rate(clk));
	return 0;

abort:
	clk_notify(clk, ABORT_PARENT_CHANGE, old_p_rate, p_rate);
	clk_notify(clk, ABORT_RATE_CHANGE, rate, rate);
	return ret;
}

/* providers, clkdev and the Tegra clock debugfs: nothing looks clocks up by phandle or exposes them here */
struct clk *of_clk_src_simple_get(struct of_phandle_args *clkspec, void *data)
{
	return data;
}

int of_clk_add_provider(struct device_node *np,
			struct clk *(*clk_src_get)(struct of_phandle_args *args, void *data),
			void *data)
{
	return 0;
}

void of_clk_del_provider(struct device_node *np)
{
}

int clk_register_clkdev(struct clk *clk, const char *con_id, const char *dev_fmt, ...)
{
	return 0;
}

/* NOLINTNEXTLINE(misc-use-internal-linkage): clk-dfll.c calls it (declared in the vendored clk.h) */
void tegra_clk_debugfs_add(struct clk *clk);

void tegra_clk_debugfs_add(struct clk *clk)
{
}
