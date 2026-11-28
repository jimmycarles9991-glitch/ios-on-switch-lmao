// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

// A cached BO's buffer: a prepared descriptor and the kernel's cacheable address of its first byte. One the spare refill
// made of 128 KiB units (SwitchGpuSpares.h switch_gpu_spare_layout) is their IOMultiMemoryDescriptor, which `map` puts
// at one kernel address; a buffer of one descriptor has no `map`. The spare refill's (units, pages) are not zeroed: the
// copy engine zeroes them before any is handed out.
struct SwitchGpuCachedBuffer {
	IOMemoryDescriptor *memory;
	IOMemoryMap *map;
	void *cpu;
};

// Physically contiguous 128 KiB-aligned units, then pages: false when the layout has no units or the VM gave none
bool switch_gpu_cached_buffer_units(uint64_t bytes, SwitchGpuCachedBuffer *out);
// One buffer of pages, under the GM20B's 34-bit physical limit
bool switch_gpu_cached_buffer_pages(uint64_t bytes, SwitchGpuCachedBuffer *out);
// One buffer of pages whose VM object is write-combined, as a system BO's, its pages as their last owner left them
bool switch_gpu_cached_buffer_write_combined(uint64_t bytes, SwitchGpuCachedBuffer *out);
SwitchGpuCachedBuffer switch_gpu_cached_buffer_of(IOMemoryDescriptor *memory);
enum class SwitchGpuPageList {
	Listed,
	NoSegment,
	Misaligned,
	PastGpu
};

// The 4 KiB pages of its first `bytes`, from the prepared descriptor's own page list, or why it has none
SwitchGpuPageList switch_gpu_cached_buffer_page_list(const SwitchGpuCachedBuffer &buffer,
						     uint64_t bytes, uint64_t *pas);
// Nothing but the buffer itself holds its memory: no view, task mapping or pin
bool switch_gpu_cached_buffer_alone(const SwitchGpuCachedBuffer &buffer);
void switch_gpu_cached_buffer_free(SwitchGpuCachedBuffer *buffer);
