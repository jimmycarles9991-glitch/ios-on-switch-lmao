// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

enum class SwitchGpuSurfaceMatch {
	Match,
	Extent,
	Padding,
	Physical,
	Pages,
};

template <class ReadPhysical>
SwitchGpuSurfaceMatch switch_gpu_surface_page_vector(uint64_t memoryBytes, uint64_t base,
						     uint64_t bytes, uint64_t nativePage,
						     const uint64_t *pages, uint32_t count,
						     ReadPhysical read)
{
	constexpr uint64_t gpuPage = 4096;
	if (!pages || !count || !bytes || (base & (gpuPage - 1)) || nativePage < gpuPage ||
	    (nativePage & (nativePage - 1)) || base > memoryBytes || bytes > memoryBytes - base)
		return SwitchGpuSurfaceMatch::Extent;
	const uint64_t imported = uint64_t(count) * gpuPage;
	uint64_t last = 0;
	if (imported > bytes) {
		uint64_t length = 0;
		last = read(base + bytes - 1, length);
		if (!last || !length)
			return SwitchGpuSurfaceMatch::Physical;
		// IOMemoryDescriptor ends at its logical length. The final native VM page still owns its rounded mapping tail.
		if (imported - bytes > nativePage - 1 - (last & (nativePage - 1)))
			return SwitchGpuSurfaceMatch::Padding;
	}
	uint64_t at = 0;
	while (at < imported && at < bytes) {
		uint64_t length = 0;
		const uint64_t physical = read(base + at, length);
		const uint64_t remaining = bytes - at;
		if (!physical || (physical & (gpuPage - 1)) || !length ||
		    (length < remaining && (length & (gpuPage - 1))))
			return SwitchGpuSurfaceMatch::Physical;
		const uint64_t span = length < remaining ? length : remaining;
		const uint64_t end = at + span;
		const uint64_t start = at;
		while (at < imported && at < end) {
			if (physical > UINT64_MAX - (at - start) ||
			    pages[at / gpuPage] != physical + at - start)
				return SwitchGpuSurfaceMatch::Pages;
			at += gpuPage;
		}
	}
	while (at < imported) {
		const uint64_t delta = at - bytes + 1;
		if (last > UINT64_MAX - delta || pages[at / gpuPage] != last + delta)
			return SwitchGpuSurfaceMatch::Pages;
		at += gpuPage;
	}
	return SwitchGpuSurfaceMatch::Match;
}
