// SPDX-License-Identifier: MIT
#pragma once

/* Whether a live channel may have work in flight (tgm_busy): a quiet job's end the interrupt pass never saw counts until
 * the settle reads its fence. Read on the CPU governor's tick: loads only. */
static inline int idle_in_flight(void)
{
	return switch_gpu_busy_among(&tgm_busy, tgm_busy.live);
}
