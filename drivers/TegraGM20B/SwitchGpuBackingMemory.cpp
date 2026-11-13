// SPDX-License-Identifier: MIT
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuBackingMemory.h"

extern "C" int switch_gpu_backing_memory_acquire(switch_gpu_backing *backing, uint32_t count,
						 int (*create)(const uint64_t *, uint32_t, void **,
							       uint32_t *),
						 void (*destroy)(void *), void **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!backing || !count || !create || !destroy)
		return -22;
	IOLockLock(backing->lock);
	if (count > backing->pages->count ||
	    (backing->gpu_memory &&
	     (count > backing->gpu_memory_count || backing->release_gpu_memory != destroy))) {
		IOLockUnlock(backing->lock);
		return -95;
	}
	if (backing->gpu_memory) {
		*out = backing->gpu_memory;
		IOLockUnlock(backing->lock);
		return 0;
	}
	const uint64_t *pages = backing->pages->addresses;
	const uint64_t generation = backing->generation;
	IOLockUnlock(backing->lock);

	void *made = nullptr;
	uint32_t flags = 0;
	int result = create(pages, count, &made, &flags);
	if (!result && (!made || (flags & ~SWITCH_GPU_BACKING_MEMORY_FIXED)))
		result = -22;
	if (!result) {
		IOLockLock(backing->lock);
		if (backing->generation != generation) {
			result = SWITCH_GPU_BACKING_STALE;
		} else if (backing->gpu_memory) {
			if (count > backing->gpu_memory_count ||
			    backing->release_gpu_memory != destroy)
				result = -95;
			else
				*out = backing->gpu_memory;
		} else if ((flags & SWITCH_GPU_BACKING_MEMORY_FIXED) &&
			   count != backing->pages->count) {
			result = -95;
		} else {
			backing->gpu_memory = made;
			backing->gpu_memory_count = count;
			backing->gpu_memory_flags = flags;
			backing->release_gpu_memory = destroy;
			*out = made;
			made = nullptr;
		}
		IOLockUnlock(backing->lock);
	}
	if (made)
		destroy(made);
	return result;
}
