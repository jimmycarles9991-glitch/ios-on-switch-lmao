// SPDX-License-Identifier: GPL-2.0
// The console as tegra_dfll_cpu_prepare() meets it on the host: the CAR, fuse, APB_MISC, I2C5 and DFLL registers in
// memory, VDD_CPU's MAX77812 on TegraI2C and the L4T probe (tegra_dfll_fakes.c), each with the answer the test sets.
#ifndef TEGRA_DFLL_FAKE_H
#define TEGRA_DFLL_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tegra_dfll_fake {
	int speedo_id, speedo_value, process_id;
	int pmic_answer;   // TegraI2C's result for each MAX77812 read
	int probe_answer;  // the L4T probe's
	unsigned pmic_reads, probes, maps, lines;
	// TRM 35.4: an I2C5 transfer while CL-DVFS is out of reset with its clock stopped hangs the bus
	unsigned i2c5_hangs;
	// CAR: CL-DVFS out of reset with its clock stopped after a write (TRM 35.4), and a write of CLK_OUT_ENB_x or
	// RST_DEVICES_x itself instead of its _SET/_CLR register
	unsigned cl_dvfs_unsafe, car_direct_writes;
};

// Every CAR write and delay in order: a delay is offset TEGRA_DFLL_FAKE_DELAY with its microseconds as the value.
#define TEGRA_DFLL_FAKE_DELAY 0xffffffffu

struct tegra_dfll_fake_write {
	uint32_t offset, value;
};
extern struct tegra_dfll_fake_write tegra_dfll_fake_car_log[512];
extern unsigned tegra_dfll_fake_car_writes;

extern struct tegra_dfll_fake tegra_dfll_fake;

enum tegra_dfll_fake_block {
	TEGRA_DFLL_FAKE_CAR,
	TEGRA_DFLL_FAKE_FUSE,
	TEGRA_DFLL_FAKE_APBMISC,
	TEGRA_DFLL_FAKE_I2C5,
	TEGRA_DFLL_FAKE_DFLL,
	TEGRA_DFLL_FAKE_BLOCKS
};

// A register of one block, as the driver reads and writes it. Blocks map on first use; `refuse` makes a block's
// mapping fail until cleared.
volatile uint32_t *tegra_dfll_fake_reg(enum tegra_dfll_fake_block block, uint32_t offset);
extern bool tegra_dfll_fake_refuse_map[TEGRA_DFLL_FAKE_BLOCKS];

// A T210B01 A01 with I2C5 clocked, the CPU on PLLX in RUN, speedo fused and the PMIC answering, CL-DVFS as hekate
// leaves it (clock on, out of reset, its sources at their reset value clk_m / 1) with the DFLL disabled.
void tegra_dfll_fake_console(void);

// The CAR's CL-DVFS bits and sources at their reset values (TRM 5.2.115, 5.2.117, 5.2.245-246), as after SC7.
void tegra_dfll_fake_cl_dvfs_reset(void);

#ifdef __cplusplus
}
#endif

#endif
