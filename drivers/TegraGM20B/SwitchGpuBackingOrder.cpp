// SPDX-License-Identifier: MIT
#include <kern/thread.h>
#include "SwitchGpuOwnershipInternal.h"

namespace
{
IOLock *gOrder;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
void *gOrderOwner;
#endif
}  // namespace

IOLock *switch_gpu_backing_order(void)
{
	IOLock *lock = __atomic_load_n(&gOrder, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	if (!__atomic_compare_exchange_n(&gOrder, &lock, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return lock;
	}
	return made;
}

void switch_gpu_backing_order_lock(void)
{
	IOLockLock(switch_gpu_backing_order());
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	__atomic_store_n(&gOrderOwner, current_thread(), __ATOMIC_RELAXED);
#endif
}

void switch_gpu_backing_order_unlock(void)
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	__atomic_store_n(&gOrderOwner, nullptr, __ATOMIC_RELAXED);
#endif
	IOLockUnlock(gOrder);
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
void switch_gpu_backing_assert_held(const switch_gpu_backing *backing)
{
	if (__atomic_load_n(&gOrderOwner, __ATOMIC_RELAXED) == current_thread() &&
	    !switch_gpu_backing_needs_lock(backing))
		return;
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
	switch_gpu_backing_order_lock();
	for (uint32_t i = 0; i < count; ++i)
		++uses[i].backing->claims;
	switch_gpu_backing_order_unlock();
}

void switch_gpu_backing_unclaim_uses(const switch_gpu_backing_use *uses, uint32_t count)
{
	switch_gpu_backing_order_lock();
	for (uint32_t i = 0; i < count; ++i)
		--uses[i].backing->claims;
	switch_gpu_backing_order_unlock();
	for (uint32_t i = count; i; --i)
		IOLockUnlock(uses[i - 1].backing->lock);
}

void switch_gpu_backing_claim(switch_gpu_backing *backing)
{
	const switch_gpu_backing_use use = {backing, 0, 0};
	switch_gpu_backing_claim_uses(&use, 1);
}

void switch_gpu_backing_unclaim(switch_gpu_backing *backing)
{
	const switch_gpu_backing_use use = {backing, 0, 0};
	switch_gpu_backing_unclaim_uses(&use, 1);
}

bool switch_gpu_backing_enter_uses(const switch_gpu_backing_use *uses, uint32_t count)
{
	switch_gpu_backing_order_lock();
	bool locks = false;
	for (uint32_t i = 0; !locks && i < count; ++i)
		locks = switch_gpu_backing_needs_lock(uses[i].backing);
	if (!locks)
		return false;
	switch_gpu_backing_order_unlock();
	switch_gpu_backing_claim_uses(uses, count);
	return true;
}

void switch_gpu_backing_leave_uses(const switch_gpu_backing_use *uses, uint32_t count, bool claimed)
{
	if (claimed)
		switch_gpu_backing_unclaim_uses(uses, count);
	else
		switch_gpu_backing_order_unlock();
}

bool switch_gpu_backing_enter(switch_gpu_backing *backing)
{
	const switch_gpu_backing_use use = {backing, 0, 0};
	return switch_gpu_backing_enter_uses(&use, 1);
}

void switch_gpu_backing_leave(switch_gpu_backing *backing, bool claimed)
{
	const switch_gpu_backing_use use = {backing, 0, 0};
	switch_gpu_backing_leave_uses(&use, 1, claimed);
}

namespace switch_gpu_ownership
{
// The order's holder takes the locks of the uses that need them (switch_gpu_backing_needs_lock) before it, in
// allocation order: it lets the order and what it holds go, takes the grown set, and looks again, until no use it does
// not hold needs its lock. The set only grows, so the looking ends.
static bool lock_needed_uses(switch_gpu_backing_transaction *transaction)
{
	auto *uses = transaction->uses;
	bool grown = false;
	for (uint32_t i = 0; i < transaction->count; ++i)
		if (!(uses[i].reserved & kUseLocked) &&
		    switch_gpu_backing_needs_lock(uses[i].backing)) {
			uses[i].reserved |= kUseGrown;
			grown = true;
		}
	if (!grown)
		return false;
	switch_gpu_backing_order_unlock();
	for (uint32_t i = transaction->count; i; --i)
		if (uses[i - 1].reserved & kUseLocked)
			IOLockUnlock(uses[i - 1].backing->lock);
	for (uint32_t i = 0; i < transaction->count; ++i)
		if (uses[i].reserved & (kUseLocked | kUseGrown))
			IOLockLock(uses[i].backing->lock);
	transaction->locked = 0;
	for (uint32_t i = 0; i < transaction->count; ++i)
		if (uses[i].reserved & (kUseLocked | kUseGrown)) {
			uses[i].reserved = kUseLocked;
			++transaction->locked;
		}
	switch_gpu_backing_order_lock();
	return true;
}

// A CPU-dirty use is cleaned under its lock alone, so no cleaning holds the order: needing its lock, it is held, and it
// stays dirty until this transaction ends while everyone else waits for that lock. Begin skips a use marked cleaned; a
// begin refused before it leaves clean_start as it was, so the next one cleans again what a CPU writer wrote since.
// False when there was nothing to clean and the order was never let go.
static bool clean_outside_order(switch_gpu_backing_transaction *transaction)
{
	auto *uses = transaction->uses;
	bool dirty = false;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto *backing = uses[i].backing;
		if ((uses[i].reserved & kUseLocked) && !(uses[i].reserved & kUseCleaned) &&
		    (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) &&
		    backing->clean_start < (uint64_t(backing->pages->count) << 12)) {
			uses[i].reserved |= kUseDirty;
			dirty = true;
		}
	}
	if (!dirty)
		return false;
	switch_gpu_backing_order_unlock();
	for (uint32_t i = 0; i < transaction->count; ++i) {
		if (!(uses[i].reserved & kUseDirty))
			continue;
		uses[i].reserved = (uses[i].reserved & ~kUseDirty) | kUseCleaned;
		const auto *backing = uses[i].backing;
		const uint64_t size = uint64_t(backing->pages->count) << 12;
		switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
					 backing->clean_start, size - backing->clean_start, 0);
	}
	switch_gpu_backing_order_lock();
	return true;
}

// Each time the order was let go, a use not held may have been claimed meanwhile: look again.
void hold_ordered(switch_gpu_backing_transaction *transaction)
{
	switch_gpu_backing_order_lock();
	transaction->ordered = 1;
	while (lock_needed_uses(transaction) || clean_outside_order(transaction))
		;
}

void hold_claimed(switch_gpu_backing_transaction *transaction)
{
	switch_gpu_backing_claim_uses(transaction->uses, transaction->count);
	transaction->locked = transaction->count;
	transaction->claimed = 1;
}
}  // namespace switch_gpu_ownership
