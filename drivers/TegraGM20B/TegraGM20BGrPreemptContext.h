/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_GR_PREEMPT_CONTEXT_H
#define TEGRA_GM20B_GR_PREEMPT_CONTEXT_H

#include <core/gpuobj.h>
#include "TegraGM20BGrPreempt.h"

static inline void tgm_gr_preempt_image_bind(struct nvkm_gpuobj *ctx, u32 size, u32 options)
{
	if (options && size >= TGM_GR_CTX_PREEMPT_MIN_SIZE)
		nvkm_wo32(ctx, TGM_GR_CTX_PREEMPT_OPTIONS, options);
}

#endif
