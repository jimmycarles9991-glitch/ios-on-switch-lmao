/* SPDX-License-Identifier: MIT */
/* Zcull, the GPU's coarse depth rejection: its layout, for clients that program zcull regions. The stack key does not
 * hash this header, so no unit it hashes (gpu/BUILD.bazel stack_key) may include it. GetInfo's zcull[] holds the same
 * record, with TegraGM20BInfoZcull set, wherever this method answers. */
#ifndef TEGRA_GM20B_ZCULL_ABI_H
#define TEGRA_GM20B_ZCULL_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodZcullInfo = 0x5a43, /* 'ZC' */
};

/* Method 'ZC', no input. drm_nouveau_get_zcull_info's layout (Mesa nv_zcull_device_info), nvgpu's
 * gr_gk20a_get_zcull_info values. Every GR channel's context saves its zcull RAM in a buffer of its own (ctxsw_size bytes,
 * ctxsw mode separate buffer), so a channel's zcull regions hold across context switches. kIOReturnUnsupported: the GPU
 * reported no zcull RAM. */
struct TegraGM20BAbiZcullInfo {
	uint32_t width_align_pixels;
	uint32_t height_align_pixels;
	uint32_t pixel_squares_by_aliquots;
	uint32_t aliquot_total;
	uint32_t region_byte_multiplier;
	uint32_t region_header_size;
	uint32_t subregion_header_size;
	uint32_t subregion_count;
	uint32_t subregion_width_align_pixels;
	uint32_t subregion_height_align_pixels;
	uint32_t ctxsw_size;
	uint32_t ctxsw_align;
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiZcullInfo) == 48, "TegraGM20BAbiZcullInfo layout");
#else
_Static_assert(sizeof(struct TegraGM20BAbiZcullInfo) == 48, "TegraGM20BAbiZcullInfo layout");
#endif

#endif
