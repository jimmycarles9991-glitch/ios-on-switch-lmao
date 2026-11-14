// SPDX-License-Identifier: MIT
// GPU PMC_ENABLE's one lock (TegraGM20BPmcEnable.h): nvkm's mc functions, wrapped at M1 before any engine runs.
#include "gpu_internal.h"
#include <subdev/mc/priv.h>
#include "TegraGM20BPmcEnable.h"

#define PMC_ENABLE 0x000200

static DEFINE_SPINLOCK(pmc_lock);
static struct nvkm_mc_func pmc_func;
static struct nvkm_mc_device_func pmc_device;

static u32 pmc_read(void *ctx)
{
	return nvkm_rd32((struct nvkm_device *)ctx, PMC_ENABLE);
}

static void pmc_write(void *ctx, u32 value)
{
	nvkm_wr32((struct nvkm_device *)ctx, PMC_ENABLE, value);
}

static unsigned long pmc_lock_irqsave(void *ctx)
{
	unsigned long flags = 0;

	(void)ctx;
	spin_lock_irqsave(&pmc_lock, flags);
	return flags;
}

static void pmc_unlock(void *ctx, unsigned long flags)
{
	(void)ctx;
	spin_unlock_irqrestore(&pmc_lock, flags);
}

void tgm_pmc_change(struct nvkm_device *device, u32 mask, u32 value)
{
	const struct tgm_pmc_enable_io io = {device, pmc_read, pmc_write, pmc_lock_irqsave,
					     pmc_unlock};

	tgm_pmc_enable_change(&io, mask, value);
}

static void pmc_disable(struct nvkm_mc *mc, u32 mask)
{
	tgm_pmc_change(mc->subdev.device, mask, 0);
}

static void pmc_enable(struct nvkm_mc *mc, u32 mask)
{
	tgm_pmc_change(mc->subdev.device, mask, mask);
}

/* nv50_mc_init, gk20a's mc init: every engine out of reset, the word written whole */
static void pmc_init(struct nvkm_mc *mc)
{
	tgm_pmc_change(mc->subdev.device, 0xffffffff, 0xffffffff);
}

void tgm_pmc_own(struct nvkm_device *device)
{
	struct nvkm_mc *mc = device->mc;

	if (!mc || mc->func == &pmc_func)
		return;
	pmc_func = *mc->func;
	pmc_device = *mc->func->device;
	pmc_device.enable = pmc_enable;
	pmc_device.disable = pmc_disable;
	pmc_func.device = &pmc_device;
	if (mc->func->init == nv50_mc_init)
		pmc_func.init = pmc_init;
	mc->func = &pmc_func;
}
