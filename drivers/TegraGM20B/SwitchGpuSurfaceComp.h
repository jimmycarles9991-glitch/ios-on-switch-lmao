// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GPU_KIND_C32_2CRA 0xdbu
#define SWITCH_GPU_KIND_C64_2CRA 0xe9u

static inline int switch_gpu_surface_is_panel_scanout(uint32_t width, uint32_t height)
{
	return (width == 720u && height == 1280u) || (width == 1280u && height == 720u);
}

static inline int switch_gpu_surface_is_external_scanout(uint32_t width, uint32_t height,
							 uint32_t bpp, int compositor,
							 int external_active)
{
	if (!external_active || !compositor || bpp != 4u || !width || !height)
		return 0;
	return width <= 4096u && height <= 4096u;
}

static inline uint32_t switch_gpu_surface_comp_kind(uint32_t width, uint32_t height, uint32_t bpp,
						    uint64_t alloc_bytes, uint64_t layout_bytes,
						    int scanned, int comp_enabled, int smmu_ready)
{
	if (!comp_enabled || !smmu_ready || scanned)
		return 0;
	if (!alloc_bytes || (alloc_bytes & 0xffffULL) != 0ULL || layout_bytes > alloc_bytes)
		return 0;
	if (bpp == 8u)
		return SWITCH_GPU_KIND_C64_2CRA;
	if (bpp == 4u && !switch_gpu_surface_is_panel_scanout(width, height))
		return SWITCH_GPU_KIND_C32_2CRA;
	return 0;
}

static inline int switch_gpu_surface_kind_compressed(uint32_t kind)
{
	return kind == SWITCH_GPU_KIND_C32_2CRA || kind == SWITCH_GPU_KIND_C64_2CRA;
}

#ifdef __cplusplus
}
#endif
