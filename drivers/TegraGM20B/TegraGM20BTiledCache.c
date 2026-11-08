// SPDX-License-Identifier: MIT
#include <core/device.h>
#include <nvxnu.h>
#include <switch_tiled_cache.h>

#include "TegraGM20BTiledCache.h"

void switch_tiled_cache_golden(struct nvkm_device *device)
{
	u32 ctrl[TGM_TC_CTRL_WORDS];
	u32 addr[TGM_TC_ADDR_WORDS];
	u32 i = 0;

	if (!switch_tiled_cache_on()) {
		nvxnu_log("tegra-gm20b: tiled cache off (switch-no-tiled-cache)\n");
		return;
	}
	tgm_tiled_cache_table(ctrl, addr);
	for (i = 0; i < TGM_TC_CTRL_WORDS; i++)
		nvkm_wr32(device, TGM_TC_BUNDLE_CTRL + i * 4, ctrl[i]);
	for (i = 0; i < TGM_TC_ADDR_WORDS; i++)
		nvkm_wr32(device, TGM_TC_BUNDLE_ADDR + i * 4, addr[i]);
	nvxnu_log("tegra-gm20b: tiled cache: tile coalescer table in the golden context\n");
}
