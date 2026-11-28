// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power stats line (SwitchPowerStats.cpp), debug kernels only.
#pragma once

#if SWITCH_STATS_POWER
// SwitchThermal, after the CPU governor's periodic line: logs the line when 10 s have passed since the last one.
void switch_power_stats_tick();
#endif
