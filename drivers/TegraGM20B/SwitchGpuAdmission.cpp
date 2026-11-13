// SPDX-License-Identifier: MIT
#include "SwitchGpuAdmission.h"
#include "SwitchGpuBackingInternal.h"

namespace
{
constexpr uint32_t accessMask = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
}

SwitchGpuAdmission::~SwitchGpuAdmission()
{
	end();
	IODelete(uses_, switch_gpu_backing_use, capacity_);
}

int SwitchGpuAdmission::begin(const switch_gpu_backing_use *uses, uint32_t count,
			      const switch_gpu_backing_guard *guards, uint32_t guardCount,
			      switch_gpu_cpu_lease *const *leases, uint32_t leaseCount)
{
	if (active_ || (count && !uses) || (guardCount && !guards) || (leaseCount && !leases))
		return -22;
	if (count > capacity_) {
		auto *made = IONew(switch_gpu_backing_use, count);
		if (!made)
			return -12;
		IODelete(uses_, switch_gpu_backing_use, capacity_);
		uses_ = made;
		capacity_ = count;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i].backing || !(uses[i].access & accessMask) ||
		    (uses[i].access & ~accessMask) || uses[i].reserved)
			return -22;
		uses_[i] = uses[i];
	}
	switch_gpu_backing_sort_uses(uses_, count);
	uint32_t unique = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (unique && uses_[unique - 1].backing == uses_[i].backing)
			uses_[unique - 1].access |= uses_[i].access;
		else
			uses_[unique++] = uses_[i];
	}
	for (uint32_t i = 0; i < leaseCount; ++i) {
		if (!leases[i])
			return -22;
		for (uint32_t j = 0; j < i; ++j)
			if (leases[j] == leases[i])
				return -22;
	}
	for (uint32_t i = 0; i < unique; ++i)
		IOLockLock(uses_[i].backing->lock);
	int result = 0;
	for (uint32_t i = 0; !result && i < guardCount; ++i) {
		const auto &guard = guards[i];
		uint32_t first = 0, last = unique;
		while (guard.backing && first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (uses_[middle].backing->id < guard.backing->id)
				first = middle + 1;
			else
				last = middle;
		}
		const bool found =
			guard.backing && first != unique && uses_[first].backing == guard.backing;
		if (!found || !guard.generation || (uintptr_t(guard.generation) & 7) ||
		    !guard.expected)
			result = -22;
		else if (__atomic_load_n(guard.generation, __ATOMIC_ACQUIRE) != guard.expected)
			result = SWITCH_GPU_BACKING_STALE;
	}
	uint32_t matched = 0;
	for (uint32_t i = 0; !result && i < unique; ++i) {
		auto *backing = uses_[i].backing;
		uint32_t readers = switch_gpu_backing_exclusive_readers(backing),
			 writers = backing->cpu_writers, owned = 0;
		for (uint32_t j = 0; !result && j < leaseCount; ++j) {
			const auto *lease =
				switch_gpu_backing_find_lease_locked(backing, leases[j]);
			if (!lease)
				continue;
			++matched;
			auto &owners = lease->access & SWITCH_GPU_BACKING_WRITE ? writers : readers;
			if (++owned > 1 || !owners || lease->offset ||
			    lease->bytes != (uint64_t(backing->pages->count) << 12) ||
			    !(lease->access & SWITCH_GPU_BACKING_CPU_NO_CACHE) ||
			    (lease->producer.engine &&
			     (uses_[i].access & SWITCH_GPU_BACKING_WRITE)))
				result = -22;
			else
				--owners;
		}
		const bool write = uses_[i].access & SWITCH_GPU_BACKING_WRITE;
		if (!result && (writers || (write && readers)))
			result = -16;
		if (!result)
			switch_gpu_backing_prune_locked(backing);
		if (!result && (write ? backing->gpu_admission_writers
				      : backing->gpu_admission_readers) == UINT32_MAX)
			result = -75;
	}
	if (!result && matched != leaseCount)
		result = -22;
	if (!result) {
		for (uint32_t i = 0; i < unique; ++i) {
			auto *backing = uses_[i].backing;
			++(uses_[i].access & SWITCH_GPU_BACKING_WRITE
				   ? backing->gpu_admission_writers
				   : backing->gpu_admission_readers);
		}
		count_ = unique;
		active_ = true;
	}
	for (uint32_t i = unique; i; --i)
		IOLockUnlock(uses_[i - 1].backing->lock);
	if (!result)
		for (uint32_t i = 0; i < unique; ++i)
			switch_gpu_backing_retain(uses_[i].backing);
	return result;
}

bool SwitchGpuAdmission::covers(const switch_gpu_backing_use *uses, uint32_t count) const
{
	if (!active_ || (count && !uses))
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i].backing || !(uses[i].access & accessMask) ||
		    (uses[i].access & ~accessMask) || uses[i].reserved)
			return false;
		uint32_t first = 0, last = count_;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (uses_[middle].backing->id < uses[i].backing->id)
				first = middle + 1;
			else
				last = middle;
		}
		if (first == count_ || uses_[first].backing != uses[i].backing ||
		    (uses[i].access & ~uses_[first].access))
			return false;
	}
	return true;
}

void SwitchGpuAdmission::end()
{
	if (!active_)
		return;
	active_ = false;
	for (uint32_t i = 0; i < count_; ++i) {
		auto *backing = uses_[i].backing;
		IOLockLock(backing->lock);
		--(uses_[i].access & SWITCH_GPU_BACKING_WRITE ? backing->gpu_admission_writers
							      : backing->gpu_admission_readers);
		IOLockWakeup(backing->lock, &backing->cpu_writers, false);
		IOLockUnlock(backing->lock);
		switch_gpu_backing_release(backing);
	}
	count_ = 0;
	switch_gpu_admission_changed();
}

extern "C" int switch_gpu_backing_admission_ready(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing || !(access & accessMask) || (access & ~accessMask))
		return -22;
	IOLockLock(backing->lock);
	const bool ready =
		!backing->cpu_writers && (!(access & SWITCH_GPU_BACKING_WRITE) ||
					  !switch_gpu_backing_exclusive_readers(backing));
	IOLockUnlock(backing->lock);
	return ready ? 1 : 0;
}

extern "C" int switch_gpu_backing_conversion_ready(switch_gpu_backing *backing, uint32_t access)
{
	if (!backing || !(access & accessMask) || (access & ~accessMask))
		return -22;
	IOLockLock(backing->lock);
	const bool ready = !backing->cpu_writers && !backing->gpu_admission_writers &&
			   (!(access & SWITCH_GPU_BACKING_WRITE) ||
			    (!switch_gpu_backing_exclusive_readers(backing) &&
			     !backing->gpu_admission_readers));
	IOLockUnlock(backing->lock);
	return ready ? 1 : 0;
}
