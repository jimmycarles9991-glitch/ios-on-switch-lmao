// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "TegraGM20BLayout.h"

struct SwitchGpuSurfaceGeometry {
	uint64_t width, height, pitch, bytes;
	uint32_t bpp, element_width, element_height, planes;
	bool client_memory;
};

inline bool switch_gpu_surface_packed(const SwitchGpuSurfaceGeometry &surface)
{
	return surface.width && surface.height && surface.width <= TGM_LAYOUT_MAX_EXTENT &&
	       surface.height <= TGM_LAYOUT_MAX_EXTENT && surface.pitch <= UINT32_MAX &&
	       tgm_layout_tiles_bpp(surface.bpp) && surface.element_width == 1 &&
	       surface.element_height == 1 && !surface.planes &&
	       surface.width * surface.bpp <= surface.pitch &&
	       surface.pitch * surface.height <= surface.bytes;
}

inline bool switch_gpu_surface_shape_matches(const SwitchGpuSurfaceGeometry &surface,
					     uint32_t width, uint32_t height, uint32_t bpp,
					     uint32_t pitch)
{
	return surface.width == width && surface.height == height && surface.bpp == bpp &&
	       surface.pitch == pitch;
}
