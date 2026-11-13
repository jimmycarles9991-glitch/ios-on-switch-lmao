// SPDX-License-Identifier: MIT
// Writer promises (TegraGM20BJobABI.h method 34, SwitchGpuWriterPromise.h): made and cancelled here, kept by the job
// that names them (SubmitJob), released from the CPU by a kill of their channel and by the connection's close.
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
	if (abandoned && promises_[promise].state != SWITCH_GPU_PROMISE_FREE &&
	    int32_t(*promiseWord(promise) - value) < 0)
		tgm_promise_abandon(offset, value);
	if (!switch_gpu_promise_release(&promises_[promise], promiseWord(promise)))
		return;
	__asm__ volatile("dsb sy" ::: "memory");
#if TGM_STATS
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
		const bool running = chans_[slot.channel] && !tgm_chan_error(chans_[slot.channel]);
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
	if (in->op == TegraGM20BPromiseCancel) {
		if (in->promise >= TegraGM20BMaxPromises ||
		    promises_[in->promise].state != SWITCH_GPU_PROMISE_MADE)
			return kIOReturnBadArgument;
		dropPromiseSurfaces(in->promise);
		releasePromise(in->promise, false);
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

// SubmitJob, before its job: the promises it keeps, made by this connection and not kept yet, and what its fence push
// releases.
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
		releases[i] = {fence_gpu_ + (TegraGM20BPromiseWord + promise) * 4,
			       promises_[promise].value};
	}
	IOLockUnlock(lock_);
	return result;
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
