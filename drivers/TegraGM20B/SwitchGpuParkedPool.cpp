// SPDX-License-Identifier: MIT
#include "TegraGM20BUserClientInternal.h"

namespace
{
struct ParkedPool {
	IOLock *lock;
	SwitchGpuParkedLru lru;
	uint64_t demandBytes;
#if TGM_STATS
	uint64_t parked, unparkResident, unparkReclaimed, reclaimedCached, reclaimedSystem;
#endif
};

ParkedPool *gParked;

ParkedPool *parkedPoolExisting()
{
	return __atomic_load_n(&gParked, __ATOMIC_ACQUIRE);
}

ParkedPool *parkedPool()
{
	if (ParkedPool *p = parkedPoolExisting())
		return p;
	ParkedPool *n = IONew(ParkedPool, 1);
	if (!n)
		return nullptr;
	bzero(n, sizeof(*n));
	n->lock = IOLockAlloc();
	if (!n->lock) {
		IODelete(n, ParkedPool, 1);
		return nullptr;
	}
	ParkedPool *none = nullptr;
	if (!__atomic_compare_exchange_n(&gParked, &none, n, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(n->lock);
		IODelete(n, ParkedPool, 1);
		return none;
	}
	return n;
}

void releaseReclaimedOne(ReclaimedBacking *rb)
{
	const bool system = rb->system;
	switch_gpu_cached_mapping_drop(&rb->cachedMapping);
	switch_gpu_cached_buffer_free(&rb->buffer);
	if (rb->pas)
		IODelete(rb->pas, uint64_t, rb->backingPages);
	switch_gpu_backing_release(rb->backing);
	IODelete(rb, ReclaimedBacking, 1);
#if TGM_STATS
	if (ParkedPool *p = parkedPoolExisting()) {
		IOLockLock(p->lock);
		if (system)
			++p->reclaimedSystem;
		else
			++p->reclaimedCached;
		IOLockUnlock(p->lock);
	}
#else
	(void)system;
#endif
}
}  // namespace

namespace tgm_uc
{
bool parkedPoolEnsure()
{
	return parkedPool() != nullptr;
}

bool parkedPoolPush(SwitchGpuParkedNode *node, bool countStat)
{
	ParkedPool *p = parkedPoolExisting();
	if (!p || !node)
		return false;
	IOLockLock(p->lock);
	p->lru.pushBack(node);
#if TGM_STATS
	if (countStat)
		++p->parked;
#else
	(void)countStat;
#endif
	const uint64_t total = p->lru.totalBytes();
	const uint64_t cached = p->lru.cachedBytes();
	const uint64_t demand = p->demandBytes;
	IOLockUnlock(p->lock);
	return switch_gpu_park_reclaim_pick(total, cached, sparesPressure(), demand,
					    sparesHasRoom()) != SwitchGpuParkReclaimPick::None;
}

bool reclaimParkedStep(bool pressure, bool room)
{
	ParkedPool *p = parkedPoolExisting();
	if (!p)
		return false;
	AGXDeviceUserClient *client = nullptr;
	uint64_t handle = 0;
	IOLockLock(p->lock);
	const SwitchGpuParkReclaimPick pick = switch_gpu_park_reclaim_pick(
		p->lru.totalBytes(), p->lru.cachedBytes(), pressure, p->demandBytes, room);
	SwitchGpuParkedNode *node = pick == SwitchGpuParkReclaimPick::Head ? p->lru.peekHead()
				    : pick == SwitchGpuParkReclaimPick::OldestCached
					    ? p->lru.peekOldestCached()
					    : nullptr;
	if (node) {
		client = static_cast<AGXDeviceUserClient *>(node->client);
		handle = node->handle;
		client->retain();
	} else if (pick == SwitchGpuParkReclaimPick::None && !p->lru.cachedCount()) {
		p->demandBytes = 0;
	}
	IOLockUnlock(p->lock);
	if (!client)
		return false;
	const bool progressed = client->reclaimParkedBo(handle);
	client->release();
	return progressed;
}

bool parkedNotifyDemand(uint64_t bytes)
{
	ParkedPool *p = parkedPoolExisting();
	if (!p || !bytes)
		return false;
	IOLockLock(p->lock);
	const bool hasCached = p->lru.cachedCount() != 0u;
	if (hasCached)
		p->demandBytes = switch_gpu_park_clamp_demand(p->demandBytes, bytes);
	IOLockUnlock(p->lock);
	return hasCached;
}

bool parkedHasAny()
{
	ParkedPool *p = parkedPoolExisting();
	if (!p)
		return false;
	IOLockLock(p->lock);
	const bool any = !p->lru.empty();
	IOLockUnlock(p->lock);
	return any;
}

#if TGM_STATS
void parkedStatUnpark(bool reclaimed)
{
	if (ParkedPool *p = parkedPoolExisting()) {
		IOLockLock(p->lock);
		if (reclaimed)
			++p->unparkReclaimed;
		else
			++p->unparkResident;
		IOLockUnlock(p->lock);
	}
}

void parkedStatsTake(uint64_t *parked, uint64_t *unparkResident, uint64_t *unparkReclaimed,
		     uint64_t *reclaimedCached, uint64_t *reclaimedSystem)
{
	*parked = *unparkResident = *unparkReclaimed = *reclaimedCached = *reclaimedSystem = 0;
	ParkedPool *p = parkedPoolExisting();
	if (!p)
		return;
	IOLockLock(p->lock);
	*parked = p->parked;
	*unparkResident = p->unparkResident;
	*unparkReclaimed = p->unparkReclaimed;
	*reclaimedCached = p->reclaimedCached;
	*reclaimedSystem = p->reclaimedSystem;
	p->parked = p->unparkResident = p->unparkReclaimed = p->reclaimedCached =
		p->reclaimedSystem = 0;
	IOLockUnlock(p->lock);
}
#endif
}  // namespace tgm_uc

void AGXDeviceUserClient::parkUnlinkLocked(Bo *bo, bool consumeDemand)
{
	if (!bo || !bo->parkedNode.linked)
		return;
	if (ParkedPool *p = parkedPoolExisting()) {
		IOLockLock(p->lock);
		if (bo->parkedNode.linked) {
			p->lru.remove(&bo->parkedNode);
			if (consumeDemand && !bo->system)
				p->demandBytes =
					switch_gpu_park_consume_demand(p->demandBytes, bo->bytes);
		}
		IOLockUnlock(p->lock);
	}
}

void AGXDeviceUserClient::parkUnlinkAllLocked()
{
	for (Bo *bo = bos_; bo; bo = bo->next) {
		parkUnlinkLocked(bo, false);
		parkSettledLocked(bo);
	}
}

// lock_ held: `bo` has left Parking, Unparking or Reclaiming (or its owner handed it on): its waiters look again.
void AGXDeviceUserClient::parkSettledLocked(Bo *bo)
{
	bo->parkOwner = nullptr;
	switch_gpu_wakeup_inheritor(lock_, &bo->parkOwner);
}

void AGXDeviceUserClient::drainParkedRanges()
{
	for (;;) {
		uint64_t va = 0;
		uint64_t bytes = 0;
		bool guard = false;
		IOLockLock(lock_);
		for (Bo *bo = bos_; bo; bo = bo->next) {
			if (bo->parkedNode.linked)
				parkUnlinkLocked(bo, false);
			if (bo->parkedVa) {
				va = bo->parkedVa;
				bytes = bo->bytes;
				guard = bo->parkedGuard;
				bo->parkedVa = 0;
				break;
			}
		}
		IOLockUnlock(lock_);
		if (!va)
			return;
		switch_iokit_drop_parked_range(task_, va, bytes, guard);
	}
}

ReclaimedBacking *AGXDeviceUserClient::detachReclaimedLocked(Bo *bo)
{
	ReclaimedBacking *rb = bo->reclaimPayload;
	bo->reclaimPayload = nullptr;
	*rb = {nullptr,   bo->buffer,        bo->pas,           bo->backing_pages,
	       bo->bytes, bo->cachedMapping, bo->parkedBacking, bo->system};
	bo->buffer = {};
	bo->pas = nullptr;
	bo->cachedMapping = nullptr;
	bo->parkedBacking = nullptr;
	bo->mem = nullptr;
	bo->parkState = SwitchGpuParkState::Reclaimed;
	if (!bo->parkReclaimed) {
		bo->parkReclaimed = true;
		if (SWITCH_GPU_OWNER_COUNTS && bo->handle)
			ownerMemory_.change(bo->ownerKind, bo->bytes, false);
#if TGM_STATS
		if (bo->system && bo->handle) {
			__atomic_sub_fetch(&gSystemCoherent, 1, __ATOMIC_RELAXED);
			__atomic_sub_fetch(&gSystemCoherentBytes, bo->bytes, __ATOMIC_RELAXED);
		}
#endif
	}
	parkSettledLocked(bo);
	return rb;
}

void AGXDeviceUserClient::parkFinishReclaimLocked(Bo *bo)
{
	if (switch_gpu_park_finish_reclaim(bo->parkState) != SwitchGpuParkAction::Ok ||
	    bo->bindings || !bo->reclaimPayload || bo->reclaimDeferred)
		return;
	bo->reclaimDeferred = true;
	bo->reclaimSeq = ++reclaimSeq_;
	++reclaimDeferredCount_;
	parkSettledLocked(bo);
	if (!__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE) && releaseCall_)
		thread_call_enter(releaseCall_);
}

void AGXDeviceUserClient::drainDeferredReclaims(uint64_t upToSeq)
{
	ReclaimedBacking *done = nullptr;
	bool again = false;
	IOLockLock(lock_);
	if (!submitGate_.closed()) {
		for (Bo *bo = bos_; bo; bo = bo->next) {
			if (!bo->reclaimDeferred || bo->reclaimSeq > upToSeq)
				continue;
			bo->reclaimDeferred = false;
			--reclaimDeferredCount_;
			if (bo->parkState == SwitchGpuParkState::Reclaiming && !bo->bindings &&
			    bo->reclaimPayload) {
				ReclaimedBacking *rb = detachReclaimedLocked(bo);
				rb->next = done;
				done = rb;
			}
		}
		again = reclaimDeferredCount_ != 0u &&
			!__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE) &&
			releaseCall_ != nullptr;
	}
	IOLockUnlock(lock_);
	if (done)
		releaseReclaimedList(done);
	if (again)
		thread_call_enter(releaseCall_);
}

void AGXDeviceUserClient::releaseReclaimedList(ReclaimedBacking *list)
{
	while (list) {
		ReclaimedBacking *next = list->next;
		releaseReclaimedOne(list);
		list = next;
	}
}

bool AGXDeviceUserClient::reclaimParkedBo(uint64_t handle)
{
	ReclaimedBacking *rb = IONew(ReclaimedBacking, 1);
	if (!rb)
		return false;

	IOLockLock(lock_);
	if (submitGate_.closed()) {
		IOLockUnlock(lock_);
		IODelete(rb, ReclaimedBacking, 1);
		return false;
	}
	Bo *bo = findBo(handle);
	if (!bo || bo->parkState != SwitchGpuParkState::Parked) {
		IOLockUnlock(lock_);
		IODelete(rb, ReclaimedBacking, 1);
		return true;
	}
	if (bo->freeing) {
		IOLockUnlock(lock_);
		IODelete(rb, ReclaimedBacking, 1);
		return false;
	}
	const auto hasBinds = [this](const Bo *target) {
		for (uint32_t i = 0; i < nbinds_; ++i)
			if (binds_[i].bo == target)
				return true;
		return false;
	};
	bool bound = hasBinds(bo);
	bool mutating = false;
	if (bound) {
		if (!submitGate_.beginMutation(lock_)) {
			IOLockUnlock(lock_);
			IODelete(rb, ReclaimedBacking, 1);
			return false;
		}
		mutating = true;
		bo = findBo(handle);
		if (!bo || bo->parkState != SwitchGpuParkState::Parked || bo->freeing) {
			const bool raced = !bo || bo->parkState != SwitchGpuParkState::Parked;
			submitGate_.endMutation(lock_);
			IOLockUnlock(lock_);
			IODelete(rb, ReclaimedBacking, 1);
			return raced;
		}
		bound = hasBinds(bo);
	}
	BindingTable::Snapshot *snapshot = bound ? BindingTable::prepare(nbinds_) : nullptr;
	if (bound && !snapshot) {
		if (mutating)
			submitGate_.endMutation(lock_);
		IOLockUnlock(lock_);
		IODelete(rb, ReclaimedBacking, 1);
		return false;
	}
	bo->parkState = SwitchGpuParkState::Reclaiming;
	bo->parkOwner = switch_gpu_current_thread();
	parkUnlinkLocked(bo, true);
	bo->reclaimPayload = rb;
	rb = nullptr;

	if (bound) {
		for (uint32_t i = 0; i < nbinds_;) {
			if (binds_[i].bo == bo)
				unbindOverlaps(binds_[i].va, binds_[i].range, bo);
			else
				++i;
		}
		publishBindings(snapshot);
	}
	if (bo->bindings) {
		RetiredBinding *idle = nullptr;
		RetiredBinding **at = &retired_;
		while (*at) {
			RetiredBinding *record = *at;
			if (record->bo == bo && bindingIdle(record)) {
				*at = record->next;
				--retiredCount_;
				if (retireRemaining_ > retiredCount_)
					retireRemaining_ = retiredCount_;
				record->next = idle;
				idle = record;
			} else {
				at = &record->next;
			}
		}
		retiredTail_ = at;
		if (idle) {
			IOLockLock(unmapLock_);
			for (RetiredBinding *record = idle; record; record = record->next)
				switch_gpu_vm_unmap(vm_, &record->mapping);
			IOLockUnlock(unmapLock_);
			while (idle) {
				RetiredBinding *next = idle->next;
				noteBind(TGM_BIND_GONE, idle->va, idle->range, idle->bo, 0,
					 idle->kind);
				--bo->bindings;
				IODelete(idle, RetiredBinding, 1);
				idle = next;
			}
		}
	}
	if (mutating)
		submitGate_.endMutation(lock_);

	if (!bo->bindings) {
		if (!bound) {
			ReclaimedBacking *done = detachReclaimedLocked(bo);
			IOLockUnlock(lock_);
			releaseReclaimedOne(done);
			return true;
		}
		// Keep bo->buffer intact until concurrent submitters leave pushView().
		IOLockUnlock(lock_);
		if (passSubmitters(false)) {
			ReclaimedBacking *done = nullptr;
			IOLockLock(lock_);
			if (!submitGate_.closed()) {
				if (Bo *still = findBo(handle);
				    still && still->parkState == SwitchGpuParkState::Reclaiming &&
				    !still->bindings && still->reclaimPayload)
					done = detachReclaimedLocked(still);
			}
			IOLockUnlock(lock_);
			if (done)
				releaseReclaimedOne(done);
			return true;
		}
		IOLockLock(lock_);
		if (submitGate_.closed()) {
			IOLockUnlock(lock_);
			return false;
		}
		if (!__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE) && releaseCall_) {
			if (Bo *still = findBo(handle))
				parkFinishReclaimLocked(still);
			IOLockUnlock(lock_);
			return true;
		}
		IOLockUnlock(lock_);
		passSubmitters(true);
		ReclaimedBacking *done = nullptr;
		IOLockLock(lock_);
		if (!submitGate_.closed()) {
			if (Bo *still = findBo(handle);
			    still && still->parkState == SwitchGpuParkState::Reclaiming &&
			    !still->bindings && still->reclaimPayload)
				done = detachReclaimedLocked(still);
		}
		IOLockUnlock(lock_);
		if (done)
			releaseReclaimedOne(done);
		return true;
	}
	parkSettledLocked(bo);
	scheduleRetirement();
	IOLockUnlock(lock_);
	return true;
}
