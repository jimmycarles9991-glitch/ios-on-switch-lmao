// SPDX-License-Identifier: MIT
// Writer promises (TegraGM20BJobABI.h method 34, SwitchGpuWriterPromise.h): made, armed with their holder's bound and
// cancelled here, kept by the job that names them (SubmitJob), released from the CPU by a kill of their channel and by
// the connection's close.
#include "TegraGM20BUserClientInternal.h"

// lock_ held
void AGXDeviceUserClient::dropPromiseSurfaces(uint32_t promise)
{
	const uint32_t offset = tgm_fence_offset(promiseWord(promise));
	for (uint32_t i = 0; i < promiseSurfaceCount_[promise]; ++i) {
		switch_gpu_backing_promise_drop(promiseSurfaces_[promise][i], offset);
		switch_gpu_backing_release(promiseSurfaces_[promise][i]);
		promiseSurfaces_[promise][i] = nullptr;
	}
	promiseSurfaceCount_[promise] = 0;
}

// lock_ held. A word written here reaches a GPU reader parked on it only through the write-combined fence page.
// Abandoned (a kill, a destruction, the close): the promised writes never came, and it is marked so before the word
// moves, so a reader the release lets go already finds it.
void AGXDeviceUserClient::releasePromise(uint32_t promise, bool abandoned)
{
	const uint32_t offset = tgm_fence_offset(promiseWord(promise)),
		       value = promises_[promise].value;
	tgm_promise_unhold(offset, value);
	if (abandoned && promises_[promise].state != SWITCH_GPU_PROMISE_FREE &&
	    int32_t(*promiseWord(promise) - value) < 0)
		tgm_promise_abandon(offset, value);
	if (!switch_gpu_promise_release(&promises_[promise], promiseWord(promise)))
		return;
	__asm__ volatile("dsb sy" ::: "memory");
#if TGM_DEBUG_PROBES
	tgm_promise_cpu_raised(offset, value);
#endif
	tgm_present_promise_released(offset, value);
}

// lock_ held. All: the connection's close. Else the kept ones: those their jobs released are free, and those a kill or
// the destruction of their channel left unreleased are released.
void AGXDeviceUserClient::releasePromises(bool all)
{
	for (uint32_t i = 0; i < TegraGM20BMaxPromises; ++i) {
		auto &slot = promises_[i];
		const bool kept = slot.state == SWITCH_GPU_PROMISE_KEPT;
		const bool released = kept && int32_t(*promiseWord(i) - slot.value) >= 0;
		const bool running =
			(chans_[slot.channel] && !tgm_chan_error(chans_[slot.channel])) ||
			retiringChans_[slot.channel].active;
		if (slot.state == SWITCH_GPU_PROMISE_FREE ||
		    (!all && (!kept || (!released && running))))
			continue;
		if (kept)
			__atomic_sub_fetch(&promisesKept_, 1u, __ATOMIC_RELAXED);
		dropPromiseSurfaces(i);
		releasePromise(i, true);
	}
}

IOReturn AGXDeviceUserClient::writerPromise(const TegraGM20BAbiWriterPromise *in,
					    TegraGM20BAbiWriterPromiseReply *out)
{
	*out = {};
	if (in->op == TegraGM20BPromiseWithdraw && in->count) {
		TegraGM20BAbiSurfaceUse uses[TegraGM20BMaxPromiseUses];
		switch_gpu_backing *refused[TegraGM20BMaxPromiseUses];
		if (in->count > TegraGM20BMaxPromiseUses ||
		    copyin(in->uses_address, uses, in->count * sizeof(uses[0])))
			return kIOReturnBadArgument;
		for (uint32_t i = 0; i < in->count; ++i) {
			const Bo *bo = findBo(uses[i].handle);
			if (!bo || !bo->backing || !bo->surface ||
			    uses[i].access != TegraGM20BSurfaceWrite || uses[i].reserved)
				return kIOReturnBadArgument;
			refused[i] = bo->backing;
		}
		for (uint32_t i = 0; i < in->count; ++i)
			switch_gpu_backing_write_refused(refused[i]);
		return kIOReturnSuccess;
	}
	if (in->op == TegraGM20BPromiseCancel || in->op == TegraGM20BPromiseWithdraw) {
		if (in->promise >= TegraGM20BMaxPromises ||
		    promises_[in->promise].state != SWITCH_GPU_PROMISE_MADE)
			return kIOReturnBadArgument;
		const bool withdrawn = in->op == TegraGM20BPromiseWithdraw;
		// withdrawn, or given up on before this cancel: its surfaces never got the promised frame, past the ref this
		// drops. A withdrawn promise is marked abandoned before its word moves, so a reader it lets go is refused.
		if (withdrawn || tgm_promise_abandoned(tgm_fence_offset(promiseWord(in->promise)),
						       promises_[in->promise].value))
			for (uint32_t i = 0; i < promiseSurfaceCount_[in->promise]; ++i)
				switch_gpu_backing_write_refused(promiseSurfaces_[in->promise][i]);
		dropPromiseSurfaces(in->promise);
		releasePromise(in->promise, withdrawn);
		return kIOReturnSuccess;
	}
	if (in->op != TegraGM20BPromiseMake || !in->count || in->count > TegraGM20BMaxPromiseUses ||
	    !in->channel || in->channel > kMaxChannels)
		return kIOReturnBadArgument;
	TegraGM20BAbiSurfaceUse uses[TegraGM20BMaxPromiseUses];
	if (copyin(in->uses_address, uses, in->count * sizeof(uses[0])))
		return kIOReturnBadArgument;
	if (switch_gpu_promise_engine())
		return kIOReturnNoResources;
	releasePromises(false);
	switch_gpu_promise_claim claim;
	tgm_promise_claim(promiseWord(0), &claim);
	const int made =
		switch_gpu_promise_make(promises_, promiseWord(0), TegraGM20BMaxPromises, &claim);
	if (made < 0)
		return kIOReturnNoResources;
	const uint32_t promise = uint32_t(made);
	if (promiseSurfaceCount_[promise])
		dropPromiseSurfaces(promise);
	const switch_gpu_promise_ref ref = {promiseWord(promise),
					    tgm_fence_offset(promiseWord(promise)),
					    promises_[promise].value};
	IOReturn result = kIOReturnSuccess;
	for (uint32_t i = 0; !result && i < in->count; ++i) {
		const Bo *bo = findBo(uses[i].handle);
		if (!bo || !bo->backing || !bo->surface ||
		    uses[i].access != TegraGM20BSurfaceWrite || uses[i].reserved)
			result = kIOReturnBadArgument;
		else if (switch_gpu_backing_promise_add(bo->backing, &ref))
			result = kIOReturnNoResources;
		else {
			switch_gpu_backing_retain(bo->backing);
			promiseSurfaces_[promise][promiseSurfaceCount_[promise]++] = bo->backing;
		}
	}
	if (result) {
		dropPromiseSurfaces(promise);
		releasePromise(promise, false);
		return result;
	}
	promises_[promise].channel = uint8_t(in->channel - 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_WRITER_PROMISE, 1);
	out->promise = promise;
	return kIOReturnSuccess;
}

// Method 34 Arm. lock_ only while the promises are read: tgm_promise_hold takes tgm_live_lock, which keptPromises too
// takes with lock_ released. The holder's submit worker arms and later keeps on one thread, so no keep of these
// promises comes between the read and the hold.
IOReturn AGXDeviceUserClient::armPromises(const TegraGM20BAbiWriterPromise *in)
{
	uint32_t held[TegraGM20BMaxPromises][2], count = 0;
	if (in->count || (in->channel & ~uint32_t(TegraGM20BPromiseArmFails)) ||
	    in->uses_address > TegraGM20BPromiseArmMaxNs)
		return kIOReturnBadArgument;
	IOLockLock(lock_);
	const bool open = !submitGate_.closed();
	for (uint32_t i = 0; open && i < TegraGM20BMaxPromises; ++i) {
		if (!(in->promise & (1u << i)) || promises_[i].state != SWITCH_GPU_PROMISE_MADE)
			continue;
		held[count][0] = tgm_fence_offset(promiseWord(i));
		held[count++][1] = promises_[i].value;
	}
	IOLockUnlock(lock_);
	if (!open)
		return kIOReturnNotOpen;
	for (uint32_t i = 0; i < count; ++i)
		tgm_promise_hold(held[i][0], held[i][1], in->uses_address,
				 in->channel & TegraGM20BPromiseArmFails);
	return kIOReturnSuccess;
}

// SubmitJob, before its job: the promises it keeps, made by this connection and not kept yet, and what its fence push
// releases. A promise past a holder's bound its command buffer fails at was given up (or is about to be), so the job is
// refused with kIOReturnTimeout and the command buffer fails as Metal fails one held past its bound: the bound and the
// keep are decided once, under the give-up's lock (tgm_promise_unhold).
IOReturn AGXDeviceUserClient::keepPromises(const TegraGM20BAbiJobPromises *promises,
					   switch_gpu_release *releases)
{
	IOLockLock(lock_);
	IOReturn result = kIOReturnSuccess;
	for (uint32_t i = 0; !result && i < promises->count; ++i) {
		const uint32_t promise = promises->promise[i];
		if (promise >= TegraGM20BMaxPromises ||
		    promises_[promise].state != SWITCH_GPU_PROMISE_MADE) {
			result = kIOReturnBadArgument;
			break;
		}
		if (tgm_promise_unhold(tgm_fence_offset(promiseWord(promise)),
				       promises_[promise].value)) {
			result = kIOReturnTimeout;
			break;
		}
		releases[i] = {fence_gpu_ + (TegraGM20BPromiseWord + promise) * 4,
			       promises_[promise].value};
	}
	IOLockUnlock(lock_);
	return result;
}

// SubmitJob refused a job after keepPromises ended the bounds of the promises it was to keep: each one still made has
// its bound again (tgm_promise_rehold), so a reader is not left waiting on a job that never came. lock_ only while the
// promises are read, as armPromises.
void AGXDeviceUserClient::reholdPromises(const TegraGM20BAbiJobPromises *promises)
{
	uint32_t made[TegraGM20BMaxJobPromises][2], count = 0;
	IOLockLock(lock_);
	for (uint32_t i = 0; i < promises->count && i < TegraGM20BMaxJobPromises; ++i) {
		const uint32_t promise = promises->promise[i];
		if (promise >= TegraGM20BMaxPromises ||
		    promises_[promise].state != SWITCH_GPU_PROMISE_MADE)
			continue;
		made[count][0] = tgm_fence_offset(promiseWord(promise));
		made[count++][1] = promises_[promise].value;
	}
	IOLockUnlock(lock_);
	for (uint32_t i = 0; i < count; ++i)
		tgm_promise_rehold(made[i][0], made[i][1]);
}

// Its job (`job`) is submitted: its users from now on follow the job's own writes, a user that already waits for the
// word gets it at the job's end, and a direct present watching it watches the job.
void AGXDeviceUserClient::keptPromises(const TegraGM20BAbiJobPromises *promises, uint32_t slot,
				       const switch_gpu_backing_token &job)
{
	uint32_t kept[TegraGM20BMaxJobPromises][2], count = 0;
	IOLockLock(lock_);
	for (uint32_t i = 0; i < promises->count; ++i) {
		const uint32_t promise = promises->promise[i];
		if (promises_[promise].state != SWITCH_GPU_PROMISE_MADE)
			continue;
		dropPromiseSurfaces(promise);
		switch_gpu_promise_kept(&promises_[promise], slot);
		__atomic_add_fetch(&promisesKept_, 1u, __ATOMIC_RELAXED);
		kept[count][0] = tgm_fence_offset(promiseWord(promise));
		kept[count++][1] = promises_[promise].value;
	}
	IOLockUnlock(lock_);
	for (uint32_t i = 0; i < count; ++i) {
		tgm_present_promise_kept(kept[i][0], kept[i][1], &job);
		tgm_promise_kept(kept[i][0], kept[i][1], chans_[slot], &job);
	}
}
