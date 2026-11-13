// SPDX-License-Identifier: MIT
// Quiet job ends (SwitchGpuQuiet.h): the wake entries a waiter of one of this connection's channels asked for.
#include "TegraGM20BUserClientInternal.h"

// Any context a thread call may be entered from: tgm_chan_need's callers hold the live lock or this connection's lock.
void AGXDeviceUserClient::wakeAsked(void *owner, uint32_t slot)
{
	auto *client = static_cast<AGXDeviceUserClient *>(owner);
	__atomic_fetch_or(&client->wakeSlots_, 1u << slot, __ATOMIC_SEQ_CST);
	thread_call_enter(client->wakeCall_);
}

void AGXDeviceUserClient::wakeWork(thread_call_param_t owner, thread_call_param_t)
{
	static_cast<AGXDeviceUserClient *>(owner)->pushWakes();
}

// In SubmitJob's order: the channel's submit lock, then the submit gate, so no submission of the channel is under way
// and the channel stays as it is. A channel drained or destroyed meanwhile pushed its own entry (tgm_chan_del).
void AGXDeviceUserClient::pushWakes()
{
	uint32_t slots = __atomic_exchange_n(&wakeSlots_, 0u, __ATOMIC_SEQ_CST);
	while (slots) {
		const uint32_t slot = uint32_t(__builtin_ctz(slots));
		slots &= slots - 1;
		IOLockLock(submitLocks_[slot]);
		bool admitted = submitGate_.enterFast();
		if (!admitted) {
			IOLockLock(lock_);
			admitted = submitGate_.enterSlow(lock_);
			IOLockUnlock(lock_);
		}
		if (admitted) {
			if (chans_[slot])
				tgm_chan_quiet_wake(chans_[slot]);
			submitGate_.leaveFast(lock_);
		}
		IOLockUnlock(submitLocks_[slot]);
	}
}
