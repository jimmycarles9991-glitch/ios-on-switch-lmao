// SPDX-License-Identifier: MIT
/* See TegraGM20BClkgate.h. */
#include <core/device.h>
#include <core/engine.h>
#include <subdev/top.h>
#include "TegraGM20BClkgate.h"
#include "TegraGM20BTherm.h"

void tgm_therm_arm(struct nvkm_device *device)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(tgm_therm_setup); i++) {
		const struct tgm_therm_write *w = &tgm_therm_setup[i];
		const u32 old = w->keep ? nvkm_rd32(device, w->addr) : 0;

		nvkm_wr32(device, w->addr, tgm_therm_value(w, old));
	}
}

void tgm_clkgate_load(struct nvkm_device *device)
{
	unsigned int i = 0;
	unsigned int j = 0;

	for (i = 0; i < ARRAY_SIZE(tgm_cg_lists); i++) {
		for (j = 0; j < tgm_cg_lists[i].count; j++) {
			nvkm_wr32(device, tgm_cg_lists[i].regs[j].addr,
				  tgm_cg_lists[i].regs[j].prod);
		}
	}
	nvkm_wr32(device, TGM_THERM_FECS_IDLE_FILTER, 0);
	nvkm_wr32(device, TGM_THERM_HUBMMU_IDLE_FILTER, 0);
	tgm_therm_arm(device);
}

void tgm_clkgate_enable(struct nvkm_device *device)
{
	static int logged;
	struct nvkm_top_device *info = NULL;
	unsigned int engines = 0;

	tgm_clkgate_load(device);
	list_for_each_entry(info, &device->top->device, head)
	{
		u32 reg = 0;

		if (info->engine < 0 ||
		    (info->type != NVKM_ENGINE_GR && info->type != NVKM_ENGINE_CE))
			continue;
		reg = TGM_THERM_GATE_CTRL(info->engine);
		nvkm_wr32(device, reg, tgm_elcg_gate_ctrl(nvkm_rd32(device, reg)));
		engines |= 1u << (unsigned int)info->engine;
	}
	if (!logged) {
		nvxnu_log("tegra-gm20b: clock gating: SLCG/BLCG (nvgpu gm20b prod lists), ELCG on "
			  "engines %#x; THERM slowdown armed: CONFIG2 %08x USE_A %08x\n",
			  engines, nvkm_rd32(device, 0x00020130), nvkm_rd32(device, 0x00020798));
	}
	logged = 1;
}

void tgm_clkgate_disable(struct nvkm_device *device)
{
	struct nvkm_top_device *info = NULL;
	unsigned int i = 0;
	unsigned int j = 0;

	list_for_each_entry(info, &device->top->device, head)
	{
		u32 reg = 0;

		if (info->engine < 0 ||
		    (info->type != NVKM_ENGINE_GR && info->type != NVKM_ENGINE_CE))
			continue;
		reg = TGM_THERM_GATE_CTRL(info->engine);
		nvkm_wr32(device, reg, tgm_elcg_gate_ctrl_run(nvkm_rd32(device, reg)));
	}
	for (i = 0; i < ARRAY_SIZE(tgm_cg_lists); i++) {
		for (j = 0; j < tgm_cg_lists[i].count; j++) {
			nvkm_wr32(device, tgm_cg_lists[i].regs[j].addr,
				  tgm_cg_lists[i].regs[j].disable);
		}
	}
}

/* GR's engine functions with the reset gated (a copy of nouveau's nvkm_gr, which names its classes through fifo.sclass and
 * leaves the trailing sclass list empty, so the copy is whole) and nouveau's own reset */
static struct nvkm_engine_func gr_func;
static int (*gr_reset)(struct nvkm_engine *engine);

static int gated_reset(struct nvkm_engine *engine)
{
	struct nvkm_device *device = engine->subdev.device;
	int ret = 0;

	tgm_clkgate_disable(device);
	ret = gr_reset(engine);
	tgm_clkgate_enable(device);
	return ret;
}

void tgm_clkgate_guard_reset(struct nvkm_device *device)
{
	struct nvkm_engine *gr = nvkm_device_engine(device, NVKM_ENGINE_GR, 0);

	if (!gr || gr->func == &gr_func || !gr->func->reset)
		return;
	gr_func = *gr->func;
	gr_reset = gr->func->reset;
	gr_func.reset = gated_reset;
	gr->func = &gr_func;
}
