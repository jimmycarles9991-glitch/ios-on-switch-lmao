// SPDX-License-Identifier: MIT
#include "TegraGM20BUserClientInternal.h"

namespace
{
struct ParkSnap {
	IOMemoryDescriptor *bufferMem;
	uint64_t va, bytes;
	uint32_t token;
	bool hasKernelMap, fullView, coherent, system, readOnly, guard;
};

IOReturn rejectToReturn(SwitchGpuParkReject reject)
{
	switch (reject) {
	case SwitchGpuParkReject::None:
		return kIOReturnSuccess;
	case SwitchGpuParkReject::Unsupported:
		return kIOReturnUnsupported;
	case SwitchGpuParkReject::NotMapped:
		return kIOReturnNotFound;
	case SwitchGpuParkReject::SizeMismatch:
		return kIOReturnBadArgument;
	default:
		return kIOReturnBusy;
	}
}

IOReturn actionToReturn(SwitchGpuParkAction action)
{
	return action == SwitchGpuParkAction::Busy ? kIOReturnBusy : kIOReturnBadArgument;
}

IOReturn backingParkable(switch_gpu_backing *backing)
{
	const int err = switch_gpu_backing_parkable(backing);
	if (err == 0)
		return kIOReturnSuccess;
	return err == -95 ? kIOReturnUnsupported : kIOReturnBusy;
}

IOMemoryDescriptor *makeBoView(const ParkSnap &snap)
{
	if (snap.fullView) {
		snap.bufferMem->retain();
		return snap.bufferMem;
	}
	return IOSubMemoryDescriptor::withSubRange(snap.bufferMem, 0, snap.bytes,
						   kIODirectionInOut);
}

IOOptionBits parkMapOptions(const ParkSnap &snap)
{
	return boMapOptions(snap.coherent, snap.system, snap.fullView, snap.readOnly, snap.bytes);
}
}  // namespace

bool AGXDeviceUserClient::boGpuBusyLocked(const Bo *bo) const
{
	if (!bo || !bo->bindings)
		return false;
	uint32_t liveBinds = 0;
	for (uint32_t i = 0; i < nbinds_; ++i) {
		if (binds_[i].bo != bo)
			continue;
		if (switch_gpu_mapping_pending(binds_[i].retirement->mapping))
			return true;
		++liveBinds;
	}
	if (liveBinds != bo->bindings)
		return true;
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		if (chans_[slot] && !tgm_chan_error(chans_[slot]) &&
		    int32_t(tgm_chan_completed(chans_[slot]) - tgm_chan_submitted(chans_[slot])) <
			    0)
			return true;
	}
	return false;
}

bool AGXDeviceUserClient::parkWaitStableLocked(uint64_t handle)
{
	for (;;) {
		if (submitGate_.closed())
			return false;
		const Bo *bo = findBo(handle);
		if (!bo || !switch_gpu_park_is_transient(bo->parkState))
			return true;
		IOLockSleep(lock_, &parkWait_, THREAD_UNINT);
	}
}

IOReturn AGXDeviceUserClient::vmBindParkedRefusal(const TegraGM20BAbiVmBind *in, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i) {
		const TegraGM20BAbiBindOp &op = in->ops[i];
		if (op.op == TegraGM20BBindMap || op.op == TegraGM20BBindMapVolatile) {
			if (const Bo *bo = findBo(op.handle);
			    bo && switch_gpu_park_blocks_use(bo->parkState))
				return switch_gpu_park_is_transient(bo->parkState)
					       ? kIOReturnBusy
					       : kIOReturnNotReady;
		}
		if (op.range && op.va <= UINT64_MAX - op.range) {
			const uint64_t end = op.va + op.range;
			for (uint32_t j = 0; j < nbinds_; ++j) {
				if (binds_[j].va >= end || op.va >= binds_[j].va + binds_[j].range)
					continue;
				if (binds_[j].bo &&
				    switch_gpu_park_blocks_use(binds_[j].bo->parkState))
					return switch_gpu_park_is_transient(binds_[j].bo->parkState)
						       ? kIOReturnBusy
						       : kIOReturnNotReady;
			}
		}
	}
	return kIOReturnSuccess;
}

bool AGXDeviceUserClient::cpuAcquireParkedRefusal(const TegraGM20BAbiCpuAccess *in)
{
	if (!in || in->reserved || !in->bytes || !in->handle)
		return false;
	IOLockLock(lock_);
	const Bo *bo = findBo(in->handle);
	const bool parked = bo && switch_gpu_park_blocks_use(bo->parkState);
	IOLockUnlock(lock_);
	return parked;
}

IOReturn AGXDeviceUserClient::boFreeMethod(const TegraGM20BAbiBoFree *in)
{
	uint64_t dropVa = 0, dropBytes = 0;
	bool dropGuard = false;
	IOLockLock(lock_);
	if (submitGate_.closed() || !parkWaitStableLocked(in->handle)) {
		IOLockUnlock(lock_);
		return kIOReturnNotOpen;
	}
	Bo *bo = findBo(in->handle);
	if (!bo || bo->freeing) {
		IOLockUnlock(lock_);
		return kIOReturnBadArgument;
	}
	bo->freeing = true;
	if (bo->parkState == SwitchGpuParkState::Parked ||
	    bo->parkState == SwitchGpuParkState::Reclaimed) {
		dropVa = bo->parkedVa;
		dropBytes = bo->bytes;
		dropGuard = bo->parkedGuard;
	}
	const IOReturn ret = boFree(in);
	if (ret != kIOReturnSuccess) {
		if (Bo *still = findBo(in->handle))
			still->freeing = false;
	}
	IOLockUnlock(lock_);
	if (!ret && dropVa)
		switch_iokit_drop_parked_range(task_, dropVa, dropBytes, dropGuard);
	return ret;
}

IOReturn AGXDeviceUserClient::boPark(const TegraGM20BAbiBoPark *in)
{
	if (!in->handle)
		return kIOReturnBadArgument;
	if (current_task() != task_)
		return kIOReturnNotPermitted;
	if (!parkedPoolEnsure())
		return kIOReturnNoMemory;

	Mapping *detachedMap = nullptr;
	IOMemoryDescriptor *oldView = nullptr;
	ParkSnap snap = {};
	snap.guard = (in->flags & TegraGM20BParkGuard) != 0u;

	IOLockLock(lock_);
	if (submitGate_.closed() || !submitGate_.beginMutation(lock_)) {
		IOLockUnlock(lock_);
		return kIOReturnNotOpen;
	}
	Bo *bo = findBo(in->handle);
	IOReturn ret = kIOReturnSuccess;
	if (!bo) {
		ret = kIOReturnBadArgument;
	} else if (const SwitchGpuParkAction action = switch_gpu_park_begin_park(bo->parkState);
		   action != SwitchGpuParkAction::Ok) {
		ret = actionToReturn(action);
	} else {
		Mapping **foundLink = nullptr;
		uint32_t mapCount = 0;
		for (Mapping **p = &maps_; *p; p = &(*p)->next) {
			if ((*p)->token == bo->token) {
				if (!foundLink)
					foundLink = p;
				++mapCount;
			}
		}
		bool leaseActive = false;
		for (const CpuLease *l = cpuLeases_; l && !leaseActive; l = l->next)
			leaseActive = l->backing == bo->backing;
		const SwitchGpuParkEligibility elig = {
			bo->buffer.memory != nullptr,
			bo->wired != nullptr,
			bo->pool_va != nullptr,
			bo->chunked || bo->smmuCompressible,
			(bo->flags & TegraGM20BBoContiguous) != 0u,
			(bo->flags & TegraGM20BBoControl) != 0u,
			bo->nativeView != nullptr,
			bo->surface != nullptr,
			bo->backing != nullptr,
			false,
			bo->freeing,
			mapCount,
			foundLink != nullptr && (*foundLink)->bytes == bo->bytes,
			bo->mappingRefs != 0u,
			leaseActive,
			boGpuBusyLocked(bo),
		};
		ret = rejectToReturn(switch_gpu_park_check_eligible(elig));
		if (!ret && !bo->view)
			ret = kIOReturnNotReady;
		if (!ret)
			ret = backingParkable(bo->backing);
		if (!ret) {
			bo->parkState = SwitchGpuParkState::Parking;
			bo->parkedBacking = bo->backing;
			bo->backing = nullptr;
			detachedMap = *foundLink;
			*foundLink = detachedMap->next;
			detachedMap->next = nullptr;
			oldView = bo->view;
			bo->view = nullptr;
			snap = {bo->buffer.memory,
				detachedMap->address,
				bo->bytes,
				bo->token,
				bo->bufferKernelMap,
				bo->backing_pages == bo->npages,
				(bo->flags & TegraGM20BBoCoherent) != 0u,
				bo->system,
				detachedMap->readOnly,
				snap.guard};
		}
	}
	submitGate_.endMutation(lock_);
	IOLockUnlock(lock_);
	if (ret != kIOReturnSuccess)
		return ret;

	bool staleDropped = false;
	const kern_return_t kr = switch_iokit_park_replace(task_, detachedMap->map, snap.bufferMem,
							   oldView, snap.hasKernelMap, snap.va,
							   snap.bytes, snap.guard, &staleDropped);
	if (kr != KERN_SUCCESS) {
		IOLockLock(lock_);
		const bool open = !submitGate_.closed() && submitGate_.beginMutation(lock_);
		if (Bo *still = findBo(in->handle)) {
			still->backing = still->parkedBacking;
			still->parkedBacking = nullptr;
			still->view = oldView;
			still->parkState = SwitchGpuParkState::Live;
			oldView = nullptr;
		}
		if (open && !staleDropped) {
			detachedMap->next = maps_;
			maps_ = detachedMap;
			detachedMap = nullptr;
		}
		if (open)
			submitGate_.endMutation(lock_);
		IOLockWakeup(lock_, &parkWait_, false);
		IOLockUnlock(lock_);
		if (oldView)
			oldView->release();
		if (detachedMap) {
			if (!staleDropped) {
				if (detachedMap->overwrite) {
					switch_iokit_unmap_overwrite(task_, detachedMap->map,
								     detachedMap->address,
								     detachedMap->bytes);
				} else {
					detachedMap->map->unmap();
					detachedMap->map->release();
				}
			}
			IODelete(detachedMap, Mapping, 1);
		}
		if (!open)
			return kIOReturnNotOpen;
		return kr == KERN_INVALID_ADDRESS      ? kIOReturnNotFound
		       : kr == KERN_PROTECTION_FAILURE ? kIOReturnNotPermitted
						       : kIOReturnBusy;
	}

	IODelete(detachedMap, Mapping, 1);
	oldView->release();

	bool wakeReaper = false;
	IOLockLock(lock_);
	if (submitGate_.closed()) {
		if (Bo *still = findBo(in->handle)) {
			still->backing = still->parkedBacking;
			still->parkedBacking = nullptr;
			still->parkState = SwitchGpuParkState::Live;
		}
		IOLockWakeup(lock_, &parkWait_, false);
		IOLockUnlock(lock_);
		switch_iokit_drop_parked_range(task_, snap.va, snap.bytes, snap.guard);
		return kIOReturnNotOpen;
	}
	if (Bo *still = findBo(in->handle)) {
		still->parkState = SwitchGpuParkState::Parked;
		still->parkedVa = snap.va;
		still->parkedGuard = snap.guard;
		still->parkedReadOnly = snap.readOnly;
		still->parkedNode = {nullptr,      nullptr,     this, still->handle,
				     still->bytes, snap.system, false};
		wakeReaper = parkedPoolPush(&still->parkedNode, true);
	}
	IOLockWakeup(lock_, &parkWait_, false);
	IOLockUnlock(lock_);
	if (wakeReaper)
		kickSpareWork();
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boUnpark(const TegraGM20BAbiBoUnpark *in,
				       TegraGM20BAbiBoUnparkReply *out)
{
	if (!in->handle)
		return kIOReturnBadArgument;
	if (current_task() != task_)
		return kIOReturnNotPermitted;
	*out = {};

	ParkSnap snap = {};
	IOLockLock(lock_);
	if (submitGate_.closed()) {
		IOLockUnlock(lock_);
		return kIOReturnNotOpen;
	}
	Bo *bo = findBo(in->handle);
	if (!bo || bo->freeing) {
		IOLockUnlock(lock_);
		return kIOReturnBadArgument;
	}
	if (bo->parkState == SwitchGpuParkState::Reclaiming) {
		if (!parkWaitStableLocked(in->handle) || submitGate_.closed()) {
			IOLockUnlock(lock_);
			return kIOReturnNotOpen;
		}
		bo = findBo(in->handle);
		if (!bo || bo->freeing) {
			IOLockUnlock(lock_);
			return kIOReturnBadArgument;
		}
	}
	const SwitchGpuParkAction action = switch_gpu_park_begin_unpark(bo->parkState);
	if (action == SwitchGpuParkAction::Reclaimed) {
		*out = {bo->parkedVa, bo->bytes, TegraGM20BUnparkReclaimed, 0};
		IOLockUnlock(lock_);
#if TGM_STATS
		parkedStatUnpark(true);
#endif
		return kIOReturnSuccess;
	}
	if (action != SwitchGpuParkAction::Ok) {
		IOLockUnlock(lock_);
		return actionToReturn(action);
	}
	bo->parkState = SwitchGpuParkState::Unparking;
	parkUnlinkLocked(bo, false);
	snap = {bo->buffer.memory,
		bo->parkedVa,
		bo->bytes,
		bo->token,
		bo->bufferKernelMap,
		bo->backing_pages == bo->npages,
		(bo->flags & TegraGM20BBoCoherent) != 0u,
		bo->system,
		bo->parkedReadOnly,
		bo->parkedGuard};
	snap.bufferMem->retain();
	IOLockUnlock(lock_);

	Mapping *mapping = IONew(Mapping, 1);
	IOMemoryDescriptor *view = makeBoView(snap);
	IOMemoryMap *newMap = nullptr;
	const IOReturn mapRet =
		(mapping && view)
			? switch_iokit_unpark_range(view, task_, snap.va, snap.bytes,
						    parkMapOptions(snap), snap.guard, &newMap)
			: kIOReturnNoMemory;
	snap.bufferMem->release();

	IOLockLock(lock_);
	if (mapRet == kIOReturnSuccess && !submitGate_.closed() &&
	    submitGate_.beginMutation(lock_)) {
		if (Bo *still = findBo(in->handle)) {
			still->view = view;
			still->backing = still->parkedBacking;
			still->parkedBacking = nullptr;
			still->parkState = SwitchGpuParkState::Live;
			still->parkedVa = 0;
			still->parkedGuard = false;
			still->parkedReadOnly = false;
			*mapping = {maps_,      newMap, snap.token,   snap.va,
				    snap.bytes, true,   snap.readOnly};
			maps_ = mapping;
			view = nullptr;
			mapping = nullptr;
			newMap = nullptr;
		}
		submitGate_.endMutation(lock_);
		IOLockWakeup(lock_, &parkWait_, false);
		IOLockUnlock(lock_);
		if (newMap)
			switch_iokit_unmap_overwrite(task_, newMap, snap.va, snap.bytes);
		if (view)
			view->release();
		if (mapping)
			IODelete(mapping, Mapping, 1);
#if TGM_STATS
		parkedStatUnpark(false);
#endif
		*out = {snap.va, snap.bytes, TegraGM20BUnparkResident, 0};
		return kIOReturnSuccess;
	}
	const bool closed = submitGate_.closed();
	bool wakeReaper = false;
	if (Bo *still = findBo(in->handle)) {
		still->parkState = SwitchGpuParkState::Parked;
		if (!closed)
			wakeReaper = parkedPoolPush(&still->parkedNode, false);
	}
	IOLockWakeup(lock_, &parkWait_, false);
	IOLockUnlock(lock_);
	if (wakeReaper)
		kickSpareWork();
	if (newMap)
		switch_iokit_unmap_overwrite(task_, newMap, snap.va, snap.bytes);
	if (view)
		view->release();
	if (mapping)
		IODelete(mapping, Mapping, 1);
	return closed ? kIOReturnNotOpen : mapRet;
}
