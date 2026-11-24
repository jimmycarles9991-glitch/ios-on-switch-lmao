// SPDX-License-Identifier: MIT
#include "switch_gpu_allocated_memory_peer.h"
#include "drivers/TegraGM20B/SwitchGpuAllocatedMemory.h"

int peer_allocated_memory_smmu(switch_gpu_backing *backing, uint32_t count, tgm_mem **out)
{
	return allocatedMemorySmmu(backing, count, out);
}

int peer_allocated_memory_existing(switch_gpu_backing *backing, uint32_t count, tgm_mem **out)
{
	return allocatedMemoryExisting(backing, count, out);
}

int peer_allocated_memory_existing_or_direct(switch_gpu_backing *backing, uint32_t count,
					     tgm_mem **out)
{
	return allocatedMemoryExistingOrDirect(backing, count, out);
}

void (*peer_destroy_address(void))(void *)
{
	return switch_gpu_allocated_memory_destroy;
}
