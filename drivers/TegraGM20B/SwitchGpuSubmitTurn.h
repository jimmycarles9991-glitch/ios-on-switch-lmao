// SPDX-License-Identifier: MIT
// A kernel copy queue's submissions in order without its lock held across a wait for ring room: the submitter that
// waits lets the lock go and keeps the turn, so the queue's other users (retirement, status, hang checks) go on while
// every later submission sleeps until the turn ends. Sleeping releases the lock atomically, so no wake is lost.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>

class SwitchGpuSubmitTurn
{
public:
	// `lock` held: returns once no submission of the queue is waiting for room
	void enter(IOLock *lock)
	{
		while (waiting_)
			IOLockSleep(lock, this, THREAD_UNINT);
	}

	// `lock` held on entry and on return, not while `wait` runs
	template <typename Wait> int waitForRoom(IOLock *lock, Wait wait)
	{
		waiting_ = true;
		IOLockUnlock(lock);
		const int result = wait();
		IOLockLock(lock);
		waiting_ = false;
		IOLockWakeup(lock, this, false);
		return result;
	}

private:
	bool waiting_ = false;
};
