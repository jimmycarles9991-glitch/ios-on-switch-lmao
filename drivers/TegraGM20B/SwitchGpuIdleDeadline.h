// SPDX-License-Identifier: MIT
#pragma once

#include "SwitchGpuIdleCount.h"

/* drivers/SwitchPower SwitchPerfGovernor.cpp, on the CPU governor's tick (XNU's perfcontrol deadline: interrupts
 * masked, scheduler context): a few loads, and a work item queued once the GPU has been idle long enough. */
void tegra_gm20b_idle_tick(void)
{
	u64 now = 0;
	int expected = 0;

	if (!READ_ONCE(tgm_idle.on) || READ_ONCE(tgm_idle.low) || READ_ONCE(tgm_idle.dropping))
		return;
	now = nvxnu_nanotime();
	/* seqnos in flight with no kick for the drop time: a quiet job's end may have come with no interrupt to count it */
	if (idle_in_flight()) {
		if (now - __atomic_load_n(&tgm_idle.last_busy, __ATOMIC_SEQ_CST) >= IDLE_DROP_NS &&
		    __atomic_compare_exchange_n(&tgm_idle.settling, &expected, 1, false,
						__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			nvxnu_work_schedule(tgm_idle.settle_w);
		return;
	}
	if (now < __atomic_load_n(&tgm_idle.service_until, __ATOMIC_SEQ_CST) ||
	    now - __atomic_load_n(&tgm_idle.last_busy, __ATOMIC_SEQ_CST) < IDLE_DROP_NS ||
	    now - native_display_last_swap_ns() < IDLE_DROP_NS)
		return;
	if (__atomic_compare_exchange_n(&tgm_idle.dropping, &expected, 1, false, __ATOMIC_SEQ_CST,
					__ATOMIC_SEQ_CST))
		nvxnu_work_schedule(tgm_idle.drop_w);
}

void tegra_gm20b_perf_deadline_ns(uint64_t until)
{
	const uint64_t now = nvxnu_nanotime();
	const uint64_t window = 20000000;
	if (!READ_ONCE(tgm_idle.on) || until <= now)
		return;
	if (until - now > window)
		until = now + window;
	uint64_t held = __atomic_load_n(&tgm_idle.service_until, __ATOMIC_SEQ_CST);
	while (held < until &&
	       !__atomic_compare_exchange_n(&tgm_idle.service_until, &held, until, false,
					    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
	}
	__atomic_store_n(&tgm_idle.last_busy, now, __ATOMIC_SEQ_CST);
	if (READ_ONCE(tgm_idle.low) || READ_ONCE(tgm_idle.dropping))
		nvxnu_work_schedule(tgm_idle.raise_w);
}
