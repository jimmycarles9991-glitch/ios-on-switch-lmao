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
// SC7, from switch_thermal_suspend/resume (thread context).
void switch_emc_suspend(void);
void switch_emc_resume(void);

// Thread context. Raise before arming a larger scanout; lower only after the old scanout retires.
int switch_emc_display_reserve(uint64_t bytes_per_second);

#if DEVELOPMENT || DEBUG
// The power stats line's EMC part since the last call: rate changes and the share of time at each rate.
size_t switch_emc_stats(char *line, size_t size);
#endif

#ifdef __cplusplus
}
#endif
