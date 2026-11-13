/* SPDX-License-Identifier: MIT */
#pragma once
#include "SwitchGpuBacking.h"

struct switch_gpu_native_surface_view {
	void *owner, *descriptor;
	struct switch_gpu_backing *logical, *native;
	struct switch_gpu_backing_guard payload, representation;
	struct switch_gpu_backing_token producer;
	struct switch_gpu_backing_layout layout;
	uint32_t crop_x, crop_y, crop_width, crop_height;
};

#ifdef __cplusplus
extern "C" {
#endif
int switch_native_surface_gpu_acquire(void *surface, struct switch_gpu_native_surface_view *view);
void switch_native_surface_gpu_release(struct switch_gpu_native_surface_view *view);
#ifdef __cplusplus
}
#endif
