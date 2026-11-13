// SPDX-License-Identifier: MIT
// A kernel copy queue's address space and channel, written once for the kernel's own producers (the VIC plane copy,
// relayout, cached-zero scrub, host1x bridge): pool pages for its pushes mapped GPU-volatile at push_va (the CPU rewrites
// a slot for every job, and a job no host wrote around runs with no L2 invalidate ahead of it), its kernel fence slot
// mapped in its own space, and a copy-only channel on them. What a producer keeps of its own: its jobs, lock, worker,
// and when to replace a killed channel.
#pragma once
#include <stdint.h>
#include "TegraGM20BGpu.h"

struct SwitchGpuKqueueShape {
	const char *name;
	uint32_t fenceSlot;  // TGM_FENCE_KERNEL_*
	uint32_t pushPages;
	uint64_t pushVa;
	int pushReadOnly;  // 0: the GPU writes into the push pages too (relayout's checkpoint)
	int high;
};

struct SwitchGpuKqueue {
	static constexpr uint32_t kMaxPages = 8;
	static constexpr uint64_t kPage = 4096;

	tgm_vm *vm;
	tgm_chan *channel;
	tgm_mem *push, *fence;
	switch_gpu_mapping *pushMapping;
	uint32_t *words;
	volatile uint32_t *fenceCpu;
	uint64_t pushPa, fenceGpu, fencePage;
	uint64_t pages[kMaxPages];

	// A failure leaves what it made for discardSpace. `owner` claims the space (the producer's state).
	bool openSpace(const SwitchGpuKqueueShape &shape, const void *owner)
	{
		uint32_t fenceOffset = 0;
		if (!shape.pushPages || shape.pushPages > kMaxPages)
			return false;
		words = static_cast<uint32_t *>(
			tgm_pool_alloc(kPage * shape.pushPages, 0, &pushPa));
		if (!words ||
		    tgm_fence_kernel(shape.fenceSlot, &fenceCpu, &fencePage, &fenceOffset) ||
		    tgm_vm_new(&vm))
			return false;
		tgm_vm_claim(vm, owner);
		for (uint32_t i = 0; i < shape.pushPages; ++i)
			pages[i] = pushPa + i * kPage;
		if (tgm_mem_new(vm, pages, shape.pushPages, &push) ||
		    switch_gpu_vm_map_volatile(vm, shape.pushVa, kPage * shape.pushPages, push, 0,
					       0, shape.pushReadOnly, 1, &pushMapping) ||
		    tgm_mem_new(vm, &fencePage, 1, &fence) ||
		    tgm_vm_kmap(vm, fence, kPage, &fenceGpu))
			return false;
		fenceGpu += fenceOffset;
		return true;
	}

	void discardSpace(const SwitchGpuKqueueShape &shape)
	{
		switch_gpu_vm_unmap(vm, &pushMapping);
		tgm_vm_del(&vm);
		tgm_mem_del(&push);
		tgm_mem_del(&fence);
		if (words)
			tgm_pool_free(words, kPage * shape.pushPages, pushPa);
		words = nullptr;
	}

	// The channel on an open space, made when there is none: 0, or the error making it.
	int openChannel(const SwitchGpuKqueueShape &shape, const void *owner,
			void (*fenced)(void *))
	{
		if (channel)
			return 0;
		const int result = tgm_chan_new_copy(vm, owner, fenceCpu, fenceGpu, shape.name,
						     shape.high, &channel);
		if (!result && fenced)
			tgm_chan_on_fence(channel, fenced, nullptr);
		return result;
	}
};
