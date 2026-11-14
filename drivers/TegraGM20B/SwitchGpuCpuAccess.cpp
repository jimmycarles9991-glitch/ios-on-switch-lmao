// SPDX-License-Identifier: MIT
#include <kern/thread.h>
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuAdmission.h"

namespace
{
constexpr uint32_t kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ;
constexpr uint32_t kAvoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kDiscard = SWITCH_GPU_BACKING_CPU_DISCARD;
constexpr uint32_t kNoCache = SWITCH_GPU_BACKING_CPU_NO_CACHE;
constexpr uint32_t kConcurrent = SWITCH_GPU_BACKING_CPU_CONCURRENT;
constexpr uint32_t kPreserve = SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY;

bool valid_range(switch_gpu_backing *backing, uint64_t offset, uint64_t bytes, uint32_t access)
{
	const uint64_t size = uint64_t(backing->pages->count) << 12;
	return (access & (kRead | kWrite)) &&
	       !(access & ~(kRead | kWrite | kAvoid | kDiscard | kNoCache | kConcurrent |
			    kPreserve | SWITCH_GPU_BACKING_PROMISED)) &&
	       bytes && offset <= size && bytes <= size - offset &&
	       (!(access & kPreserve) || access == (kRead | kAvoid | kPreserve)) &&
	       (!(access & kConcurrent) || (!offset && bytes == size)) &&
	       (!(access & kDiscard) ||
		((access & kWrite) && !(access & kRead) && !((offset | bytes) & 63)));
}

bool conflicts(uint32_t recorded, uint32_t access)
{
	return (recorded | access) & kWrite;
}

int wait_locked(switch_gpu_backing *backing, uint32_t access)
{
	for (;;) {
		switch_gpu_backing_prune_locked(backing);
		switch_gpu_backing_token token = {};
		bool external = false;
		for (uint32_t i = 0; i < backing->access_count; ++i) {
			if (conflicts(backing->accesses[i].access, access) &&
			    ((backing->accesses[i].access & kWrite)
				     ? switch_gpu_backing_token_external_status(
					       &backing->accesses[i].token)
				     : switch_gpu_backing_token_status(
					       &backing->accesses[i].token)) == 0) {
				token = backing->accesses[i].token;
				external = backing->accesses[i].access & kWrite;
				break;
			}
		}
		if (!token.engine)
			return 0;
		if (access & kAvoid)
			return -16;
		const auto *ops = switch_gpu_backing_engine_ops(token.engine);
		int result = ops->retain_token(&token);
		if (result)
			return result;
		IOLockUnlock(backing->lock);
		result = external ? switch_gpu_backing_token_external_wait(&token)
				  : ops->wait(&token);
		const int settled = result >= 0 ? result
				    : external  ? switch_gpu_backing_token_external_status(&token)
						: ops->status(&token);
		ops->release_token(&token);
		IOLockLock(backing->lock);
		// A producer that failed while waited for is the next prune's to retire; the waiter goes on.
		if (!settled)
			return result < 0 ? result : -110;
	}
}

}  // namespace

uint32_t switch_gpu_cpu_sleepers;

void switch_gpu_backing_sleep_cpu_locked(switch_gpu_backing *backing)
{
	switch_gpu_backing_assert_claimed(backing);
	__atomic_add_fetch(&switch_gpu_cpu_sleepers, 1u, __ATOMIC_ACQ_REL);
	IOLockSleep(backing->lock, &backing->cpu_writers, THREAD_UNINT);
	__atomic_sub_fetch(&switch_gpu_cpu_sleepers, 1u, __ATOMIC_ACQ_REL);
}

namespace
{
bool cpu_conflicts(const switch_gpu_backing *backing, uint32_t access)
{
	const bool concurrent = access & kConcurrent;
	const uint32_t writers =
		backing->cpu_writers - (concurrent ? backing->concurrent_writers : 0);
	const uint32_t readers =
		backing->cpu_readers - (concurrent ? backing->concurrent_readers : 0);
	return writers || backing->gpu_admission_writers ||
	       switch_gpu_backing_promise_pending_locked(backing) ||
	       ((access & kWrite) && (readers || backing->gpu_admission_readers));
}

void reserve_cpu_locked(switch_gpu_backing *backing, uint32_t access)
{
	switch_gpu_backing_cpu_owners_add(
		(access & kWrite) ? backing->cpu_writers : backing->cpu_readers, 1);
	if (access & kConcurrent)
		++((access & kWrite) ? backing->concurrent_writers : backing->concurrent_readers);
}

void release_cpu_locked(switch_gpu_backing *backing, uint32_t access)
{
	if (access & kConcurrent)
		--((access & kWrite) ? backing->concurrent_writers : backing->concurrent_readers);
	switch_gpu_backing_cpu_owners_sub(
		(access & kWrite) ? backing->cpu_writers : backing->cpu_readers, 1);
	if (access & SWITCH_GPU_BACKING_PROMISED)
		--backing->promised_readers;
	IOLockWakeup(backing->lock, &backing->cpu_writers, false);
	switch_gpu_admission_changed();
}

// A section that cleans what the CPU wrote claims the backing: the cache pass grows with it, and never holds the order.
bool enter_cleaning(switch_gpu_backing *backing, bool cleans)
{
	if (!cleans)
		return switch_gpu_backing_enter(backing);
	switch_gpu_backing_claim(backing);
	return true;
}
}  // namespace

extern "C" int switch_gpu_backing_wait_visible(switch_gpu_backing *backing)
{
	if (!backing)
		return -22;
	switch_gpu_backing_claim(backing);
	const int result = wait_locked(backing, kRead);
	switch_gpu_backing_unclaim(backing);
	return result;
}

extern "C" int switch_gpu_backing_cpu_acquire(switch_gpu_backing *backing, uint64_t offset,
					      uint64_t bytes, uint32_t access)
{
	if (!backing)
		return -22;
	switch_gpu_backing_retain(backing);
	switch_gpu_backing_claim(backing);
	const bool valid = valid_range(backing, offset, bytes, access) &&
			   !(access & SWITCH_GPU_BACKING_PROMISED);
	int result = valid ? 0 : -22;
	switch_gpu_promise_parked parked = {};
	while (!result && cpu_conflicts(backing, access)) {
		if (access & kAvoid)
			result = -16;
		else
			switch_gpu_backing_sleep_promised_locked(backing, &parked, "a CPU lock");
	}
	switch_gpu_promise_parked_leave(&parked);
	bool held = false;
	if (!result) {
		reserve_cpu_locked(backing, access);
		held = true;
		result = wait_locked(backing, access);
	}
	if (!result && (backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY) && !(access & kNoCache) &&
	    !switch_gpu_backing_cpu_current_prepare_locked(backing))
		result = -12;
	if (!result) {
		const uint64_t size = uint64_t(backing->pages->count) << 12;
		if ((backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY) &&
		    !(access & (kDiscard | kNoCache)))
			switch_gpu_backing_cpu_cache_locked(
				backing, offset, bytes,
				access & kPreserve ? SWITCH_GPU_CACHE_CLEAN_INVALIDATE
						   : SWITCH_GPU_CACHE_INVALIDATE);
		if (!(access & kNoCache) && !offset && bytes == size)
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_GPU_DIRTY);
		if ((access & kWrite) && !(access & kNoCache))
			backing->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
	}
	if (result && held)
		release_cpu_locked(backing, access);
	switch_gpu_backing_unclaim(backing);
	if (result)
		switch_gpu_backing_release(backing);
	return result;
}

int switch_gpu_backing_cpu_release_locked(switch_gpu_backing *backing, uint64_t offset,
					  uint64_t bytes, uint32_t access)
{
	if (!valid_range(backing, offset, bytes, access) ||
	    ((access & kWrite) ? !backing->cpu_writers : !backing->cpu_readers) ||
	    ((access & kConcurrent) &&
	     ((access & kWrite) ? !backing->concurrent_writers : !backing->concurrent_readers)))
		return -22;
	if (access & kWrite) {
		if (!(access & kNoCache))
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
						 offset, bytes, 0);
		if (!(access & kNoCache))
			switch_gpu_backing_cpu_current_written_locked(backing, offset, bytes);
		const bool current =
			!(access & kNoCache) && backing->cpu_current_content == backing->content;
		++backing->content;
		if (current)
			backing->cpu_current_content = backing->content;
		if (!(access & kNoCache))
			++backing->host_epoch;
		if (backing->initial_clean && backing->cpu_writers == 1)
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
	}
	release_cpu_locked(backing, access);
	return 0;
}

void switch_gpu_backing_cpu_cancel_locked(switch_gpu_backing *backing, uint32_t access)
{
	if ((access & kWrite) && backing->initial_clean && backing->cpu_writers == 1)
		backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
	release_cpu_locked(backing, access);
}

bool switch_gpu_backing_enter_releasing(switch_gpu_backing *backing, uint32_t access)
{
	return enter_cleaning(backing, (access & kWrite) && !(access & kNoCache));
}

extern "C" int switch_gpu_backing_cpu_release(switch_gpu_backing *backing, uint64_t offset,
					      uint64_t bytes, uint32_t access)
{
	if (!backing)
		return -22;
	const bool claimed = switch_gpu_backing_enter_releasing(backing, access);
	const int result = switch_gpu_backing_cpu_release_locked(backing, offset, bytes, access);
	switch_gpu_backing_leave(backing, claimed);
	if (!result)
		switch_gpu_backing_release(backing);
	return result;
}

extern "C" int switch_gpu_backing_wait(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing || !(access & (kRead | kWrite)) || (access & ~(kRead | kWrite | kAvoid)))
		return -22;
	switch_gpu_backing_claim(backing);
	const int result = wait_locked(backing, access);
	switch_gpu_backing_unclaim(backing);
	return result;
}

extern "C" int switch_gpu_backing_pending(switch_gpu_backing *backing, uint32_t access,
					  uint32_t *engines, uint32_t *cpu_owners)
{
	if (!backing || !engines || !cpu_owners || !(access & (kRead | kWrite)) ||
	    (access & ~(kRead | kWrite)))
		return -22;
	uint32_t found = 0;
	const bool claimed = switch_gpu_backing_enter(backing);
	switch_gpu_backing_prune_locked(backing);
	for (uint32_t i = 0; i < backing->access_count; ++i) {
		const auto &recorded = backing->accesses[i];
		if (!conflicts(recorded.access, access) ||
		    recorded.token.engine >= SWITCH_GPU_ENGINE_COUNT)
			continue;
		const int status =
			(recorded.access & kWrite)
				? switch_gpu_backing_token_external_status(&recorded.token)
				: switch_gpu_backing_token_status(&recorded.token);
		if (!status)
			found |= 1u << recorded.token.engine;
	}
	*cpu_owners = backing->cpu_readers + backing->cpu_writers;
	switch_gpu_backing_leave(backing, claimed);
	*engines = found;
	return 0;
}

extern "C" int switch_gpu_backing_cpu_publish(switch_gpu_backing *backing, uint64_t offset,
					      uint64_t bytes)
{
	if (!backing)
		return -22;
	switch_gpu_backing_claim(backing);
	const int result =
		!backing->cpu_writers || !valid_range(backing, offset, bytes, kWrite) ? -22 : 0;
	if (!result) {
		switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count, offset,
					 bytes, 0);
		++backing->host_epoch;
	}
	switch_gpu_backing_unclaim(backing);
	return result;
}

extern "C" void switch_gpu_backing_cpu_cancel(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing)
		return;
	const bool claimed = switch_gpu_backing_enter(backing);
	switch_gpu_backing_cpu_cancel_locked(backing, access);
	switch_gpu_backing_leave(backing, claimed);
	switch_gpu_backing_release(backing);
}

extern "C" int switch_gpu_backing_cpu_transfer_owned(switch_gpu_backing *backing,
						     switch_gpu_cpu_lease **owned, uint32_t access)
{
	if (!backing || !owned || !*owned || (access & ~(kRead | kWrite | kAvoid | kConcurrent)))
		return -22;
	switch_gpu_backing_claim(backing);
	auto *held = const_cast<switch_gpu_cpu_lease *>(
		switch_gpu_backing_find_lease_locked(backing, *owned));
	const uint64_t bytes = uint64_t(backing->pages->count) << 12;
	int result = !held || !(held->access & kNoCache) || held->offset || held->bytes != bytes ||
				     ((access & kWrite) && !(held->access & kWrite)) ||
				     !valid_range(backing, 0, bytes, access)
			     ? -22
			     : 0;
	if (!result) {
		const bool write = held->access & kWrite, concurrent = held->access & kConcurrent;
		uint32_t writers = backing->cpu_writers - unsigned(write);
		uint32_t readers = backing->cpu_readers - unsigned(!write);
		if (access & kConcurrent) {
			writers -= backing->concurrent_writers - unsigned(concurrent && write);
			readers -= backing->concurrent_readers - unsigned(concurrent && !write);
		}
		if (writers || ((access & kWrite) && readers))
			result = -16;
	}
	if (!result)
		result = wait_locked(backing, access);
	if (!result && (backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY) &&
	    backing->cpu_writers > unsigned(bool(held->access & kWrite)))
		result = -16;
	if (!result && (backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY) &&
	    !switch_gpu_backing_cpu_current_prepare_locked(backing))
		result = -12;
	if (!result) {
		if (backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY)
			switch_gpu_backing_cpu_cache_locked(backing, 0, bytes,
							    SWITCH_GPU_CACHE_INVALIDATE);
		backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_GPU_DIRTY);
		if (access & kWrite)
			backing->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
		auto **at = &backing->cpu_leases;
		while (*at != held)
			at = &(*at)->next;
		*at = held->next;
		release_cpu_locked(backing, held->access);
		reserve_cpu_locked(backing, access);
		*owned = nullptr;
	}
	switch_gpu_backing_unclaim(backing);
	if (!result) {
		if (held->producer.engine)
			switch_gpu_backing_drop_token(held->producer);
		IODelete(held, switch_gpu_cpu_lease, 1);
	}
	return result;
}

extern "C" int switch_gpu_backing_cpu_adopt(switch_gpu_backing *backing, uint32_t readers,
					    uint32_t writers)
{
	if (!backing || readers > UINT32_MAX - writers)
		return -22;
	const uint32_t total = readers + writers;
	if (!total)
		return 0;
	for (uint32_t i = 0; i < total; ++i)
		switch_gpu_backing_retain(backing);
	switch_gpu_backing_claim(backing);
	const uint32_t access = (writers ? kWrite : kRead) | kConcurrent | kAvoid;
	int result = readers > UINT32_MAX - backing->cpu_readers ||
				     writers > UINT32_MAX - backing->cpu_writers
			     ? -22
		     : cpu_conflicts(backing, access) ? -16
						      : wait_locked(backing, access);
	if (!result) {
		switch_gpu_backing_cpu_owners_add(backing->cpu_readers, readers);
		backing->concurrent_readers += readers;
		switch_gpu_backing_cpu_owners_add(backing->cpu_writers, writers);
		backing->concurrent_writers += writers;
		if (writers)
			backing->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
	}
	switch_gpu_backing_unclaim(backing);
	if (result)
		for (uint32_t i = 0; i < total; ++i)
			switch_gpu_backing_release(backing);
	return result;
}
