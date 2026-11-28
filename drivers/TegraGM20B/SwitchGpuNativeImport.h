// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuPathStats.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

// Why a view cannot be described: a refusal key of its own per reason, so each is logged (switch_refusal: once per
// process and key)
struct SwitchGpuNativeRefusal {
	const char *key, *why;
	uint32_t plane;  // the plane the refusal is about
};

// No key when `out` describes the view. A plane's extent is its memory's, not a texture's: each texture over it is
// shaped and fitted to it in the client (gm20b nativePlaneFits), so a pitch plane wider than any texture (a texel
// buffer's rows of 16384 over a '420v' 20000 wide) is still a view.
static SwitchGpuNativeRefusal switch_gpu_native_reply(const switch_gpu_native_surface_view &view,
						      const switch_gpu_backing_info &allocation,
						      TegraGM20BAbiNativeSurfaceViewReply &out)
{
	if (!view.logical || !view.native || !view.descriptor || !allocation.pages ||
	    !allocation.page_count || !view.representation.generation ||
	    view.representation.backing != view.logical || !view.representation.expected ||
	    (view.payload.generation &&
	     (view.payload.backing != view.logical || !view.payload.expected)))
		return {"backings", "its backings or generations are missing", 0};
	if (!view.layout.plane_count || view.layout.plane_count > 3)
		return {"planes", "it has no planes, or more than 3", 0};
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
		if (!plane.width || !plane.height || !plane.bytes_per_element ||
		    plane.bytes_per_element > 16 ||
		    (plane.bytes_per_element & (plane.bytes_per_element - 1)))
			return {"plane-shape",
				"a plane is empty, or its element size is not 1, 2, 4, 8 or 16", i};
		if (plane.layout > SWITCH_GPU_BACKING_BLOCK_LINEAR || plane.block_height_log2 > 5 ||
		    plane.layout != view.layout.planes[0].layout)
			return {"plane-layout",
				"a plane's layout is unknown or differs from the first plane's", i};
		if (plane.offset > out.bytes || !plane.bytes ||
		    plane.bytes > out.bytes - plane.offset ||
		    uint64_t(plane.width) * plane.bytes_per_element > plane.pitch ||
		    uint64_t(plane.pitch) * plane.height > plane.bytes)
			return {"plane-extent",
				"a plane's rows do not fit its bytes, or its bytes the allocation",
				i};
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
	if (!view.crop_width || !view.crop_height || view.crop_x > first.width ||
	    view.crop_width > first.width - view.crop_x || view.crop_y > first.height ||
	    view.crop_height > first.height - view.crop_y)
		return {"crop", "its crop is empty or outside the first plane", 0};
	return {};
}

IOReturn AGXDeviceUserClient::nativeSurfaceView(const TegraGM20BAbiNativeSurfaceView *in,
						TegraGM20BAbiNativeSurfaceViewReply *out)
{
	*out = {};
	const bool writable = (in->flags & TegraGM20BNativeSurfaceWrite) != 0;
	const Bo *source = findBo(in->source_handle);
	if ((in->flags & ~uint32_t(TegraGM20BNativeSurfaceWrite)) || in->reserved) {
		switch_refusal("native-view", "request",
			       "a view asked with flags %#x, reserved %#x: no view", in->flags,
			       in->reserved);
		return kIOReturnBadArgument;
	}
	if (!source || !source->surface || source->nativeView) {
		switch_refusal("native-view", "source", "a view of BO %llu: %s; no view",
			       (unsigned long long)in->source_handle,
			       !source            ? "no such BO on the connection"
			       : !source->surface ? "the BO is not an IOSurface's"
						  : "the BO is a native view itself");
		return kIOReturnBadArgument;
	}
	if (!boIndex_.reserve())
		return kIOReturnNoMemory;
	if ((writable ? !switch_native_surface_gpu_acquire_write
		      : !switch_native_surface_gpu_acquire) ||
	    !switch_native_surface_gpu_release)
		return kIOReturnUnsupported;
	auto *view = IONewZero(switch_gpu_native_surface_view, 1);
	if (!view)
		return kIOReturnNoMemory;
	IOLockUnlock(lock_);
	int result = writable ? switch_native_surface_gpu_acquire_write(source->surface, view)
			      : switch_native_surface_gpu_acquire(source->surface, view);
	IOLockLock(lock_);
	if (!result && submitGate_.closed()) {
		switch_refusal("native-view", "gate-closed",
			       "a view of BO %llu while the GPU's submissions are shut (reset or "
			       "sleep): no view",
			       (unsigned long long)in->source_handle);
		result = -19;
	}
	switch_gpu_backing_info allocation = {};
	if (!result) {
		switch_gpu_backing_describe(view->native, &allocation);
		const SwitchGpuNativeRefusal refused =
			switch_gpu_native_reply(*view, allocation, *out);
		if (refused.key) {
			const auto &plane = view->layout.planes[refused.plane];
			switch_refusal("native-view", refused.key,
				       "a view of format %#x, %u planes (plane %u %ux%u, pitch %u, "
				       "%llu bytes at %llu), in %u pages: %s",
				       view->layout.format, view->layout.plane_count, refused.plane,
				       plane.width, plane.height, plane.pitch,
				       (unsigned long long)plane.bytes,
				       (unsigned long long)plane.offset, allocation.page_count,
				       refused.why);
			result = -22;
		}
	}
	Bo *bo = !result ? IONewZero(Bo, 1) : nullptr;
	if (!result && !bo)
		result = -12;
	if (!result) {
		if (switch_gpu_backing_tgm_mem_existing_or_direct(
			    view->native, allocation.page_count, &bo->mem) == 0) {
			bo->backingMemory = true;
		} else {
			result =
				tgm_mem_new(vm_, allocation.pages, allocation.page_count, &bo->mem);
			if (!result)
				bo->ownsMem = true;
		}
	}
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
	bo->readOnly = !writable;
	bo->handle = next_handle_++;
	bo->next = bos_;
	bos_ = bo;
	boIndex_.insert(bo->handle, bo);
	bumpBoGen();
	out->handle = bo->handle;
	switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_VIEW, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_TILED, out->kind == 0xfe ? 1 : 0);
	return kIOReturnSuccess;
}
