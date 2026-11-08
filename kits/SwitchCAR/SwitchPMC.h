// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 PMC power partitions and IO pad deep power-down, shared by every driver (thread context).
//
// The PMC takes one partition toggle at a time: writing PWRGATE_TOGGLE while START is still set drops the request.
// Every toggle in the system therefore goes through here, under one lock (Linux pmc->powergates_lock); a driver that
// checks START and writes on its own races every other driver doing the same.
//
// Partition numbers are Linux's TEGRA_POWERGATE_*; pads are Linux's tegra210 io_pad .dpd numbers (below 32 an IO_DPD
// bit, from 32 an IO_DPD2 bit). Each bool is false only if the hardware did not acknowledge within its bound.
#ifndef SWITCH_PMC_H
#define SWITCH_PMC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_PMC_PA 0x7000e400ULL

bool switch_pmc_partition_is_on(unsigned partition);
bool switch_pmc_partition_on(unsigned partition);
bool switch_pmc_partition_off(unsigned partition);
void switch_pmc_remove_clamping(unsigned partition);

// tegra_powergate_sequence_power_up: reset asserted, partition on, clock on, clamps off, reset deasserted (10 us
// between steps). Engine quirks (MC hot reset, MBIST work-arounds, clock sources) stay with the engine's driver.
bool switch_pmc_power_up(unsigned partition, unsigned clock_id, unsigned reset_id);
// The same up to the clamps, the reset left asserted for a driver that programs its engine's clocks first (XUSB).
bool switch_pmc_power_up_in_reset(unsigned partition, unsigned clock_id, unsigned reset_id);
// tegra_powergate_power_down: clock on, reset asserted, clock off, partition off.
bool switch_pmc_power_down(unsigned partition, unsigned clock_id, unsigned reset_id);

bool switch_pmc_io_pad_on(unsigned dpd);
bool switch_pmc_io_pad_off(unsigned dpd);

uint32_t switch_pmc_read(uint32_t offset);
void switch_pmc_write(uint32_t offset, uint32_t value);
// Serialised with every other kit user's PMC read-modify-write (thread context).
void switch_pmc_rmw(uint32_t offset, uint32_t clear, uint32_t set);

#ifdef __cplusplus
}
#endif

#endif
