// SPDX-License-Identifier: MIT
#include "SwitchGpuAllocatedMemory.h"

#include <IOKit/IOLib.h>
#include <kern/thread.h>

namespace
{
// The backings' retirement work while it destroys one batch (switch_gpu_backing_retire): the canonical memories it
// releases share one L2 write-back and invalidate (tgm_mem_del_later). Only that work item's thread defers; others
// release at once. Everything here is that work's alone, the copy queue's completions included (they run on it).
decltype(current_thread()) gRetireThread;
tgm_mem *gRetired;

// A retirement batch whose memories wait for their L2 write-back and invalidate: the memories (a tgm_mem_del_later
// list) and the batch's retired backings, whose pages go back to the VM only once the GPU's flush has completed. A
// batch the copy queue could not take yet is parked, and the next submission takes every parked batch: each one's
// flush is queued after its own GMMU unmaps, so no batch rides a flush submitted before them.
struct RetiredBatch {
	tgm_mem *memories;
	switch_gpu_backing *backings;
	RetiredBatch *next;
};

RetiredBatch *gParked;

void flushed(void *arg, int status)
{
	for (auto *batch = static_cast<RetiredBatch *>(arg); batch;) {
		auto *next = batch->next;
		// a failed copy channel leaves its flush unproven, so the CPU writes back and invalidates (nvgpu's registers)
		if (status < 0) {
			tgm_mem_del_finish(batch->memories);
		} else {
#if TGM_STATS
			__atomic_fetch_add(&tgm_retire_gpu_flushes, 1, __ATOMIC_RELAXED);
#endif
			tgm_mem_del_release(batch->memories);
		}
		switch_gpu_backing_release_retired(batch->backings);
		IODelete(batch, RetiredBatch, 1);
		batch = next;
	}
}

void submitParked()
{
	if (!gParked)
		return;
	const int result = switch_gpu_copy_l2_flush(flushed, gParked);
	// -16 and -11 pass (a later retirement pass asks again); any other refusal is the CPU path now
	if (result == -16 || result == -11)
		return;
	RetiredBatch *batches = gParked;
	gParked = nullptr;
	if (result)
		flushed(batches, result);
}

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

int allocatedMemoryBigCreate(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	tgm_mem *memory = nullptr;
	const int result = tgm_mem_new_smmu_big(nullptr, pages, count, &memory);
	*out = memory;
	*flags = tgm_mem_is_smmu(memory) ? SWITCH_GPU_BACKING_MEMORY_SMMU : 0;
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
	if (__atomic_load_n(&gRetireThread, __ATOMIC_RELAXED) == current_thread())
		tgm_mem_del_later(&memory, &gRetired);
	else
		tgm_mem_del(&memory);
}

extern "C" void switch_gpu_memory_retire_begin(void)
{
	__atomic_store_n(&gRetireThread, current_thread(), __ATOMIC_RELAXED);
}

extern "C" bool switch_gpu_memory_retire_end(switch_gpu_backing *retired)
{
	__atomic_store_n(&gRetireThread, nullptr, __ATOMIC_RELAXED);
	tgm_mem *later = gRetired;
	gRetired = nullptr;
	if (!later)
		return false;
	auto *batch = IONew(RetiredBatch, 1);
	if (!batch) {
		tgm_mem_del_finish(later);
		return false;
	}
	*batch = {later, retired, gParked};
	gParked = batch;
	submitParked();
	return true;
}

extern "C" void switch_gpu_memory_retire_parked(void)
{
	submitParked();
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

extern "C" int switch_gpu_backing_tgm_mem_big(struct switch_gpu_backing *backing, uint32_t count,
					      struct tgm_mem **out)
{
	void *object = nullptr;
	const int result = switch_gpu_backing_memory_acquire_smmu(
		backing, count, allocatedMemoryBigCreate, allocatedMemoryDirectFallbackCreate,
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
