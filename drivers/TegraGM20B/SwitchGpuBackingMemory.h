// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SWITCH_GPU_BACKING_MEMORY_FIXED = 1u << 0
};

/* The returned object is borrowed through the caller's backing reference. Creation runs outside ownership locks;
 * racing unpublished objects are discarded. FIXED forbids growing an allocation whose compression tags name it. */
int switch_gpu_backing_memory_acquire(struct switch_gpu_backing *backing, uint32_t page_count,
				      int (*create)(const uint64_t *, uint32_t, void **,
						    uint32_t *),
				      void (*destroy)(void *), void **object);

#ifdef __cplusplus
}
#endif
