// SPDX-License-Identifier: MIT
#include <core/device.h>
#include <nvxnu.h>
#include <switch_nvn_golden.h>

#include "TegraGM20BNvnGolden.h"

void switch_nvn_golden(struct nvkm_device *device)
{
	const u32 skipped = switch_nvn_golden_skipped();
	u32 written = 0;
	u32 i = 0;

	for (i = 0; i < TGM_NVN_GOLDEN_WRITES; i++) {
		const struct tgm_masked_write *w = &tgm_nvn_golden[i];
		if (skipped & (1u << i))
			continue;
		nvkm_mask(device, w->reg, w->mask, w->value & w->mask);
		written |= 1u << i;
	}
	nvxnu_log("tegra-gm20b: NVN golden-context writes %#x of %#x (0x404468, 0x418800, "
		  "0x419a04)\n",
		  written, (1u << TGM_NVN_GOLDEN_WRITES) - 1u);
}
