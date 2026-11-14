// SPDX-License-Identifier: MIT
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include "SwitchGpuCachedBuffer.h"
#include "SwitchGpuSpares.h"

namespace
{
// GM20B on Tegra210 addresses 34-bit physical memory (TegraGM20BUserClientBo.cpp collectPages)
constexpr uint64_t kReach = 0x3ffffffffULL;
constexpr IOOptionBits kOptions = static_cast<IOOptionBits>(kIODirectionInOut) |
				  static_cast<IOOptionBits>(kIOMemoryMapperNone);

IOMemoryDescriptor *part(uint64_t bytes, bool unit)
{
	// A unit's alignment in the mask makes IOKit search for one physically contiguous run of it (kmem_alloc_contig):
	// 32 pages, which a used system still has, where a whole spare of 16 MiB in one run it often has not.
	return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, unit ? kOptions | kIOMemoryPhysicallyContiguous : kOptions, bytes,
		unit ? kReach & ~(SWITCH_GPU_SPARE_UNIT_BYTES - 1) : kReach);
}
}  // namespace

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
		parts[made] = made < layout.units ? part(SWITCH_GPU_SPARE_UNIT_BYTES, true)
						  : part(layout.tail, false);
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
	IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kOptions, bytes, kReach);
	if (!buffer)
		return false;
	if (buffer->prepare() != kIOReturnSuccess) {
		buffer->release();
		return false;
	}
	*out = switch_gpu_cached_buffer_of(buffer);
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
