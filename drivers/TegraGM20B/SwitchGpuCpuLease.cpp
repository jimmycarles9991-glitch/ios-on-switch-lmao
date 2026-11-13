// SPDX-License-Identifier: MIT
#include <kern/thread.h>
#include "SwitchGpuBackingInternal.h"

extern "C" int switch_gpu_backing_cpu_acquire_owned(switch_gpu_backing *backing, uint64_t offset,
						    uint64_t bytes, uint32_t access,
						    switch_gpu_cpu_lease **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!(access & SWITCH_GPU_BACKING_CPU_NO_CACHE))
		return -22;
	auto *lease = IONew(switch_gpu_cpu_lease, 1);
	if (!lease)
		return -12;
	const int result = switch_gpu_backing_cpu_acquire(backing, offset, bytes, access);
	if (result) {
		IODelete(lease, switch_gpu_cpu_lease, 1);
		return result;
	}
	IOLockLock(backing->lock);
	*lease = {backing->cpu_leases, backing, offset, bytes, access, {}};
	backing->cpu_leases = lease;
	IOLockUnlock(backing->lock);
	*out = lease;
	return 0;
}

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
	if (!owned || !*owned)
		return -22;
	auto *lease = *owned;
	auto *backing = lease->backing;
	IOLockLock(backing->lock);
	auto **at = &backing->cpu_leases;
	while (*at && *at != lease)
		at = &(*at)->next;
	if (!*at) {
		IOLockUnlock(backing->lock);
		return -22;
	}
	*at = lease->next;
	IOLockUnlock(backing->lock);
	if ((lease->access & SWITCH_GPU_BACKING_WRITE) && !wrote) {
		switch_gpu_backing_cpu_cancel(lease->backing, lease->access);
	} else {
		const int result = switch_gpu_backing_cpu_release(lease->backing, lease->offset,
								  lease->bytes, lease->access);
		if (result) {
			IOLockLock(backing->lock);
			lease->next = backing->cpu_leases;
			backing->cpu_leases = lease;
			IOLockUnlock(backing->lock);
			return result;
		}
	}
	if (lease->producer.engine)
		switch_gpu_backing_drop_token(lease->producer);
	*owned = nullptr;
	IODelete(lease, switch_gpu_cpu_lease, 1);
	return 0;
}

const switch_gpu_cpu_lease *switch_gpu_backing_find_lease_locked(const switch_gpu_backing *backing,
								 const switch_gpu_cpu_lease *lease)
{
	for (auto *at = backing->cpu_leases; at; at = at->next)
		if (at == lease)
			return at;
	return nullptr;
}

extern "C" int switch_gpu_backing_cpu_publish_generation(switch_gpu_backing *backing,
							 switch_gpu_cpu_lease *lease,
							 volatile uint64_t *generation,
							 uint64_t expected, uint64_t next)
{
	if (!backing || !lease || !generation || (uintptr_t(generation) & 7) || !expected ||
	    next <= expected)
		return -22;
	IOLockLock(backing->lock);
	const auto *held = switch_gpu_backing_find_lease_locked(backing, lease);
	int result = !held || !(held->access & SWITCH_GPU_BACKING_WRITE) ||
				     !(held->access & SWITCH_GPU_BACKING_CPU_NO_CACHE) ||
				     held->offset ||
				     held->bytes != (uint64_t(backing->pages->count) << 12)
			     ? -22
			     : 0;
	if (!result && (backing->cpu_writers != 1 || backing->cpu_readers))
		result = -16;
	if (!result && !__atomic_compare_exchange_n(generation, &expected, next, false,
						    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		result = SWITCH_GPU_BACKING_STALE;
	IOLockUnlock(backing->lock);
	return result;
}

// A surface with writer promises pending is snapshotted behind a lease the promised job's admission does not wait for,
// with the promises as tokens when the reader asks (`promises`): an engine reader orders behind them on the GPU. Only a
// CPU reader (`cpu`: the display's CPU copy) cannot, and sleeps until the promised job is submitted, as a reader that
// may wait (`!avoid`) does for a CPU writer.
static int snapshot_read(switch_gpu_backing *backing, switch_gpu_backing_token *producer,
			 switch_gpu_cpu_lease **out, bool allow_cpu, bool avoid, bool cpu = false,
			 switch_gpu_backing_token *promises = nullptr, uint32_t capacity = 0,
			 uint32_t *promised = nullptr)
{
	if (!backing || !producer || !out)
		return -22;
	*producer = {};
	*out = nullptr;
	auto *lease = IONew(switch_gpu_cpu_lease, 1);
	if (!lease)
		return -12;
	switch_gpu_backing_retain(backing);
	IOLockLock(backing->lock);
	switch_gpu_promise_parked parked = {};
	while (!avoid && (backing->cpu_writers || backing->gpu_admission_writers ||
			  (cpu && switch_gpu_backing_promise_pending_locked(backing))))
		switch_gpu_backing_sleep_promised_locked(backing, &parked,
							 cpu ? "a CPU copy" : nullptr);
	switch_gpu_promise_parked_leave(&parked);
	int result = backing->cpu_writers || backing->gpu_admission_writers ||
				     backing->cpu_readers == UINT32_MAX
			     ? -16
			     : 0;
	int pending = 0;
	if (!result && backing->promise_count) {
		pending = switch_gpu_backing_promises_locked(backing, 0, 0, promises, capacity);
		result = pending < 0 ? pending : 0;
	}
	const switch_gpu_backing_access *writer = nullptr;
	bool failed_writer = false;
	for (uint32_t i = 0; !result && i < backing->access_count; ++i) {
		auto &access = backing->accesses[i];
		if (!(access.access & SWITCH_GPU_BACKING_WRITE) ||
		    access.content != backing->content)
			continue;
		// A failed producer leaves undefined bytes and nothing to wait for: the snapshot goes on without it.
		const int status = switch_gpu_backing_token_status(&access.token);
		if (status < 0) {
			switch_gpu_backing_note_failure_locked(backing, access, status);
			failed_writer = true;
		} else {
			writer = &access;
		}
	}
	if (!result && !writer && !failed_writer && !allow_cpu)
		result = SWITCH_GPU_BACKING_STALE;
	if (!result && writer)
		result = switch_gpu_backing_engine_ops(writer->token.engine)
				 ->retain_token(&writer->token);
	if (!result) {
		if (!writer && (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY)) {
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
						 0, uint64_t(backing->pages->count) << 12,
						 SWITCH_GPU_CACHE_CLEAN);
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
			backing->initial_clean = true;
		}
		++backing->cpu_readers;
		backing->promised_readers += pending > 0;
		*lease = {backing->cpu_leases,
			  backing,
			  0,
			  uint64_t(backing->pages->count) << 12,
			  SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE |
				  (pending > 0 ? uint32_t(SWITCH_GPU_BACKING_PROMISED) : 0u),
			  writer ? writer->token : switch_gpu_backing_token{}};
		backing->cpu_leases = lease;
		*producer = lease->producer;
		*out = lease;
		if (promised)
			*promised = promises && pending > 0 ? uint32_t(pending) : 0u;
	}
	IOLockUnlock(backing->lock);
	if (result) {
		switch_gpu_backing_release(backing);
		IODelete(lease, switch_gpu_cpu_lease, 1);
	}
	return result;
}

extern "C" int switch_gpu_backing_export_read(switch_gpu_backing *backing,
					      switch_gpu_backing_token *producer,
					      switch_gpu_cpu_lease **out)
{
	return snapshot_read(backing, producer, out, false, true);
}

extern "C" int switch_gpu_backing_snapshot_read(switch_gpu_backing *backing,
						switch_gpu_backing_token *producer,
						switch_gpu_cpu_lease **out)
{
	return snapshot_read(backing, producer, out, true, true);
}

extern "C" int switch_gpu_backing_snapshot_read_promises(switch_gpu_backing *backing,
							 switch_gpu_backing_token *producer,
							 switch_gpu_cpu_lease **out,
							 switch_gpu_backing_token *promises,
							 uint32_t capacity)
{
	uint32_t promised = 0;
	const int result = snapshot_read(backing, producer, out, true, true, false, promises,
					 capacity, &promised);
	return result ? result : int(promised);
}

extern "C" int switch_gpu_backing_snapshot_read_wait(switch_gpu_backing *backing,
						     switch_gpu_backing_token *producer,
						     switch_gpu_cpu_lease **out)
{
	return snapshot_read(backing, producer, out, true, false);
}

extern "C" int switch_gpu_backing_snapshot_read_cpu(switch_gpu_backing *backing,
						    switch_gpu_backing_token *producer,
						    switch_gpu_cpu_lease **out)
{
	return snapshot_read(backing, producer, out, true, false, true);
}

extern "C" int switch_gpu_backing_conversion_reserve(switch_gpu_backing *backing, uint32_t access,
						     int avoid, switch_gpu_cpu_lease **out)
{
	if (!out)
		return -22;
	*out = nullptr;
	if (!backing || (access != SWITCH_GPU_BACKING_READ && access != SWITCH_GPU_BACKING_WRITE))
		return -22;
	auto *lease = IONew(switch_gpu_cpu_lease, 1);
	if (!lease)
		return -12;
	switch_gpu_backing_retain(backing);
	IOLockLock(backing->lock);
	const bool write = access == SWITCH_GPU_BACKING_WRITE;
	int result = 0;
	while (backing->cpu_writers || backing->gpu_admission_writers ||
	       (write && (switch_gpu_backing_exclusive_readers(backing) ||
			  backing->gpu_admission_readers))) {
		if (avoid) {
			result = -16;
			break;
		}
		switch_gpu_backing_sleep_cpu_locked(backing);
	}
	if (!result)
		switch_gpu_backing_prune_locked(backing);
	auto &users = write ? backing->cpu_writers : backing->cpu_readers;
	if (!result && users == UINT32_MAX)
		result = -75;
	if (!result) {
		++users;
		*lease = {backing->cpu_leases,
			  backing,
			  0,
			  uint64_t(backing->pages->count) << 12,
			  access | SWITCH_GPU_BACKING_CPU_NO_CACHE,
			  {}};
		backing->cpu_leases = lease;
		*out = lease;
	}
	IOLockUnlock(backing->lock);
	if (result) {
		switch_gpu_backing_release(backing);
		IODelete(lease, switch_gpu_cpu_lease, 1);
	}
	return result;
}
