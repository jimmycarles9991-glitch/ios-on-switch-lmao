// SPDX-License-Identifier: MIT
// A kernel copy queue's submissions in order without its lock held across a wait for ring room: the submitter that
// waits lets the lock go and keeps the turn, so the queue's other users (retirement, status, hang checks) go on while
// every later submission sleeps until the turn ends. Sleeping releases the lock atomically, so no wake is lost.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include "SwitchGpuInheritorSleep.h"

class SwitchGpuSubmitTurn
{
public:
	// `lock` held: whether a submission holds the turn, waiting for room
	bool waiting() const
	{
		return waiting_;
	}

	// `lock` held: returns once no submission of the queue is waiting for room
	void enter(IOLock *lock)
	{
		while (waiting_)
			switch_gpu_sleep_inheritor(lock, this, owner_);
	}

	// `lock` held on entry and on return, not while `wait` runs
	template <typename Wait> int waitForRoom(IOLock *lock, Wait wait)
	{
		waiting_ = true;
		owner_ = switch_gpu_current_thread();
		IOLockUnlock(lock);
		const int result = wait();
		IOLockLock(lock);
		waiting_ = false;
		owner_ = nullptr;
		switch_gpu_wakeup_inheritor(lock, this);
		return result;
	}

private:
	bool waiting_ = false;
	SwitchGpuThreadRef owner_ = nullptr;
};
