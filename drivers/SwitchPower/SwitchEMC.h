// SPDX-License-Identifier: BSD-2-Clause-Patent
// The EMC clock on demand (SwitchEMC.cpp, SwitchEMCGovernor.h).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SwitchPMIC, once the BPMP has set the boot EMC rate (thread context): ACTMON and the work loop ready, the EMC kept at
// that rate until System Boot Complete. One line either way.
void switch_emc_start(void);
// switch_perf_hint's hints (SwitchPower.h), any context: BOOT_DONE starts governing (thread context), the rest raise
// the EMC to its top rate at once and keep it there for a while.
void switch_emc_hint(unsigned int hint);
// The CPU governor's tick (SwitchPerfGovernor.cpp, interrupts masked, one CPU at a time), ending a window in which the
// CPUs were busy for `busy` (timebase ticks, summed over them): wakes the EMC's work loop once when the hold on its top
// rate (frames, CPU work, hints) has ended, or at once when the window was work and the EMC is not held at its top rate.
// A few loads otherwise.
void switch_emc_tick(uint64_t now, uint64_t busy);
// The CPU governor started or stopped (thread context): the hold is programmed again, held for good while there is no
// tick to account for the CPUs' work.
void switch_emc_retick(void);
// SC7, from switch_thermal_suspend/resume (thread context).
void switch_emc_suspend(void);
void switch_emc_resume(void);

// Thread context. Raise before arming a larger scanout; lower only after the old scanout retires.
int switch_emc_display_reserve(uint64_t bytes_per_second);

#if SWITCH_STATS_POWER
// The power stats line's EMC part since the last call: rate changes and the share of time at each rate.
size_t switch_emc_stats(char *line, size_t size);
#endif

#ifdef __cplusplus
}
#endif
