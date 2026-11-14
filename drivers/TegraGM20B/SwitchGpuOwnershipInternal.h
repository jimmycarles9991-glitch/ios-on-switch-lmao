// SPDX-License-Identifier: MIT
// What begin (SwitchGpuOwnership.cpp) and the records it leads to (SwitchGpuOwnershipRecord.cpp) share.
#pragma once
#include <string.h>
#include "SwitchGpuBackingInternal.h"

namespace switch_gpu_ownership
{
constexpr uint32_t kAccessMask = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kBesideCpu = SWITCH_GPU_BACKING_UNLISTED | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
constexpr uint32_t kUseMask = kAccessMask | kBesideCpu;

inline uint32_t access_kind(uint32_t access)
{
	return (access & SWITCH_GPU_BACKING_WRITE ? SWITCH_GPU_BACKING_WRITE
						  : SWITCH_GPU_BACKING_READ) |
	       (access & SWITCH_GPU_BACKING_UNLISTED);
}

inline uint32_t access_slot(const switch_gpu_backing *backing,
			    const switch_gpu_backing_token &token, uint32_t access)
{
	for (uint32_t i = 0; i < backing->access_count; ++i)
		if (backing->accesses[i].access == access_kind(access) &&
		    switch_gpu_backing_same_timeline(backing->accesses[i].token, token))
			return i;
	return backing->access_count;
}

inline int reserve_access(switch_gpu_backing *backing)
{
	if (backing->access_count < backing->access_capacity)
		return 0;
	const uint32_t capacity = backing->access_capacity ? backing->access_capacity * 2 : 4;
	if (capacity < backing->access_capacity)
		return -12;
	auto *accesses = IONew(switch_gpu_backing_access, capacity);
	if (!accesses)
		return -12;
	if (backing->access_count)
		memcpy(accesses, backing->accesses, sizeof(*accesses) * backing->access_count);
	IODelete(backing->accesses, switch_gpu_backing_access, backing->access_capacity);
	backing->accesses = accesses;
	backing->access_capacity = capacity;
	return 0;
}

// A GM20B job's commit takes the job's completion as the backing's host acquire: the first GM20B use after a host write.
inline bool host_acquire_wanted(const switch_gpu_backing &backing,
				const switch_gpu_backing_token &completion)
{
	return completion.engine == SWITCH_GPU_ENGINE_GM20B &&
	       backing.host_visible_epoch != backing.host_epoch && !backing.host_acquire.engine;
}

// An ordered transaction's marks in its uses' reserved words (zero again when it ends): the use's lock is held, it was
// found to need it, it is to be cleaned, it was cleaned with the order let go.
constexpr uint32_t kUseLocked = 1, kUseGrown = 2, kUseDirty = 4, kUseCleaned = 8;

// How begin holds the transaction's uses (SwitchGpuBackingOrder.cpp): in the order with the locks of the uses that need
// them, or every use claimed.
void hold_ordered(switch_gpu_backing_transaction *transaction);
void hold_claimed(switch_gpu_backing_transaction *transaction);

// What begin holds on the transaction's uses: the ownership order, or each use's lock.
inline bool holds(const switch_gpu_backing_transaction *transaction)
{
	return transaction->ordered || transaction->locked == transaction->count;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
inline uint64_t uses_digest(const switch_gpu_backing_use *uses, uint32_t count)
{
	uint64_t digest = 0xcbf29ce484222325ull ^ uintptr_t(uses);
	for (uint32_t i = 0; i < count; ++i) {
		digest = (digest ^ uintptr_t(uses[i].backing)) * 0x100000001b3ull;
		digest = (digest ^ uses[i].access) * 0x100000001b3ull;
	}
	return digest | 1;
}
#endif

// Debug kernels: the use array still holds what begin left in it, or the transaction is not touched (its backings stay
// locked) rather than unlocked and recorded through whatever the array now holds. The ownership order, which needs no
// array, is let go.
inline bool uses_intact(switch_gpu_backing_transaction *transaction, const char *step)
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	if (!transaction->uses_check ||
	    uses_digest(transaction->uses, transaction->count) == transaction->uses_check)
		return true;
	if (transaction->ordered) {
		transaction->ordered = 0;
		switch_gpu_backing_order_unlock();
	}
	IOLog("SwitchGpu: engine %u queue %u job %llu: its %u uses changed between begin and %s "
	      "(the caller's array went "
	      "away); nothing is recorded or unlocked\n",
	      transaction->completion.engine, transaction->completion.queue,
	      transaction->completion.value, transaction->count, step);
	return false;
#else
	(void)transaction, (void)step;
	return true;
#endif
}
}  // namespace switch_gpu_ownership
