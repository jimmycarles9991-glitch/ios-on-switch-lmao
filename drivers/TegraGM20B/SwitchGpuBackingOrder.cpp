// SPDX-License-Identifier: MIT
#include "SwitchGpuOwnershipInternal.h"
#include "TegraGM20BGpu.h"

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
void switch_gpu_backing_assert_held(const switch_gpu_backing *backing)
{
	IOLockAssert(backing->lock, kIOLockAssertOwned);
}

void switch_gpu_backing_assert_claimed(const switch_gpu_backing *backing)
{
	IOLockAssert(backing->lock, kIOLockAssertOwned);
}
#endif

void switch_gpu_backing_claim_uses(const switch_gpu_backing_use *uses, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
		IOLockLock(uses[i].backing->lock);
}

void switch_gpu_backing_unclaim_uses(const switch_gpu_backing_use *uses, uint32_t count)
{
	for (uint32_t i = count; i; --i)
		IOLockUnlock(uses[i - 1].backing->lock);
}

void switch_gpu_backing_claim(switch_gpu_backing *backing)
{
	IOLockLock(backing->lock);
}

void switch_gpu_backing_unclaim(switch_gpu_backing *backing)
{
	IOLockUnlock(backing->lock);
}

bool switch_gpu_backing_enter_uses(const switch_gpu_backing_use *uses, uint32_t count)
{
	switch_gpu_backing_claim_uses(uses, count);
	return true;
}

void switch_gpu_backing_leave_uses(const switch_gpu_backing_use *uses, uint32_t count, bool)
{
	switch_gpu_backing_unclaim_uses(uses, count);
}

bool switch_gpu_backing_enter(switch_gpu_backing *backing)
{
	IOLockLock(backing->lock);
	return true;
}

void switch_gpu_backing_leave(switch_gpu_backing *backing, bool)
{
	IOLockUnlock(backing->lock);
}

namespace switch_gpu_ownership
{
void hold_ordered(switch_gpu_backing_transaction *transaction)
{
	auto *uses = transaction->uses;
	switch_gpu_backing_claim_uses(uses, transaction->count);
	transaction->locked = transaction->count;
	transaction->ordered = 1;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		uses[i].reserved = kUseLocked;
		const auto *backing = uses[i].backing;
		if ((backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) &&
		    backing->clean_start < (uint64_t(backing->pages->count) << 12)) {
			uses[i].reserved |= kUseCleaned;
			const uint64_t size = uint64_t(backing->pages->count) << 12;
#if TGM_STATS
			if (backing->cpu_writers)
				switch_gpu_concurrent_clean_note(backing->surface_id,
								 size - backing->clean_start);
#endif
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
						 backing->clean_start, size - backing->clean_start,
						 0);
		}
	}
}

void hold_claimed(switch_gpu_backing_transaction *transaction)
{
	switch_gpu_backing_claim_uses(transaction->uses, transaction->count);
	transaction->locked = transaction->count;
	transaction->claimed = 1;
}
}  // namespace switch_gpu_ownership
