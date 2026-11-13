// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <stdint.h>

// Every operation but enterFast and leaveFast is called with the connection lock held. A queued mutation prevents new
// submissions from extending its drain; sleeping releases that lock so other channels, status reads and CPU unlocks can
// progress. enterFast admits a submission without the lock while no mutation is queued or running; enterSlow is its
// wait when one is, and leaveFast takes the lock only to wake a mutation waiting for the last one.
class SwitchGpuSubmitGate
{
public:
	bool enter(IOLock *lock)
	{
		while (!closed_ && (mutations_ || mutating_))
			IOLockSleep(lock, this, THREAD_UNINT);
		if (closed_)
			return false;
		++active_;
		return true;
	}

	void leave(IOLock *lock)
	{
		if (!--active_)
			IOLockWakeup(lock, this, false);
	}

	bool enterFast()
	{
		__atomic_add_fetch(&fast_, 1u, __ATOMIC_SEQ_CST);
		if (!__atomic_load_n(&blocked_, __ATOMIC_SEQ_CST))
			return true;
		__atomic_sub_fetch(&fast_, 1u, __ATOMIC_SEQ_CST);
		return false;
	}

	bool enterSlow(IOLock *lock)
	{
		// enterFast's refused count was undone with no wake: a mutation that saw it as the last one is woken here
		IOLockWakeup(lock, this, false);
		while (!closed_ && (mutations_ || mutating_))
			IOLockSleep(lock, this, THREAD_UNINT);
		if (closed_)
			return false;
		__atomic_add_fetch(&fast_, 1u, __ATOMIC_SEQ_CST);
		return true;
	}

	void leaveFast(IOLock *lock)
	{
		if (__atomic_sub_fetch(&fast_, 1u, __ATOMIC_SEQ_CST) ||
		    !__atomic_load_n(&blocked_, __ATOMIC_SEQ_CST))
			return;
		IOLockLock(lock);
		IOLockWakeup(lock, this, false);
		IOLockUnlock(lock);
	}

	bool beginMutation(IOLock *lock)
	{
		++mutations_;
		block();
		while (!closed_ && (active_ || mutating_ || fast()))
			IOLockSleep(lock, this, THREAD_UNINT);
		--mutations_;
		if (closed_) {
			IOLockWakeup(lock, this, false);
			return false;
		}
		mutating_ = true;
		return true;
	}

	void endMutation(IOLock *lock)
	{
		mutating_ = false;
		block();
		IOLockWakeup(lock, this, false);
	}

	bool mutate(IOLock *lock)
	{
		if (!beginMutation(lock))
			return false;
		endMutation(lock);
		return true;
	}

	void close(IOLock *lock)
	{
		closed_ = true;
		block();
		IOLockWakeup(lock, this, false);
		while (active_ || mutating_ || fast())
			IOLockSleep(lock, this, THREAD_UNINT);
	}

	bool closed() const
	{
		return closed_;
	}

private:
	void block()
	{
		__atomic_store_n(&blocked_, closed_ || mutations_ || mutating_, __ATOMIC_SEQ_CST);
	}

	unsigned fast() const
	{
		return __atomic_load_n(&fast_, __ATOMIC_SEQ_CST);
	}

	unsigned active_ = 0, mutations_ = 0;
	bool closed_ = false, mutating_ = false;
	// enterFast's submissions: at most one per channel slot, so a byte (the struct keeps its size)
	uint8_t fast_ = 0;
	bool blocked_ = false;
};
