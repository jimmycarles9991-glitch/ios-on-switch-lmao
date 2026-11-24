// SPDX-License-Identifier: MIT
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuBackingMemory.h"

namespace
{
int backing_memory_acquire_impl(switch_gpu_backing *backing, uint32_t count,
				switch_gpu_backing_create_fn create_smmu,
				switch_gpu_backing_create_fn create_direct, void (*destroy)(void *),
				void **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!backing || !count || (!create_smmu && !create_direct) || !destroy)
		return -22;
	constexpr uint32_t kAllowedFlags =
		SWITCH_GPU_BACKING_MEMORY_FIXED | SWITCH_GPU_BACKING_MEMORY_SMMU;
	for (;;) {
		bool claimed = switch_gpu_backing_enter(backing);
		if (count > backing->pages->count ||
		    (backing->gpu_memory && (count > backing->gpu_memory_count ||
					     backing->release_gpu_memory != destroy))) {
			switch_gpu_backing_leave(backing, claimed);
			return -95;
		}
		if (backing->gpu_memory) {
			*out = backing->gpu_memory;
			switch_gpu_backing_leave(backing, claimed);
			return 0;
		}
		const bool try_smmu = create_smmu != nullptr && !backing->gpu_direct_pa &&
				      count == backing->pages->count;
		auto *const creator = try_smmu ? create_smmu : create_direct;
		if (!creator) {
			switch_gpu_backing_leave(backing, claimed);
			return -95;
		}
		const uint64_t *pages = backing->pages->addresses;
		const uint64_t generation = backing->generation;
		switch_gpu_backing_leave(backing, claimed);

		void *made = nullptr;
		uint32_t flags = 0;
		int result = creator(pages, count, &made, &flags);
		if (!result && (!made || (flags & ~kAllowedFlags) ||
				((flags & SWITCH_GPU_BACKING_MEMORY_SMMU) && !try_smmu)))
			result = -22;
		bool retry_direct = false;
		if (!result) {
			const bool is_smmu = (flags & SWITCH_GPU_BACKING_MEMORY_SMMU) != 0u;
			const uint32_t stored_flags =
				(flags & SWITCH_GPU_BACKING_MEMORY_FIXED) |
				(is_smmu ? SWITCH_GPU_BACKING_MEMORY_FIXED : 0u);
			claimed = switch_gpu_backing_enter(backing);
			if (backing->generation != generation) {
				result = SWITCH_GPU_BACKING_STALE;
			} else if (backing->gpu_memory) {
				if (count > backing->gpu_memory_count ||
				    backing->release_gpu_memory != destroy)
					result = -95;
				else
					*out = backing->gpu_memory;
			} else if (is_smmu && backing->gpu_direct_pa) {
				// Direct PA became active while the SMMU candidate was built;
				// discard it unpublished so L2 never sees both tags for these pages.
				if (create_direct)
					retry_direct = true;
				else
					result = -95;
			} else if ((stored_flags & SWITCH_GPU_BACKING_MEMORY_FIXED) &&
				   count != backing->pages->count) {
				result = -95;
			} else {
				backing->gpu_memory = made;
				backing->gpu_memory_count = count;
				backing->gpu_memory_flags = stored_flags;
				backing->release_gpu_memory = destroy;
				if (!is_smmu)
					backing->gpu_direct_pa = true;
				*out = made;
				made = nullptr;
			}
			switch_gpu_backing_leave(backing, claimed);
		}
		if (made)
			destroy(made);
		if (!retry_direct)
			return result;
	}
}

int backing_memory_existing_impl(switch_gpu_backing *backing, uint32_t count,
				 void (*destroy)(void *), bool mark_direct_pa, void **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!backing || !count || !destroy)
		return -22;
	const bool claimed = switch_gpu_backing_enter(backing);
	int result = 0;
	if (!backing->gpu_memory) {
		if (mark_direct_pa)
			backing->gpu_direct_pa = true;
		result = -2;
	} else if (count > backing->gpu_memory_count || backing->release_gpu_memory != destroy) {
		result = -95;
	} else {
		*out = backing->gpu_memory;
	}
	switch_gpu_backing_leave(backing, claimed);
	return result;
}
}  // namespace

extern "C" int switch_gpu_backing_memory_acquire(switch_gpu_backing *backing, uint32_t count,
						 switch_gpu_backing_create_fn create,
						 void (*destroy)(void *), void **out)
{
	return backing_memory_acquire_impl(backing, count, nullptr, create, destroy, out);
}

extern "C" int switch_gpu_backing_memory_acquire_smmu(switch_gpu_backing *backing, uint32_t count,
						      switch_gpu_backing_create_fn create_smmu,
						      switch_gpu_backing_create_fn create_direct,
						      void (*destroy)(void *), void **out)
{
	if (!create_smmu)
		return -22;
	return backing_memory_acquire_impl(backing, count, create_smmu, create_direct, destroy,
					   out);
}

extern "C" int switch_gpu_backing_memory_existing(switch_gpu_backing *backing, uint32_t count,
						  void (*destroy)(void *), void **out)
{
	return backing_memory_existing_impl(backing, count, destroy, false, out);
}

extern "C" int switch_gpu_backing_memory_existing_or_direct(switch_gpu_backing *backing,
							    uint32_t count, void (*destroy)(void *),
							    void **out)
{
	return backing_memory_existing_impl(backing, count, destroy, true, out);
}

extern "C" void switch_gpu_backing_mark_direct_pa(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	const bool claimed = switch_gpu_backing_enter(backing);
	backing->gpu_direct_pa = true;
	switch_gpu_backing_leave(backing, claimed);
}
