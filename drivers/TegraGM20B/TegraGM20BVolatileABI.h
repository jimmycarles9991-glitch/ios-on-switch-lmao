/* SPDX-License-Identifier: MIT */
/* Volatile-mapped write visibility for SubmitJob (TegraGM20BJobABI.h). Kept out of
 * TegraGM20BABI.h and TegraGM20BJobABI.h so //gpu:stack_key does not move. */
#ifndef TEGRA_GM20B_VOLATILE_ABI_H
#define TEGRA_GM20B_VOLATILE_ABI_H
#include <stdint.h>

enum {
	/* GetInfo.flags: SubmitJob accepts TegraGM20BJobVolatileWrites */
	TegraGM20BInfoVolatileWrites = 1u << 29,
	/* TegraGM20BAbiJob.flags: GPU wrote volatile-mapped host memory */
	TegraGM20BJobVolatileWrites = 1u << 9,
};

#endif
