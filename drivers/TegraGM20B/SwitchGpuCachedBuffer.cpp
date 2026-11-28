// SPDX-License-Identifier: MIT
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <switch/fresh_memory.h>
#include "SwitchGpuCachedBuffer.h"
#include "SwitchGpuSpares.h"

bool switch_gpu_cached_buffer_units(uint64_t bytes, SwitchGpuCachedBuffer *out)
{
	const switch_gpu_spare_units layout = switch_gpu_spare_layout(bytes);
	if (!layout.units)
		return false;
	const uint32_t count = layout.units + (layout.tail ? 1u : 0u);
	auto **parts = IONew(IOMemoryDescriptor *, count);
	if (!parts)
		return false;
	uint32_t made = 0;
	while (made < count) {
		// One physically contiguous run per unit: 32 pages, which a used system still has, where a whole spare of
		// 16 MiB in one run it often has not.
		void *cpu = nullptr;
		parts[made] = made < layout.units
				      ? switch_iokit_fresh_memory(SWITCH_GPU_SPARE_UNIT_BYTES,
								  SWITCH_GPU_SPARE_UNIT_BYTES,
								  SWITCH_IOKIT_FRESH_CACHED, &cpu)
				      : switch_iokit_fresh_memory(layout.tail, 0,
								  SWITCH_IOKIT_FRESH_CACHED, &cpu);
		if (!parts[made])
			break;
		++made;
	}
	IOMultiMemoryDescriptor *multi =
		made == count ? IOMultiMemoryDescriptor::withDescriptors(parts, count,
									 kIODirectionInOut, false)
			      : nullptr;
	for (uint32_t i = 0; i < made; ++i)
		parts[i]->release();
	IODelete(parts, IOMemoryDescriptor *, count);
	if (!multi)
		return false;
	if (multi->prepare() != kIOReturnSuccess) {
		multi->release();
		return false;
	}
	// Not prefaulted: a kernel-map prefault asserts a VM page for every entry in DEVELOPMENT kernels
	// (vm_map_enter_mem_object: "no VM page"), which this view's memory entries are not proven to give. Its PTEs come
	// from a read of each page here, on the refill thread with no lock held, as IOBufferMemoryDescriptor does for a
	// cache-mode buffer: the scrub's cache maintenance by VA, later under the zeroing queue's lock, then faults nothing.
	IOMemoryMap *map = multi->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
	if (!map) {
		multi->complete();
		multi->release();
		return false;
	}
	const volatile uint8_t *bytesOf =
		reinterpret_cast<const volatile uint8_t *>(map->getVirtualAddress());
	for (uint64_t off = 0; off < bytes; off += PAGE_SIZE)
		(void)bytesOf[off];
	*out = {multi, map, reinterpret_cast<void *>(map->getVirtualAddress())};
	return true;
}

namespace
{
bool freshBuffer(uint64_t bytes, switch_iokit_fresh_cache cache, SwitchGpuCachedBuffer *out)
{
	void *cpu = nullptr;
	IOMemoryDescriptor *memory = switch_iokit_fresh_memory(bytes, 0, cache, &cpu);
	if (!memory)
		return false;
	if (memory->prepare() != kIOReturnSuccess) {
		memory->release();
		return false;
	}
	*out = {memory, nullptr, cpu};
	return true;
}
}  // namespace

bool switch_gpu_cached_buffer_pages(uint64_t bytes, SwitchGpuCachedBuffer *out)
{
	return freshBuffer(bytes, SWITCH_IOKIT_FRESH_CACHED, out);
}

bool switch_gpu_cached_buffer_write_combined(uint64_t bytes, SwitchGpuCachedBuffer *out)
{
	return freshBuffer(bytes, SWITCH_IOKIT_FRESH_WRITE_COMBINED, out);
}

SwitchGpuCachedBuffer switch_gpu_cached_buffer_of(IOMemoryDescriptor *memory)
{
	auto *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	return {memory, nullptr, buffer ? buffer->getBytesNoCopy() : nullptr};
}

SwitchGpuPageList switch_gpu_cached_buffer_page_list(const SwitchGpuCachedBuffer &buffer,
						     uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount run = 0;
		const addr64_t pa =
			buffer.memory->getPhysicalSegment(off, &run, kIOMemoryMapperNone);
		if (!pa || run < 4096)
			return SwitchGpuPageList::NoSegment;
		if (pa & 0xfff)
			return SwitchGpuPageList::Misaligned;
		for (uint64_t at = 0; at + 4096 <= run && off < bytes; at += 4096, off += 4096) {
			if (pa + at + 4096 > (1ULL << 34))
				return SwitchGpuPageList::PastGpu;
			pas[off >> 12] = pa + at;
		}
	}
	return SwitchGpuPageList::Listed;
}

bool switch_gpu_cached_buffer_alone(const SwitchGpuCachedBuffer &buffer)
{
	// an IOMemoryMap holds its descriptor
	return buffer.memory->getRetainCount() == (buffer.map ? 2 : 1);
}

void switch_gpu_cached_buffer_free(SwitchGpuCachedBuffer *buffer)
{
	OSSafeReleaseNULL(buffer->map);
	if (buffer->memory) {
		buffer->memory->complete();
		buffer->memory->release();
	}
	*buffer = {};
}
