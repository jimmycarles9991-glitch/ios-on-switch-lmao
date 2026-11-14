// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 clock and reset controller (CAR, 0x60006000), shared by every driver.
//
// Clock and reset IDs are Linux's (include/dt-bindings/clock/tegra210-car.h, the same numbers for resets): bank
// id / 32 over L, H, U, V, W, X, Y and bit id % 32. Enable, disable, assert and deassert use the banks' SET/CLR
// registers, which change only their own bit, so they take no lock and are safe from any context. switch_car_rmw is
// the one read-modify-write and serialises with every other kit user (thread context).
#ifndef SWITCH_CAR_H
#define SWITCH_CAR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_CAR_PA     0x60006000ULL
#define SWITCH_CAR_BANKS  7u
#define SWITCH_CAR_MAX_ID (SWITCH_CAR_BANKS * 32u)

void switch_car_clock_enable(unsigned id);
void switch_car_clock_disable(unsigned id);
bool switch_car_clock_enabled(unsigned id);
void switch_car_reset_assert(unsigned id);
void switch_car_reset_deassert(unsigned id);
bool switch_car_reset_asserted(unsigned id);

uint32_t switch_car_read(uint32_t offset);
void switch_car_write(uint32_t offset, uint32_t value);
void switch_car_rmw(uint32_t offset, uint32_t clear, uint32_t set);
// A CAR handshake with no interrupt (a PLL's lock bit): switch_mmio_wait32 on the CAR.
bool switch_car_wait(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_us);

// The fuse cache readable from here on: the FUSE clock on (CLK_ENB_H_SET bit 7) and MISC_CLK_ENB's CFG_ALL_VISIBLE
// (bit 28) set. MISC_CLK_ENB has no set/clear form (TRM 5.2.13), so the kit is its one writer, under its lock. Nothing
// turns either off again: every fuse reader (DFLL speedo, SOC_THERM calibration, GPU fuses, the device key) relies on
// both staying on, and one reader putting them back would zero another's reads.
void switch_car_fuse_readable(void);

#ifdef __cplusplus
}
#endif

#endif
