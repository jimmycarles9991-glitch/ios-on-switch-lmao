// SPDX-License-Identifier: BSD-2-Clause-Patent
// Where the Switch stops itself for heat, in order, all from L4T for this console (switchroot's OLED device tree):
//   * thermalmonitord's own mitigation and its "needs to cool down" state run first, on the sensors SwitchSensors gives
//     it (CONTRACT.md), and SwitchThermal's fan curve and DFLL limits below 90 C;
//   * software thermal shutdown, orderly through reboot(2)'s path with a record the next boot files: the SoC diode at
//     L4T's Tdiode_tegra critical trip (gpu_shutdown, 92.5 C), or SOC_THERM's PLLX group at its PLL-therm "hot" trip
//     (cpu_heavy, 94.5 C, where L4T would start hardware throttling);
//   * hardware THERMTRIP, the PMC's reset, only above that: PLLX at the DT's nvidia,thermtrips (96 C), CPU and GPU at L4T's
//     127 C default (SwitchSocThermWindow.h).
#ifndef SWITCH_THERMAL_LIMITS_H
#define SWITCH_THERMAL_LIMITS_H

#define SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC 92500
#define SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC  94500
#define SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC 96000
#define SWITCH_THERMAL_THERMTRIP_CPU_MILLIC  127000
#define SWITCH_THERMAL_THERMTRIP_GPU_MILLIC  127000

#endif
