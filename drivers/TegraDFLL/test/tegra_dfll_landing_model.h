// SPDX-License-Identifier: GPL-2.0
// The vendored clk-dfll.c in closed loop over a DFLL whose loop settles at once (Tegra X1 TRM 6.1.6 DFLL_FREQ_REQ,
// 6.1.9 DFLL_OUTPUT_CFG): the ring oscillator runs at MULT x 25.5 MHz unless that is under its rate at OUTPUT_CFG MIN
// (then it runs there, pinned) or over its rate at MAX; OUTPUT_FORCE holds the output; the CPU gets the oscillator
// through the SCALE skipper. The oscillator's rate is linear in the LUT index, faster with the low tune (TUNE1), and
// DFLL_MONITOR_DATA counts it in REF_CLK / 4 units, truncated. Time moves only by udelay and run_ms; the shim's
// timers (Vmin calibration, low-to-high tuning) fire when due.
#ifndef TEGRA_DFLL_LANDING_MODEL_H
#define TEGRA_DFLL_LANDING_MODEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A T210B01-shaped DFLL closed at `rate_hz`, high tune, its oscillator at `floor_hz` with the output at the tune-high
// floor (850 mV, LUT 855 mV) and `per_step_hz` faster per 15 mV LUT step.
int tegra_dfll_landing_start(unsigned long floor_hz, unsigned long per_step_hz,
			     unsigned long rate_hz);
// As clk_set_rate's path asks it: under the DFLL lock.
int tegra_dfll_landing_request(unsigned long rate_hz);
void tegra_dfll_landing_run_ms(unsigned int ms);
void tegra_dfll_landing_set_floor(unsigned long floor_hz);
// From the next request on, the output comes down from the request's forced value (DFLL_FREQ_REQ FORCE_VAL) at
// `steps_per_s` LUT steps a second instead of at once, the oscillator running at that output meanwhile.
void tegra_dfll_landing_set_slew(unsigned long steps_per_s);
unsigned long tegra_dfll_landing_cpu_hz(void);
bool tegra_dfll_landing_pinned(void);  // at OUTPUT_CFG MIN with the oscillator over its target
unsigned long tegra_dfll_landing_dvco_rate_min(void);
// Allocations, sleeps or mutexes taken with interrupts off (the DFLL lock is spin_lock_irqsave)
unsigned int tegra_dfll_landing_irqs_off_blocking(void);

#ifdef __cplusplus
}
#endif

#endif
