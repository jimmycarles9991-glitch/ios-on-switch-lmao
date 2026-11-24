// SPDX-License-Identifier: MIT
#include "SwitchGpuAllocatedMemory.h"

namespace
{
int allocatedMemoryCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_exclusive(nullptr, pages, count, &memory);
	*out = memory;
	*flags = tgm_mem_can_compress(memory) ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0;
	return result;
}

int allocatedMemorySmmuCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_exclusive_smmu(nullptr, pages, count, &memory);
	*out = memory;
	*flags = (tgm_mem_can_compress(memory) ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0) |
		 (tgm_mem_is_smmu(memory) ? SWITCH_GPU_BACKING_MEMORY_SMMU : 0);
	return result;
}

int allocatedMemoryDirectFallbackCreate(const uint64_t *pages, uint32_t count, void **out,
					uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new(nullptr, pages, count, &memory);
	*out = memory;
	*flags = 0;
	return result;
}
}  // namespace

extern "C" void switch_gpu_allocated_memory_destroy(void *object)
{
	auto *memory = static_cast<tgm_mem *>(object);
	tgm_mem_del(&memory);
}

extern "C" int switch_gpu_backing_tgm_mem_direct(struct switch_gpu_backing *backing, uint32_t count,
						 struct tgm_mem **out)
{
	void *object = nullptr;
	const int result =
		switch_gpu_backing_memory_acquire(backing, count, allocatedMemoryCreate,
						  switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem(struct switch_gpu_backing *backing, uint32_t count,
					  struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_acquire_smmu(
		backing, count, allocatedMemorySmmuCreate, allocatedMemoryDirectFallbackCreate,
		switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem_existing(struct switch_gpu_backing *backing,
						   uint32_t count, struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_existing(
		backing, count, switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}

extern "C" int switch_gpu_backing_tgm_mem_existing_or_direct(struct switch_gpu_backing *backing,
							     uint32_t count, struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_existing_or_direct(
		backing, count, switch_gpu_allocated_memory_destroy, &object);
	*out = static_cast<tgm_mem *>(object);
	return result;
}
