// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#include "TegraGM20BViews.h"

struct SwitchGpuCpuSurface {
	void *descriptor = nullptr;
	uint64_t offset = 0, bytes = 0;
	uint32_t access = 0;
	bool held = false;

	int acquire(switch_gpu_backing *backing, void *surface, uint32_t id, void *task,
		    uint64_t requestedOffset, uint64_t requestedBytes, uint32_t requestedAccess)
	{
		offset = requestedOffset;
		bytes = requestedBytes;
		access = requestedAccess;
		switch_gpu_backing *logical = nullptr;
		switch_gpu_cpu_lease *owned = nullptr;
		int result = surface ? switch_gpu_surface_prepare_cpu(surface, id, task, access,
								      &logical, &owned, &descriptor)
				     : 0;
		if (!result && owned && (access & SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY))
			result = -95;
		if (!result && logical && logical != backing)
			result = -95;
		if (!result && owned) {
			switch_gpu_backing_info info = {};
			switch_gpu_backing_describe(backing, &info);
			offset = 0;
			bytes = uint64_t(info.page_count) << 12;
			access &= SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE |
				  SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
			result = switch_gpu_backing_cpu_transfer_owned(backing, &owned, access);
		} else if (!result) {
			result = switch_gpu_backing_cpu_acquire(backing, offset, bytes, access);
		}
		held = !result;
		if (owned)
			switch_gpu_backing_cpu_release_owned(&owned, 0);
		if (logical)
			switch_gpu_backing_release(logical);
		if (result && descriptor) {
			switch_gpu_surface_finish(descriptor);
			descriptor = nullptr;
		}
		return result;
	}

	int release(switch_gpu_backing *backing)
	{
		const int result =
			held ? switch_gpu_backing_cpu_release(backing, offset, bytes, access) : 0;
		held = false;
		if (descriptor) {
			switch_gpu_surface_finish(descriptor);
			descriptor = nullptr;
		}
		return result;
	}
};
