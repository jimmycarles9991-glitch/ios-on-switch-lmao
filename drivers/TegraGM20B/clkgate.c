// SPDX-License-Identifier: MIT
/* See TegraGM20BClkgate.h. */
#include <core/device.h>
#include <subdev/top.h>
#include "TegraGM20BClkgate.h"

void tgm_clkgate_load(struct nvkm_device *device)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(tgm_cg_lists); i++)
		for (j = 0; j < tgm_cg_lists[i].count; j++)
			nvkm_wr32(device, tgm_cg_lists[i].regs[j].addr,
				  tgm_cg_lists[i].regs[j].prod);
	nvkm_wr32(device, TGM_THERM_FECS_IDLE_FILTER, 0);
	nvkm_wr32(device, TGM_THERM_HUBMMU_IDLE_FILTER, 0);
}

void tgm_clkgate_enable(struct nvkm_device *device)
{
	static int logged;
	struct nvkm_top_device *info;
	unsigned int engines = 0;

	tgm_clkgate_load(device);
	list_for_each_entry(info, &device->top->device, head)
	{
		u32 reg;

		if (info->engine < 0 ||
		    (info->type != NVKM_ENGINE_GR && info->type != NVKM_ENGINE_CE))
			continue;
		reg = TGM_THERM_GATE_CTRL(info->engine);
		nvkm_wr32(device, reg, tgm_elcg_gate_ctrl(nvkm_rd32(device, reg)));
		engines |= 1u << info->engine;
	}
	if (!logged)
		nvxnu_log("tegra-gm20b: clock gating: SLCG/BLCG (nvgpu gm20b prod lists), ELCG on "
			  "engines %#x\n",
			  engines);
	logged = 1;
}

void tgm_clkgate_disable(struct nvkm_device *device)
{
	struct nvkm_top_device *info;
	unsigned int i, j;

	list_for_each_entry(info, &device->top->device, head)
	{
		u32 reg;

		if (info->engine < 0 ||
		    (info->type != NVKM_ENGINE_GR && info->type != NVKM_ENGINE_CE))
			continue;
		reg = TGM_THERM_GATE_CTRL(info->engine);
		nvkm_wr32(device, reg, tgm_elcg_gate_ctrl_run(nvkm_rd32(device, reg)));
	}
	for (i = 0; i < ARRAY_SIZE(tgm_cg_lists); i++)
		for (j = 0; j < tgm_cg_lists[i].count; j++)
			nvkm_wr32(device, tgm_cg_lists[i].regs[j].addr,
				  tgm_cg_lists[i].regs[j].disable);
}
