/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: Linux 4.9 timer_list (function + data) on an XNU thread_call (kits/SwitchLinux). The callback runs on a
 * kernel thread, not in softirq context; clk-dfll.c's calibration_timer_cb takes its own spinlock with interrupts off,
 * so that is equivalent. "Deferrable" only matters for idle CPUs in Linux and is ignored.
 */
#ifndef TDFLL_LINUX_TIMER_H
#define TDFLL_LINUX_TIMER_H
#include <linux/kernel.h>
struct timer_list {
	unsigned long expires;
	void (*function)(unsigned long);
	unsigned long data;
	void *tdfll_call;
};
void init_timer(struct timer_list *timer);
#define init_timer_deferrable(t) init_timer(t)
int mod_timer(struct timer_list *timer, unsigned long expires);
int del_timer(struct timer_list *timer);
int del_timer_sync(struct timer_list *timer);
#endif
