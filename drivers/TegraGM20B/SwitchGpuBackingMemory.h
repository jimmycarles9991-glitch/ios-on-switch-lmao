// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SWITCH_GPU_BACKING_MEMORY_FIXED = 1u << 0,
	SWITCH_GPU_BACKING_MEMORY_SMMU = 1u << 1
};

typedef int (*switch_gpu_backing_create_fn)(const uint64_t *, uint32_t, void **, uint32_t *);

/* The returned object is borrowed through the caller's backing reference. Creation runs outside ownership locks;
 * racing unpublished objects are discarded. FIXED forbids growing an allocation whose compression tags name it. */
int switch_gpu_backing_memory_acquire(struct switch_gpu_backing *backing, uint32_t page_count,
				      switch_gpu_backing_create_fn create, void (*destroy)(void *),
				      void **object);

int switch_gpu_backing_memory_acquire_smmu(struct switch_gpu_backing *backing, uint32_t page_count,
					   switch_gpu_backing_create_fn create_smmu,
					   switch_gpu_backing_create_fn create_direct,
					   void (*destroy)(void *), void **object);

int switch_gpu_backing_memory_existing(struct switch_gpu_backing *backing, uint32_t page_count,
				       void (*destroy)(void *), void **object);

int switch_gpu_backing_memory_existing_or_direct(struct switch_gpu_backing *backing,
						 uint32_t page_count, void (*destroy)(void *),
						 void **object);

void switch_gpu_backing_mark_direct_pa(struct switch_gpu_backing *backing);

#ifdef __cplusplus
}
#endif
