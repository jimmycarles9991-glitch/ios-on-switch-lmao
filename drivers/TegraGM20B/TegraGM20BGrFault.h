/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_GR_FAULT_H
#define TEGRA_GM20B_GR_FAULT_H

/* nouveau's gf100_gr_intr, for each channel error it reports (ILLEGAL_MTHD, ILLEGAL_CLASS, DATA_ERROR, a TRAP other than
 * TEX alone), before it acknowledges it: GR holds the faulting method until then (gpu_fault.c). */
void tgm_gr_fault(int chid);

#endif /* TEGRA_GM20B_GR_FAULT_H */
