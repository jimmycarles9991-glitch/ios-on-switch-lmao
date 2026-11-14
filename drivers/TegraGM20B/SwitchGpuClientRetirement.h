// SPDX-License-Identifier: MIT
#pragma once

void AGXDeviceUserClient::accountRelease(Bo *bo)
{
	if (bo->handle && (bo->pool_va || bo->chunked))
		pool_bytes_ -= bo->bytes;
	if (bo->handle && bo->pool_va && (bo->flags & TegraGM20BBoControl))
		control_bytes_ -= bo->controlCharge;
	if (SWITCH_GPU_OWNER_COUNTS && bo->handle)
		ownerMemory_.change(bo->ownerKind, bo->bytes, false);
}

void AGXDeviceUserClient::readyBo(Bo *bo)
{
	if (!bo->zombieLink || bo->bindings)
		return;
	*bo->zombieLink = bo->next;
	if (bo->next)
		bo->next->zombieLink = bo->zombieLink;
	bo->zombieLink = nullptr;
	accountRelease(bo);
	bo->next = readyBos_;
	readyBos_ = bo;
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::queueZombie(Bo *bo)
{
	bo->next = zombies_;
	bo->zombieLink = &zombies_;
	if (zombies_)
		zombies_->zombieLink = &bo->next;
	zombies_ = bo;
	readyBo(bo);
}

void AGXDeviceUserClient::scheduleRetirement()
{
	if (!__atomic_load_n(&retirementPending_, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		return;
	__atomic_add_fetch(&retirementEpoch_, 1u, __ATOMIC_RELEASE);
	thread_call_enter(retireCall_);
}

// A kept writer promise is released by its job's end or, after a kill of its channel, by retirement.
void AGXDeviceUserClient::retirementFence(void *owner)
{
	auto *client = static_cast<AGXDeviceUserClient *>(owner);
	if (__atomic_load_n(&client->promisesKept_, __ATOMIC_RELAXED))
		__atomic_store_n(&client->retirementPending_, 1u, __ATOMIC_RELEASE);
	client->scheduleRetirement();
}

void AGXDeviceUserClient::retirementWork(thread_call_param_t owner, thread_call_param_t)
{
	static_cast<AGXDeviceUserClient *>(owner)->retireBatch();
}

bool AGXDeviceUserClient::bindingIdle(const RetiredBinding *binding) const
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		const auto &snapshot = binding->snapshots[slot];
		if (!snapshot.engine || !chans_[slot])
			continue;
		switch_gpu_backing_token current = {};
		switch_gpu_chan_next_token(chans_[slot], &current);
		if (snapshot.queue == current.queue &&
		    snapshot.reset_generation == current.reset_generation &&
		    !tgm_chan_error(chans_[slot]) &&
		    int32_t(tgm_chan_completed(chans_[slot]) - uint32_t(snapshot.value)) < 0)
			return false;
	}
	return true;
}

void AGXDeviceUserClient::retireBatch()
{
	constexpr uint32_t batch = 64;
	IOLockLock(lock_);
	if (__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE)) {
		IOLockUnlock(lock_);
		return;
	}
	if (!retireRemaining_) {
		retireRemaining_ = retiredCount_;
		retireScanEpoch_ = __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	}
	for (uint32_t checked = 0; retired_ && retireRemaining_ && checked < batch; ++checked) {
		auto *record = retired_;
		retired_ = record->next;
		if (!retired_)
			retiredTail_ = &retired_;
		--retireRemaining_;
		if (bindingIdle(record)) {
			--retiredCount_;
			record->next = unmapping_;
			unmapping_ = record;
		} else {
			record->next = nullptr;
			*retiredTail_ = record;
			retiredTail_ = &record->next;
		}
	}
	auto *ready = unmapping_;
	logFaultBindings();
	releasePromises(false);
	IOLockUnlock(lock_);
	IOLockLock(unmapLock_);
	for (auto *record = ready; record; record = record->next)
		switch_gpu_vm_unmap(vm_, &record->mapping);
	IOLockUnlock(unmapLock_);
	IOLockLock(lock_);
	unmapping_ = nullptr;
	while (ready) {
		auto *next = ready->next;
		noteBind(TGM_BIND_GONE, ready->va, ready->range, ready->bo, 0, ready->kind);
		--ready->bo->bindings;
		readyBo(ready->bo);
		IODelete(ready, RetiredBinding, 1);
		ready = next;
	}
	installPendingBinds();
	Bo *release = nullptr;
	for (uint32_t count = 0; readyBos_ && count < batch; ++count) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		bo->next = release;
		release = bo;
	}
	Left left = {release, boIndex_.takeRetired(), bindTable_.takeRetired()};
	const bool mutation = submitGate_.mutating();
	__atomic_store_n(&retirementPending_, unsigned(retired_ || readyBos_), __ATOMIC_RELEASE);
	const bool again = readyBos_ || retireRemaining_ ||
			   retireScanEpoch_ != __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	if (again && !__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		thread_call_enter(retireCall_);
	IOLockWakeup(lock_, &retired_, false);
	IOLockUnlock(lock_);
	if (!left.bos && !left.indexes && !left.bindings)
		return;
	if (!mutation && passSubmitters(false)) {
		dropLeft(left);
		return;
	}
	IOLockLock(lock_);
	keepLeft(left);
	IOLockUnlock(lock_);
	thread_call_enter(releaseCall_);
}

// Every SubmitJob finds the connection's BOs and bindings inside its channel's submit lock, without lock_: once each
// submit lock was taken after something left the tables, no job can still hold it. Not under lock_ (SubmitJob takes
// lock_ inside its submit lock). Without `wait`, false at the first lock held: a submit lock is held across a full
// ring's wait, which waits for the GPU, which may wait for the bind epoch only retireBatch releases.
bool AGXDeviceUserClient::passSubmitters(bool wait)
{
	for (IOLock *lock : submitLocks_) {
		if (wait)
			IOLockLock(lock);
		else if (!IOLockTryLock(lock))
			return false;
		IOLockUnlock(lock);
	}
	return true;
}

void AGXDeviceUserClient::dropLeft(const Left &left)
{
	for (Bo *bo = left.bos; bo;) {
		Bo *next = bo->next;
		releaseBo(bo);
		bo = next;
	}
	boIndex_.releaseRetired(left.indexes);
	BindingTable::releaseRetired(left.bindings);
}

// lock_ held: `left` waits for releaseLeft
void AGXDeviceUserClient::keepLeft(const Left &left)
{
	const auto append = [](auto *&chain, auto *more, auto link) {
		auto **at = &chain;
		while (*at)
			at = &((*at)->*link);
		*at = more;
	};
	append(left_.bos, left.bos, &Bo::next);
	append(left_.indexes, left.indexes, &BoIndex::Table::retired);
	append(left_.bindings, left.bindings, &BindingTable::Snapshot::retired);
}

// releaseCall_'s: what retireBatch could not give back without waiting, for a submit lock or for a gated method, which
// may hold a BO across a drop of lock_ (exec's resolved uses, nativeSurfaceView's source)
void AGXDeviceUserClient::releaseLeft()
{
	IOLockLock(lock_);
	const Left left = left_;
	left_ = {};
	while ((left.bos || left.indexes || left.bindings) && submitGate_.mutating())
		IOLockSleep(lock_, &submitGate_, THREAD_UNINT);
	IOLockUnlock(lock_);
	if (!left.bos && !left.indexes && !left.bindings)
		return;
	passSubmitters(true);
	dropLeft(left);
}

void AGXDeviceUserClient::releaseWork(thread_call_param_t owner, thread_call_param_t)
{
	static_cast<AGXDeviceUserClient *>(owner)->releaseLeft();
}

// lock_ held: the bindings as they are now become what SubmitJob reads; the copy they replace waits for retireBatch.
void AGXDeviceUserClient::publishBindings(BindingTable::Snapshot *made)
{
	bindTable_.publish(made, binds_, nbinds_);
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
	scheduleRetirement();
}

// lock_ held. The PTEs of retired bindings in [va, va + bytes) go now, so a new mapping can take the range; their pages
// stay until the GPU work that may use them completes. A record retireBatch is unmapping is waited for (unmapLock_), never
// the GPU.
void AGXDeviceUserClient::takeRetiredOverlaps(uint64_t va, uint64_t bytes)
{
	const auto overlaps = [=](const RetiredBinding *record) {
		uint64_t from = 0, span = 0;
		return !switch_gpu_mapping_span(record->mapping, &from, &span) &&
		       from < va + bytes && va < from + span;
	};
	RetiredBinding *const lists[] = {retired_, unmapping_};
	IOLockLock(unmapLock_);
	for (auto *list : lists)
		for (auto *record = list; record; record = record->next)
			if (record->mapping && overlaps(record)) {
				switch_gpu_vm_unmap(vm_, &record->mapping);
				noteBind(TGM_BIND_GONE, record->va, record->range, record->bo, 0,
					 record->kind);
			}
	IOLockUnlock(unmapLock_);
}

void AGXDeviceUserClient::retire(RetiredBinding *record)
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		record->snapshots[slot] = {};
		if (chans_[slot]) {
			switch_gpu_chan_next_token(chans_[slot], &record->snapshots[slot]);
			record->snapshots[slot].value = uint32_t(record->snapshots[slot].value - 1);
			// its retirement (and the bind epoch behind it) waits for that seqno's interrupt
			tgm_chan_need(chans_[slot], uint32_t(record->snapshots[slot].value));
		}
	}
	record->next = nullptr;
	*retiredTail_ = record;
	retiredTail_ = &record->next;
	++retiredCount_;
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::reap(bool force)
{
	if (!force)
		return;
	while (retired_) {
		auto *record = retired_;
		retired_ = record->next;
		switch_gpu_vm_unmap(vm_, &record->mapping);
		--record->bo->bindings;
		readyBo(record->bo);
		IODelete(record, RetiredBinding, 1);
	}
	retiredTail_ = &retired_;
	retiredCount_ = retireRemaining_ = 0;
	while (readyBos_) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		releaseBo(bo);
	}
	__atomic_store_n(&retirementPending_, 0u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::drainResources()
{
	IOLockLock(lock_);
	submitGate_.close(lock_);
	__atomic_store_n(&retirementStopped_, 1u, __ATOMIC_RELEASE);
	IOLockUnlock(lock_);
	events_.stop();
	resources_.stop();
	releaseMappings();
#if TGM_STATS
	tgm_readback_closed(this);
#endif
	IOLockLock(lock_);
	releaseCpuLeases();
	releasePromises(true);
	hangHolder_ = current_thread();
	for (auto &chan : chans_)
		tgm_chan_del(&chan);
	destroySets();
	for (auto &channel : eventChannels_)
		channel.reset();
	for (auto &admission : admissions_)
		admission.end();
	bzero(admissionCookies_, sizeof(admissionCookies_));
	hangHolder_ = nullptr;
	IOLockUnlock(lock_);
	// Channel destruction drains the callbacks before their preallocated work is cancelled.
	if (retireCall_)
		thread_call_cancel_wait(retireCall_);
	if (wakeCall_)
		thread_call_cancel_wait(wakeCall_);
	if (releaseCall_)
		thread_call_cancel_wait(releaseCall_);
	IOLockLock(lock_);
	while (nbinds_)
		unbindOverlaps(binds_[0].va, binds_[0].range, nullptr);
	surfaces_.clear();
	boIndex_.clear();
	while (bos_) {
		auto *bo = bos_;
		bos_ = bo->next;
		queueZombie(bo);
	}
	reap(true);
	const Left left = left_;
	left_ = {};
	IOLockUnlock(lock_);
	dropLeft(left);
	scheduleOrphanRelease();
}

// lock_ held. The BO leaves the index and the published bindings at once, and goes only after its bindings retire and
// every job that may have found it left (retireBatch). The submit gate stays open unless the BO is a resource set's
// member: its free ends the set's generation, waits for the jobs inside the set (switch_gpu_set_reached) and rewrites
// the member list they read.
IOReturn AGXDeviceUserClient::boFree(const TegraGM20BAbiBoFree *in)
{
	const uint64_t start = nowNs();
	Bo **p = findLink(in->handle);
	if (!*p)
		return kIOReturnBadArgument;
	const bool member = switch_gpu_backing_set((*p)->backing) != nullptr;
	if (member) {
		if (!submitGate_.beginMutation(lock_))
			return kIOReturnNotOpen;
		p = findLink(in->handle);
	}
	const IOReturn freed = *p ? freeBo(p) : kIOReturnBadArgument;
	if (member)
		submitGate_.endMutation(lock_);
	if (!freed)
		tgm_stats_bo_free(nowNs() - start);
	return freed;
}

AGXDeviceUserClient::Bo **AGXDeviceUserClient::findLink(uint64_t handle)
{
	Bo **p = &bos_;
	while (*p && (*p)->handle != handle)
		p = &(*p)->next;
	return p;
}

IOReturn AGXDeviceUserClient::freeBo(Bo **p)
{
	Bo *bo = *p;
	bool bound = false;
	for (uint32_t i = 0; i < nbinds_ && !bound; i++)
		bound = binds_[i].bo == bo;
	BindingTable::Snapshot *snapshot = bound ? BindingTable::prepare(nbinds_) : nullptr;
	if (bound && !snapshot)
		return kIOReturnNoMemory;
	*p = bo->next;
	switch_gpu_backing_member_freed(bo->backing);
	boIndex_.erase(bo->handle);
	surfaces_.remove(bo->handle);
	for (uint32_t i = 0; i < nbinds_;) {
		if (binds_[i].bo == bo) {
			unbindOverlaps(binds_[i].va, binds_[i].range, bo);
		} else {
			i++;
		}
	}
	if (snapshot)
		publishBindings(snapshot);
	queueZombie(bo);
	scheduleRetirement();
	return kIOReturnSuccess;
}

// Whole binds of `only` (BoFree) or every bind (close) in the range retire.
void AGXDeviceUserClient::unbindOverlaps(uint64_t va, uint64_t range, Bo *only)
{
	for (uint32_t i = 0; i < nbinds_;) {
		Binding &b = binds_[i];
		if (b.va < va + range && va < b.va + b.range && (!only || b.bo == only)) {
			if (only)
				noteBind(TGM_BIND_FREED, b.va, b.range, b.bo, b.bo_offset,
					 b.retirement->kind);
			retire(b.retirement);
			--nbinds_;
			memmove(&binds_[i], &binds_[i + 1], sizeof(Binding) * (nbinds_ - i));
		} else {
			i++;
		}
	}
	scheduleRetirement();
}

#include "SwitchGpuClientBinds.h"
