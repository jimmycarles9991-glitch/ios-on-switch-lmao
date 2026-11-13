/* SPDX-License-Identifier: MIT */
/*
 * Rail-gating with the display off (TegraGM20B.cpp "rail"), the decision only, kept here for the host test
 * (test/gm20b_rail_test.cpp). The GPU goes down through system sleep's own steps (gpu_power.c tgm_rail_down, VDD_GPU off)
 * once the display is off and the GPU idle (at its idle clock: 500 ms without a submission or a swap, gpu_clock.c idle_drop),
 * so the display's last frames finish at full speed. It comes back on the rail's thread call as the panel starts to
 * wake, so the unrail runs under the panel's power-up, or at once in whichever caller takes tgm_gpu.lock or enters the gate
 * while it is down (a demand, gpu_bringup.c tgm_gpu_lock). Each demand with the display off holds the GPU up longer before it may go
 * down again, so work that keeps coming back does not pay a rail cycle each time. System sleep owns the GPU from its
 * suspend to its resume: nothing moves the rail meanwhile, and a GPU already down stays down across it.
 */
#ifndef TEGRA_GM20B_RAIL_H
#define TEGRA_GM20B_RAIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tgm_rail_move {
	TGM_RAIL_STAY = 0,
	TGM_RAIL_DOWN = 1,
	TGM_RAIL_UP = 2,
};

#define TGM_RAIL_HOLD_NS 1000000000ULL
#define TGM_RAIL_HOLD_DOUBLINGS 6

/* After `demands` wakes by work since the display went off, how long the GPU stays up after the last before it may go
 * down: none before the first, then 1 s doubling to 64 s. */
static inline uint64_t tgm_rail_hold_ns(uint32_t demands)
{
	if (!demands)
		return 0;
	return TGM_RAIL_HOLD_NS << (demands - 1 < TGM_RAIL_HOLD_DOUBLINGS
					    ? demands - 1
					    : TGM_RAIL_HOLD_DOUBLINGS);
}

/* GPU time never goes backwards across a rail cycle or system sleep: PTIMER restarts from ktime at every wake
 * (nvkm_timer_init) and the GPU's clock may have run ahead of ktime, so each time handed out, and PTIMER after a wake, is
 * at least the last one before it (gpu_bringup.c tgm_stamp_last). */
static inline uint64_t tgm_stamp_monotonic(uint64_t last, uint64_t now)
{
	return now > last ? now : last;
}

/* PTIMER read right after a wake's nvkm_timer_init: what to set it to so it is past `last`, every GPU time before the
 * power-down (a power-up's times then lie below the next power-up's clock anchor, TegraGM20BClockABI.h), or 0 where it
 * already is */
static inline uint64_t tgm_stamp_reseed(uint64_t last, uint64_t ptimer)
{
	return ptimer > last ? 0 : last + 1;
}

/* `up`: the GPU is powered; `display_on`: the backlight's last word; `idle`: the GPU is at its idle clock; `held`: a
 * demand's hold has not run out; `sc7`: system sleep (or the bring-up) holds the GPU. */
static inline enum tgm_rail_move tgm_rail_next(int up, int display_on, int idle, int held, int sc7)
{
	if (sc7)
		return TGM_RAIL_STAY;
	if (!up)
		return display_on ? TGM_RAIL_UP : TGM_RAIL_STAY;
	return !display_on && idle && !held ? TGM_RAIL_DOWN : TGM_RAIL_STAY;
}

#ifdef __cplusplus
}
#endif

#endif
