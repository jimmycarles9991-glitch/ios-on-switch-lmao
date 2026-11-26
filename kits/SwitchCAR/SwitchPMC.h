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
// switch_pmc_remove_clamping, then REMOVE_CLAMPING_CMD's bit read until it clears (100 us). False if it stays.
bool switch_pmc_unclamp(unsigned partition);

// A partition's memory client in MC_CLIENT_HOTRESET_CTRL (0x200) or CTRL_1 (0x970), released through the MC's locked
// read-modify-write (TegraSMMU.h tegra_mc_hot_reset).
struct switch_pmc_mc_client {
    uint32_t ctrl, bit;
    void (*hot_reset)(uint32_t ctrl, uint32_t bit, bool on);
};

// L4T powergate-t21x.c __tegra1xx_unpowergate: partition on, clock on, reset asserted, clamps removed (until
// REMOVE_CLAMPING_CMD's bit clears, 100 us), reset deasserted, the memory client's hot reset released (`client` NULL: the
// partition has none, or its driver releases it), 10 us between steps. Clock sources go first (the caller's), and
// engine quirks (MBIST work-arounds) stay with the engine's driver. False if the partition did not power on or its
// clamps stayed; the sequence runs to its end either way.
bool switch_pmc_power_up(unsigned partition, unsigned clock_id, unsigned reset_id,
                         const struct switch_pmc_mc_client *client);
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
