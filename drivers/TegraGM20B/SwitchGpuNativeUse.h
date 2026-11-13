// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuNativeSurface.h"

static inline int switch_gpu_native_surface_add_read(const switch_gpu_native_surface_view &view,
						     uint32_t access, switch_gpu_backing_use *uses,
						     switch_gpu_backing_guard *guards,
						     uint32_t capacity, uint32_t &useCount,
						     uint32_t &guardCount)
{
	const uint32_t guardNeed = view.payload.generation ? 2 : 1;
	if (access != SWITCH_GPU_BACKING_READ)
		return -22;
	if (useCount > capacity || capacity - useCount < 2 || guardCount > capacity ||
	    capacity - guardCount < guardNeed)
		return -28;
	uses[useCount++] = {view.native, SWITCH_GPU_BACKING_READ, 0};
	uses[useCount++] = {view.logical, SWITCH_GPU_BACKING_READ, 0};
	if (view.payload.generation)
		guards[guardCount++] = view.payload;
	guards[guardCount++] = view.representation;
	return 0;
}
