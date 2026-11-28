/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: the hrtimer subset clk-dfll.c uses for its low->high tuning state machine (dfll_tune_timer_cb), on an
 * XNU thread_call. Only HRTIMER_MODE_REL and HRTIMER_NORESTART are used by the vendored code (a callback that wants to
 * run again calls hrtimer_start itself), so HRTIMER_RESTART is not supported.
 */
#ifndef TDFLL_LINUX_HRTIMER_H
#define TDFLL_LINUX_HRTIMER_H
#include <linux/kernel.h>
enum hrtimer_mode { HRTIMER_MODE_ABS = 0x0, HRTIMER_MODE_REL = 0x1 };
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
#define CLOCK_MONOTONIC 1
struct hrtimer {
	enum hrtimer_restart (*function)(struct hrtimer *);
	void *tdfll_call;
};
void hrtimer_init(struct hrtimer *timer, int which_clock, enum hrtimer_mode mode);
void hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode);
int hrtimer_cancel(struct hrtimer *timer);
int hrtimer_try_to_cancel(struct hrtimer *timer);
#endif
