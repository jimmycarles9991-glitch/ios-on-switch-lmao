/* SPDX-License-Identifier: MIT */
/* Debug-only GR context preemption option (offset 0x60 in the FECS ctxsw main image, nvgpu
 * hw_ctxsw_prog_gm20b.h ctxsw_prog_main_image_preemption_options_o): selects WFI (0) or CTA (1)
 * on one channel while quiesced. Release kernels answer kIOReturnUnsupported. Kept out of
 * TegraGM20BABI.h so //gpu:stack_key does not move. */
#ifndef TEGRA_GM20B_GR_PREEMPT_ABI_H
#define TEGRA_GM20B_GR_PREEMPT_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodGrPreempt = 0x4750, /* 'GP' */

	TegraGM20BGrPreemptWfi = 0,
	TegraGM20BGrPreemptCta = 1,
};

struct TegraGM20BAbiGrPreempt {
	uint32_t channel;
	uint32_t mode;
	uint32_t reserved[2];
};

struct TegraGM20BAbiGrPreemptReply {
	uint32_t old_mode;
	uint32_t readback;
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiGrPreempt) == 16, "TegraGM20BAbiGrPreempt layout");
static_assert(sizeof(TegraGM20BAbiGrPreemptReply) == 8, "TegraGM20BAbiGrPreemptReply layout");
#endif
#endif
