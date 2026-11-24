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
								  SWITCH_GPU_SPARE_UNIT_BYTES, &cpu)
				      : switch_iokit_fresh_memory(layout.tail, 0, &cpu);
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
	IOMemoryMap *map = multi->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
	if (!map) {
		multi->complete();
		multi->release();
		return false;
	}
	*out = {multi, map, reinterpret_cast<void *>(map->getVirtualAddress())};
	return true;
}

bool switch_gpu_cached_buffer_pages(uint64_t bytes, SwitchGpuCachedBuffer *out)
{
	void *cpu = nullptr;
	IOMemoryDescriptor *memory = switch_iokit_fresh_memory(bytes, 0, &cpu);
	if (!memory)
		return false;
	if (memory->prepare() != kIOReturnSuccess) {
		memory->release();
		return false;
	}
	*out = {memory, nullptr, cpu};
	return true;
}

SwitchGpuCachedBuffer switch_gpu_cached_buffer_of(IOMemoryDescriptor *memory)
{
	auto *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	return {memory, nullptr, buffer ? buffer->getBytesNoCopy() : nullptr};
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
