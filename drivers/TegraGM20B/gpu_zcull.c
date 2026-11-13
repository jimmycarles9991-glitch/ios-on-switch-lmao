// SPDX-License-Identifier: MIT
// Zcull's layout (TegraGM20BZcull.h), read once GR is up: the registers are the GPU's fixed configuration.
#include "gpu_internal.h"
#include "TegraGM20BZcull.h"

static struct TegraGM20BAbiZcullInfo tgm_zcull;
static int tgm_zcull_valid;

void tgm_zcull_init(struct nvkm_device *device)
{
	struct gf100_gr *gr = gf100_gr(device->gr);

	tgm_zcull_valid = tgm_zcull_layout(
		gr->gpc_nr, gr->tpc_total, gr->tpc_nr[0], nvkm_rd32(device, TGM_ZCULL_FS_GPC),
		nvkm_rd32(device, TGM_ZCULL_RAM_SIZE), gr->size_zcull, &tgm_zcull);
	nvxnu_step("tegra-gm20b: zcull %s: %u aliquots, %u pixel squares an aliquot, ctxsw image "
		   "%u bytes\n",
		   tgm_zcull_valid ? "on" : "off", tgm_zcull.aliquot_total,
		   tgm_zcull.pixel_squares_by_aliquots, gr->size_zcull);
}

int tgm_zcull_info(struct TegraGM20BAbiZcullInfo *out)
{
	if (!tgm_zcull_valid)
		return -95; /* EOPNOTSUPP */
	*out = tgm_zcull;
	return 0;
}
