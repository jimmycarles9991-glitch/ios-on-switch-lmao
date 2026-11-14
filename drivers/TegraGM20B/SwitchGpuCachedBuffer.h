// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

// A cached BO's buffer: a prepared descriptor and the kernel's cacheable address of its first byte. One the spare refill
// made of 128 KiB units (SwitchGpuSpares.h switch_gpu_spare_layout) is their IOMultiMemoryDescriptor, which `map` puts
// at one kernel address; a buffer of one IOBufferMemoryDescriptor has no `map`.
struct SwitchGpuCachedBuffer {
	IOMemoryDescriptor *memory;
	IOMemoryMap *map;
	void *cpu;
};

// Physically contiguous 128 KiB-aligned units, then pages: false when the layout has no units or the VM gave none
bool switch_gpu_cached_buffer_units(uint64_t bytes, SwitchGpuCachedBuffer *out);
// One buffer of pages, under the GM20B's 34-bit physical limit
bool switch_gpu_cached_buffer_pages(uint64_t bytes, SwitchGpuCachedBuffer *out);
SwitchGpuCachedBuffer switch_gpu_cached_buffer_of(IOMemoryDescriptor *memory);
// Nothing but the buffer itself holds its memory: no view, task mapping or pin
bool switch_gpu_cached_buffer_alone(const SwitchGpuCachedBuffer &buffer);
void switch_gpu_cached_buffer_free(SwitchGpuCachedBuffer *buffer);
