// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBackingMemory.h"
#include "TegraGM20BGpu.h"

static int allocatedMemoryCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_exclusive(nullptr, pages, count, &memory);
	*out = memory;
	*flags = tgm_mem_can_compress(memory) ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0;
	return result;
}

static void allocatedMemoryDestroy(void *object)
{
	auto *memory = static_cast<tgm_mem *>(object);
	tgm_mem_del(&memory);
}

static int allocatedMemory(switch_gpu_backing *backing, uint32_t count, tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_acquire(backing, count, allocatedMemoryCreate,
							     allocatedMemoryDestroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}
