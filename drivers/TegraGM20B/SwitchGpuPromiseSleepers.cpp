// SPDX-License-Identifier: MIT
// CPU threads asleep on a backing behind a pending writer promise (SwitchGpuWriterPromise.h). A cancel, a keep or a
// close drops the backing's ref and wakes them under its lock; a promise given up on (SwitchGpuPromiseGiveUp.h) names
// only its fence word, so the backings with sleepers are listed here for its wake. The list's lock is taken inside a
// backing's and never around one: the wake copies a few entries out, then takes each backing's lock.
#include "SwitchGpuBackingInternal.h"
#include "TegraGM20BGpu.h"

extern "C" void proc_selfname(char *buf, int size);

namespace
{
IOLock *gLock;
switch_gpu_backing *gHead;
uint32_t gGeneration;

IOLock *sleepersLock()
{
	IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	if (!__atomic_compare_exchange_n(&gLock, &lock, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return lock;
	}
	return made;
}

// sleepersLock held
void link(switch_gpu_backing *backing)
{
	if (backing->promise_sleepers++)
		return;
	backing->promise_sleep_prev = nullptr;
	backing->promise_sleep_next = gHead;
	if (gHead)
		gHead->promise_sleep_prev = backing;
	gHead = backing;
}

// sleepersLock held
void unlink(switch_gpu_backing *backing)
{
	if (--backing->promise_sleepers)
		return;
	if (backing->promise_sleep_prev)
		backing->promise_sleep_prev->promise_sleep_next = backing->promise_sleep_next;
	else
		gHead = backing->promise_sleep_next;
	if (backing->promise_sleep_next)
		backing->promise_sleep_next->promise_sleep_prev = backing->promise_sleep_prev;
	backing->promise_sleep_next = backing->promise_sleep_prev = nullptr;
}

bool listed(const switch_gpu_backing_token *pending, int count, uint32_t offset, uint32_t value)
{
	for (int i = 0; i < count; ++i)
		if (pending[i].queue == offset && uint32_t(pending[i].value) == value)
			return true;
	return false;
}

// The backing's lock held: the parked promises no longer pending are left. At most SWITCH_GPU_BACKING_PROMISES are
// pending at once, so the list then has room for every one of them.
void leaveSettled(switch_gpu_promise_parked *parked, const switch_gpu_backing_token *pending,
		  int count)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < parked->count; ++i) {
		if (!listed(pending, count, parked->offset[i], parked->value[i])) {
			tgm_promise_leave(parked->offset[i], parked->value[i], 0);
			continue;
		}
		parked->offset[kept] = parked->offset[i];
		parked->value[kept++] = parked->value[i];
	}
	parked->count = kept;
}

// The backing's lock held
void park(switch_gpu_backing *backing, switch_gpu_promise_parked *parked, const char *who)
{
	switch_gpu_backing_token pending[SWITCH_GPU_BACKING_PROMISES];
	const int count = switch_gpu_backing_promises_locked(backing, 0, 0, pending,
							     SWITCH_GPU_BACKING_PROMISES);
	char name[17] = {};
	for (int i = 0; i < count; ++i) {
		const uint32_t offset = pending[i].queue, value = uint32_t(pending[i].value);
		bool known = false;
		for (uint32_t j = 0; !known && j < parked->count; ++j)
			known = parked->offset[j] == offset && parked->value[j] == value;
		if (known)
			continue;
		if (parked->count == SWITCH_GPU_PROMISE_PARKED)
			leaveSettled(parked, pending, count);
		if (!name[0])
			proc_selfname(name, sizeof(name));
		if (tgm_promise_park(offset, value, name, who, nullptr, 0) != 1)
			continue;
		parked->offset[parked->count] = offset;
		parked->value[parked->count++] = value;
	}
}
}  // namespace

// The backing's lock held: the caller asleep until it is woken, listed while a promise is pending. A promise given up
// on between the caller's check and the listing is seen by the check again here, after the listing: the wake's copy,
// if it came first, could not have found this sleeper. The park is once a promise for the reader's whole wait: a
// wake for another reason does not restart its second.
void switch_gpu_backing_sleep_promised_locked(switch_gpu_backing *backing,
					      switch_gpu_promise_parked *parked, const char *who)
{
	IOLock *lock =
		switch_gpu_backing_promise_pending_locked(backing) ? sleepersLock() : nullptr;
	if (!lock) {
		switch_gpu_backing_sleep_cpu_locked(backing);
		return;
	}
	if (who)
		park(backing, parked, who);
	IOLockLock(lock);
	link(backing);
	IOLockUnlock(lock);
	if (switch_gpu_backing_promise_pending_locked(backing))
		switch_gpu_backing_sleep_cpu_locked(backing);
	IOLockLock(lock);
	unlink(backing);
	IOLockUnlock(lock);
}

void switch_gpu_promise_parked_leave(switch_gpu_promise_parked *parked)
{
	for (uint32_t i = 0; i < parked->count; ++i)
		tgm_promise_leave(parked->offset[i], parked->value[i], 0);
	parked->count = 0;
}

extern "C" void switch_gpu_backing_promise_sleepers_wake(void)
{
	IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
	if (!lock)
		return;
	IOLockLock(lock);
	const uint32_t generation = ++gGeneration;
	IOLockUnlock(lock);
	for (;;) {
		switch_gpu_backing *batch[16];
		uint32_t count = 0;
		IOLockLock(lock);
		for (switch_gpu_backing *at = gHead; at && count < 16;
		     at = at->promise_sleep_next) {
			if (at->promise_woken == generation)
				continue;
			at->promise_woken = generation;
			switch_gpu_backing_retain(at);
			batch[count++] = at;
		}
		IOLockUnlock(lock);
		if (!count)
			return;
		for (uint32_t i = 0; i < count; ++i) {
			IOLockLock(batch[i]->lock);
			IOLockWakeup(batch[i]->lock, &batch[i]->cpu_writers, false);
			IOLockUnlock(batch[i]->lock);
			switch_gpu_backing_release(batch[i]);
		}
	}
}
