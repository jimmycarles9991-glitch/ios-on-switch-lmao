/* SPDX-License-Identifier: MIT */
/*
 * Zcull's layout from GR's registers, as nvgpu reports it to its clients (L4T r32.7.6 gk20a/gr_gk20a.c:
 * gr_gk20a_init_zcull, gr_gk20a_get_zcull_info; constants from hw/gm20b/hw_gr_gm20b.h and hw_proj_gm20b.h).
 * Copyright (c) 2011-2020, NVIDIA CORPORATION.  All rights reserved.
 * test/gm20b_zcull_test.cpp checks it.
 */
#ifndef TEGRA_GM20B_ZCULL_H
#define TEGRA_GM20B_ZCULL_H
#include <stdint.h>
#include "TegraGM20BZcullABI.h"

#define TGM_ZCULL_FS_GPC 0x00502608   /* gr_gpc0_fs_gpc_r: num_available_zculls in 20:16 */
#define TGM_ZCULL_RAM_SIZE 0x00500920 /* gr_gpc0_zcull_total_ram_size_r: num_aliquots in 15:0 */
#define TGM_ZCULL_CTXSW_ALIGN 0x1000

/* GR of `gpcs` GPCs, `tpcs` TPCs of which `gpc0_tpcs` in GPC 0, its zcull registers and FECS's zcull image size
 * (discover_zcull_image_size): 0 where the GPU reports no zcull RAM or a register read back 0xbadf (unimplemented).
 * GM20B has one GPC (proj_scal_litter_num_gpcs_v), which the header sizes are per. The X1 reads 0x00040002 (two TPCs, four
 * zcull banks) and 0x01ff0800 (2048 aliquots): x1-stacklog's register dumps. */
static inline int tgm_zcull_layout(uint32_t gpcs, uint32_t tpcs, uint32_t gpc0_tpcs,
				   uint32_t fs_gpc, uint32_t ram_size, uint32_t ctxsw_size,
				   struct TegraGM20BAbiZcullInfo *out)
{
	const uint32_t zcbs = (fs_gpc >> 16) & 0x1f;
	const uint32_t aliquots = ram_size & 0xffff;

	if ((fs_gpc >> 16) == 0xbadf || (ram_size >> 16) == 0xbadf)
		return 0;
	if (!gpcs || !tpcs || !gpc0_tpcs || !zcbs || !aliquots || !ctxsw_size)
		return 0;
	out->width_align_pixels = tpcs * 16;
	out->height_align_pixels = 32;
	out->pixel_squares_by_aliquots = zcbs * 16 * 16 * tpcs / (gpcs * gpc0_tpcs);
	out->aliquot_total = aliquots;
	out->region_byte_multiplier = gpcs * 0x20; /* gr_zcull_bytes_per_aliquot_per_gpu_v */
	out->region_header_size = 1 * 0x20; /* gr_zcull_save_restore_header_bytes_per_gpc_v */
	out->subregion_header_size =
		1 * 0xc0;            /* gr_zcull_save_restore_subregion_header_bytes_per_gpc_v */
	out->subregion_count = 0x10; /* gr_zcull_subregion_qty_v */
	out->subregion_width_align_pixels =
		tpcs * 0x10; /* gr_gpc0_zcull_zcsize_width_subregion__multiple_v */
	out->subregion_height_align_pixels =
		0x40; /* gr_gpc0_zcull_zcsize_height_subregion__multiple_v */
	out->ctxsw_size = ctxsw_size;
	out->ctxsw_align = TGM_ZCULL_CTXSW_ALIGN;
	return 1;
}

#endif
