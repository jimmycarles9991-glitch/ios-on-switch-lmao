// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power stats line (SwitchPowerStats.cpp), debug kernels only.
#pragma once

#if DEVELOPMENT || DEBUG
// SwitchThermal, after the CPU governor's periodic line: logs the line when 10 s have passed since the last one. Sleeps on I2C1.
void switch_power_stats_tick(void);
#endif
