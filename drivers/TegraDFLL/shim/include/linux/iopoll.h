/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: Linux 4.9's atomic register poll, as include/linux/iopoll.h has it: read until the condition holds, at
 * most until the timeout, then once more. Its one user is the DFLL monitor's DATA_NEW flag (clk-dfll.c
 * dfll_wait_monitor_data, two sample periods = 160 us at the DT's 12.5 kHz), which has no interrupt line (the DFLL's
 * SPI 62 only reports output min/max limits, and L4T leaves it masked: DFLL_INTR_EN = 0); it runs under the DFLL
 * spinlock with interrupts off and usually ends after one sample period. A miss returns -ETIMEDOUT exactly as Linux
 * does (clk-dfll.c tolerates it: dfll_get_monitor_data compares two consecutive readings) and is counted for
 * tegra_dfll_cpu_describe().
 */
#ifndef TDFLL_LINUX_IOPOLL_H
#define TDFLL_LINUX_IOPOLL_H
#include <linux/kernel.h>
int tdfll_poll_timed_out(void);
#define readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) ({ \
	const u64 __end = switch_linux_nanotime() + (u64)(timeout_us) * NSEC_PER_USEC; \
	for (;;) { \
		(val) = op(addr); \
		if (cond) \
			break; \
		if (switch_linux_nanotime() > __end) { \
			(val) = op(addr); \
			break; \
		} \
		if (delay_us) \
			udelay(delay_us); \
	} \
	(cond) ? 0 : tdfll_poll_timed_out(); })
#define readl_relaxed_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout_atomic(readl_relaxed, addr, val, cond, delay_us, timeout_us)
#endif
