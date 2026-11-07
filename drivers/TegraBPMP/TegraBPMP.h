// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraBPMP: the CPU side of the T210 BPMP mailbox (L4T's drivers/firmware/tegra mail.c / mail_t210.c / mailman.c,
// vendored under linux/ with an XNU shim), and the one thing Switch asks the BPMP firmware for: the EMC (DRAM)
// clock, which on T210B01 belongs to the BPMP-FW that hekate starts (TegraBPMP.c says how and why).
#ifndef TEGRA_BPMP_PUBLIC_H
#define TEGRA_BPMP_PUBLIC_H
#ifdef __cplusplus
extern "C" {
#endif

// Brings the mailbox up (once), then sets the EMC clock to `khz` exactly as L4T's clk_set_rate(emc, ...) does on
// T210B01: MRQ_CLK on BPMP clock 0. Thread context; sleeps on BPMP answers (interrupt-driven) and on I2C5. Fills
// `line` with one console line (before -> after, firmware tag, the BPMP's EMC table, the core-voltage check) and
// returns 0, or a negative errno with `line` saying why the EMC clock was left as it was. A BPMP answer whose
// doorbell interrupt never arrives panics. Call with khz 0 to bring the mailbox up and only report.
int tegra_bpmp_emc_boot(unsigned int khz, char *line, unsigned long size);

// System sleep (drivers/SwitchSuspend), thread context with interrupts on, nothing else using the mailbox: suspend
// brings the mailbox up if it is not, lowers EMC to 204 MHz, tells the firmware the next power-down is SC7 and returns
// -EBUSY unless every mailbox channel is then free (tegra_bpmp_semaphore); resume asks for the EMC rate suspend found.
// Each fills `line` and returns 0 or a negative errno.
int tegra_bpmp_sc7_suspend(char *line, unsigned long size);
int tegra_bpmp_sc7_resume(char *line, unsigned long size);

// The EMC governor (drivers/SwitchPower/SwitchEMC.cpp), once tegra_bpmp_emc_boot has brought the mailbox up; thread
// context, each one MRQ that sleeps on the BPMP's answer. rates: the firmware's EMC table in kHz (MRQ_EMC_DVFS_TABLE),
// up to `max`, returning how many or a negative errno. get: the EMC clock now. set: MRQ_CLK SET_RATE on clock 0 to one
// of those rates, the clock it then runs in *now_khz; -EBUSY from SC7 entry (tegra_bpmp_sc7_suspend) to its resume.
int tegra_bpmp_emc_rates(unsigned int *khz, unsigned int max);
int tegra_bpmp_emc_get(unsigned int *khz);
int tegra_bpmp_emc_set(unsigned int khz, unsigned int *now_khz);

// The mailbox's arbitration semaphore status, two bits a channel; 0 before the mailbox is up. ATF's SYSTEM_SUSPEND
// (switch-l4t-atf plat_psci_handlers.c, drivers/bpmp/bpmp.c tegra_bpmp_init) takes the BPMP firmware for running only
// while this reads SIGN_OF_LIFE, every channel free; otherwise it powers the BPMP off and runs an SC7 entry firmware,
// which this boot chain does not have (hekate passes one on T210 only), and never reaches SC7. Any context.
#define TEGRA_BPMP_SIGN_OF_LIFE 0xAAAAAAAAu
unsigned int tegra_bpmp_semaphore(void);

#ifdef __cplusplus
}
#endif
#endif
