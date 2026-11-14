// SPDX-License-Identifier: MIT
// Job interrupts on PMC tree 1, the nonstall line (SwitchGpuNonstall.h)
#include "gpu_internal.h"
#include <subdev/mc.h>
#include "SwitchGpuNonstall.h"

static struct nvkm_inth nonstall_inth;

/* A stall pass that finds the channel bit pending on tree 1 too: acknowledged here, so nvkm counts it handled and never
 * blocks the tree; the pass's scan (shim/xnu_glue.cpp gpuInterrupt) completes the jobs. */
static irqreturn_t nonstall_in_stall_pass(struct nvkm_inth *inth)
{
	nvkm_wr32(inth->intr->subdev->device, SWITCH_GPU_PFIFO_INTR_0,
		  SWITCH_GPU_PFIFO_CHANNEL_INTR);
	return IRQ_HANDLED;
}

/* M1, before nvkm's first init arms its trees: PFIFO on PMC_INTR_MASK_1 as nvkm's own mask, which each rearm (init,
 * resume) writes again. PFIFO_INTR_EN_1 is gk104_fifo_init's. */
int tgm_nonstall_init(struct nvkm_device *device)
{
	int ret = 0;

	if (nonstall_inth.intr)
		return 0;
	ret = nvkm_inth_add(&device->mc->intr, SWITCH_GPU_NONSTALL_VECTOR, NVKM_INTR_PRIO_NORMAL,
			    &device->mc->subdev, nonstall_in_stall_pass, &nonstall_inth);
	if (!ret)
		nvkm_inth_allow(&nonstall_inth);
	return ret;
}
