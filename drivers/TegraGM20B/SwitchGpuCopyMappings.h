// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuRelayoutCache.h"
class IOMemoryDescriptor;

struct SwitchGpuCopyMapping {
	IOMemoryDescriptor *descriptor;
	SwitchGpuRelayoutMapping *mapping;
	uint64_t descriptorStart, backingStart, length, lastUse;
	uint32_t users;
};

struct SwitchGpuCopyMappings {
	SwitchGpuRelayoutCache allocations;
	SwitchGpuCopyMapping descriptors[SwitchGpuRelayoutCache::Capacity];
	uint64_t clock;
};

int switch_gpu_copy_mapping(tgm_vm *, SwitchGpuCopyMappings *, IOMemoryDescriptor *,
			    uint64_t descriptorOffset, switch_gpu_backing *, uint64_t backingOffset,
			    uint64_t bytes, bool tiled, SwitchGpuCopyMapping **);
void switch_gpu_copy_mapping_put(SwitchGpuCopyMapping *);
void switch_gpu_copy_mappings_clear(tgm_vm *, SwitchGpuCopyMappings *);
