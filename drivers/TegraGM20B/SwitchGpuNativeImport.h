// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuPathStats.h"

static bool switch_gpu_native_reply(const switch_gpu_native_surface_view &view,
				    const switch_gpu_backing_info &allocation,
				    TegraGM20BAbiNativeSurfaceViewReply &out)
{
	if (!view.logical || !view.native || !view.descriptor || !allocation.pages ||
	    !allocation.page_count || !view.representation.generation ||
	    view.representation.backing != view.logical || !view.representation.expected ||
	    !view.layout.plane_count || view.layout.plane_count > 3 ||
	    (view.payload.generation &&
	     (view.payload.backing != view.logical || !view.payload.expected)))
		return false;
	out.bytes = uint64_t(allocation.page_count) << 12;
	out.representation_generation = view.representation.expected;
	out.allocation_generation = allocation.allocation_generation;
	out.format = view.layout.format;
	out.plane_count = view.layout.plane_count;
	out.kind = view.layout.planes[0].layout == SWITCH_GPU_BACKING_BLOCK_LINEAR ? 0xfe : 0;
	out.crop_x = view.crop_x;
	out.crop_y = view.crop_y;
	out.crop_width = view.crop_width;
	out.crop_height = view.crop_height;
	for (uint32_t i = 0; i < view.layout.plane_count; ++i) {
		const auto &plane = view.layout.planes[i];
		if (!plane.width || !plane.height || plane.width > 16384 || plane.height > 16384 ||
		    !plane.bytes_per_element || plane.bytes_per_element > 16 ||
		    (plane.bytes_per_element & (plane.bytes_per_element - 1)) ||
		    plane.layout > SWITCH_GPU_BACKING_BLOCK_LINEAR || plane.block_height_log2 > 5 ||
		    plane.layout != view.layout.planes[0].layout || plane.offset > out.bytes ||
		    !plane.bytes || plane.bytes > out.bytes - plane.offset ||
		    uint64_t(plane.width) * plane.bytes_per_element > plane.pitch ||
		    uint64_t(plane.pitch) * plane.height > plane.bytes)
			return false;
		out.planes[i] = {plane.offset,
				 plane.bytes,
				 plane.width,
				 plane.height,
				 plane.pitch,
				 plane.bytes_per_element,
				 plane.block_height_log2,
				 0};
	}
	const auto &first = view.layout.planes[0];
	return view.crop_width && view.crop_height && view.crop_x <= first.width &&
	       view.crop_width <= first.width - view.crop_x && view.crop_y <= first.height &&
	       view.crop_height <= first.height - view.crop_y;
}

IOReturn AGXDeviceUserClient::nativeSurfaceView(const TegraGM20BAbiNativeSurfaceView *in,
						TegraGM20BAbiNativeSurfaceViewReply *out)
{
	*out = {};
	const Bo *source = findBo(in->source_handle);
	if (in->flags || in->reserved || !source || !source->surface || source->nativeView)
		return kIOReturnBadArgument;
	if (!boIndex_.reserve())
		return kIOReturnNoMemory;
	if (!switch_native_surface_gpu_acquire || !switch_native_surface_gpu_release)
		return kIOReturnUnsupported;
	auto *view = IONewZero(switch_gpu_native_surface_view, 1);
	if (!view)
		return kIOReturnNoMemory;
	IOLockUnlock(lock_);
	int result = switch_native_surface_gpu_acquire(source->surface, view);
	IOLockLock(lock_);
	if (!result && submitGate_.closed())
		result = -19;
	switch_gpu_backing_info allocation = {};
	if (!result) {
		switch_gpu_backing_describe(view->native, &allocation);
		if (!switch_gpu_native_reply(*view, allocation, *out))
			result = -22;
	}
	Bo *bo = !result ? IONewZero(Bo, 1) : nullptr;
	if (!result && !bo)
		result = -12;
	if (!result)
		result = tgm_mem_new(vm_, allocation.pages, allocation.page_count, &bo->mem);
	if (result) {
		if (bo)
			IODelete(bo, Bo, 1);
		switch_native_surface_gpu_release(view);
		IODelete(view, switch_gpu_native_surface_view, 1);
		*out = {};
		return result == -2   ? kIOReturnSuccess
		       : result == -5 ? kIOReturnAborted
				      : ioreturn(result);
	}
	bo->nativeView = view;
	bo->backing = view->native;
	switch_gpu_backing_retain(bo->backing);
	bo->surface = source->surface;
	bo->surface->retain();
	bo->pas = const_cast<uint64_t *>(allocation.pages);
	bo->npages = bo->backing_pages = allocation.page_count;
	bo->bytes = out->bytes;
	bo->readOnly = true;
	bo->handle = next_handle_++;
	bo->next = bos_;
	bos_ = bo;
	boIndex_.insert(bo->handle, bo);
	out->handle = bo->handle;
	switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_VIEW, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_TILED, out->kind == 0xfe ? 1 : 0);
	return kIOReturnSuccess;
}
