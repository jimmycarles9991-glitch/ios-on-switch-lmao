// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBackingMemory.h"
#include "TegraGM20BGpu.h"

#ifdef __cplusplus
extern "C" {
#endif

void switch_gpu_allocated_memory_destroy(void *object);

#ifdef __cplusplus
}
#endif

static inline int allocatedMemory(struct switch_gpu_backing *backing, uint32_t count,
				  struct tgm_mem **out)
{
	return switch_gpu_backing_tgm_mem_direct(backing, count, out);
}

static inline int allocatedMemorySmmu(struct switch_gpu_backing *backing, uint32_t count,
				      struct tgm_mem **out)
{
	return switch_gpu_backing_tgm_mem(backing, count, out);
}

static inline int allocatedMemoryBig(struct switch_gpu_backing *backing, uint32_t count,
				     struct tgm_mem **out)
{
	return switch_gpu_backing_tgm_mem_big(backing, count, out);
}

static inline int allocatedMemoryExisting(struct switch_gpu_backing *backing, uint32_t count,
					  struct tgm_mem **out)
{
	return switch_gpu_backing_tgm_mem_existing(backing, count, out);
}

static inline int allocatedMemoryExistingOrDirect(struct switch_gpu_backing *backing,
						  uint32_t count, struct tgm_mem **out)
{
	return switch_gpu_backing_tgm_mem_existing_or_direct(backing, count, out);
}
