// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <stdint.h>
#include "SwitchGpuInheritorSleep.h"

// Every operation but enterFast and leaveFast is called with the connection lock held. A queued mutation prevents new
// submissions from extending its drain; sleeping releases that lock so other channels, status reads and CPU unlocks can
// progress. enterFast admits a submission without the lock while no mutation is queued or running; enterSlow is its
// wait when one is, and leaveFast takes the lock only to wake a mutation waiting for the last one.
class SwitchGpuSubmitGate
{
public:
	static constexpr uint32_t kSlots = 8;

	bool enterFast(uint32_t slot)
	{
		if (__atomic_load_n(&blocked_, __ATOMIC_RELAXED))
			return false;
		__atomic_store_n(&slotActive_[slot], 1u, __ATOMIC_RELEASE);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!__atomic_load_n(&blocked_, __ATOMIC_ACQUIRE))
			return true;
		__atomic_store_n(&slotActive_[slot], 0u, __ATOMIC_RELEASE);
		return false;
	}

	bool enterSlow(uint32_t slot, IOLock *lock)
	{
		wake(lock);
		while (!closed_ && (mutations_ || mutating_))
			sleep(lock);
		if (closed_)
			return false;
		__atomic_store_n(&slotActive_[slot], 1u, __ATOMIC_RELEASE);
		return true;
	}

	void leaveFast(uint32_t slot, IOLock *lock)
	{
		__atomic_store_n(&slotActive_[slot], 0u, __ATOMIC_RELEASE);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!__atomic_load_n(&blocked_, __ATOMIC_RELAXED) || fast())
			return;
		IOLockLock(lock);
		wake(lock);
		IOLockUnlock(lock);
	}

	bool enterFast()
	{
		if (__atomic_load_n(&blocked_, __ATOMIC_RELAXED))
			return false;
		__atomic_add_fetch(&fast_, 1u, __ATOMIC_SEQ_CST);
		if (!__atomic_load_n(&blocked_, __ATOMIC_SEQ_CST))
			return true;
		__atomic_sub_fetch(&fast_, 1u, __ATOMIC_RELEASE);
		return false;
	}

	bool enterSlow(IOLock *lock)
	{
		// enterFast's refused count was undone with no wake: a mutation that saw it as the last one is woken here
		wake(lock);
		while (!closed_ && (mutations_ || mutating_))
			sleep(lock);
		if (closed_)
			return false;
		__atomic_add_fetch(&fast_, 1u, __ATOMIC_RELEASE);
		return true;
	}

	void leaveFast(IOLock *lock)
	{
		if (__atomic_sub_fetch(&fast_, 1u, __ATOMIC_SEQ_CST) ||
		    !__atomic_load_n(&blocked_, __ATOMIC_SEQ_CST) || anySlotActive())
			return;
		IOLockLock(lock);
		wake(lock);
		IOLockUnlock(lock);
	}

	bool beginMutation(IOLock *lock)
	{
		++mutations_;
		block();
		while (!closed_ && (mutating_ || fast()))
			sleep(lock);
		--mutations_;
		if (closed_) {
			wake(lock);
			return false;
		}
		mutating_ = true;
		mutator_ = switch_gpu_current_thread();
		// Whoever went to sleep while this mutation was only queued slept with no owner to push: woken, each sleeps
		// again pushing the mutator
		wake(lock);
		return true;
	}

	void endMutation(IOLock *lock)
	{
		mutating_ = false;
		mutator_ = nullptr;
		block();
		wake(lock);
	}

	void close(IOLock *lock)
	{
		closed_ = true;
		block();
		wake(lock);
		while (mutating_ || fast())
			sleep(lock);
	}

	bool closed() const
	{
		return closed_;
	}

	// Until the running mutation ends (it may drop the lock meanwhile), pushing its thread. Every sleep on the gate goes
	// through the gate: its sleeper count decides whether endMutation wakes anyone.
	void waitMutationEnd(IOLock *lock)
	{
		while (mutating_)
			sleep(lock);
	}

	// A method that closed the gate is running (it may have dropped the lock); its end wakes sleepers on this gate
	bool mutating() const
	{
		return mutating_;
	}

	SwitchGpuThreadRef mutator() const
	{
		return mutator_;
	}

private:
	// lock held, as every sleep and wake on the gate: the count is exact, so a gate no one sleeps on pays no wake
	void sleep(IOLock *lock)
	{
		++sleepers_;
		switch_gpu_sleep_inheritor(lock, this, mutator_);
		--sleepers_;
	}

	void wake(IOLock *lock)
	{
		if (sleepers_)
			switch_gpu_wakeup_inheritor(lock, this);
	}

	void block()
	{
		__atomic_store_n(&blocked_, closed_ || mutations_ || mutating_, __ATOMIC_RELEASE);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
	}

	bool anySlotActive() const
	{
		for (uint32_t i = 0; i < kSlots; ++i)
			if (__atomic_load_n(&slotActive_[i], __ATOMIC_ACQUIRE))
				return true;
		return false;
	}

	unsigned fast() const
	{
		return __atomic_load_n(&fast_, __ATOMIC_ACQUIRE) || anySlotActive();
	}

	unsigned mutations_ = 0, sleepers_ = 0;
	bool closed_ = false, mutating_ = false;
	SwitchGpuThreadRef mutator_ = nullptr;
	uint8_t slotActive_[kSlots] = {};
	uint8_t fast_ = 0;
	bool blocked_ = false;
};
