// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLib.h>
#include <kern/cpu_number.h>
#include <string.h>
#include "TegraGM20BSleepGate.h"

extern "C" void _disable_preemption(void);  // NOLINT(bugprone-reserved-identifier): XNU's own name
extern "C" void _enable_preemption(void);   // NOLINT(bugprone-reserved-identifier): XNU's own name

// A connection's IOKit entries without IOKit's default locking (kIOUserClientDefaultLockingKey false, as Apple's AGX
// runs): each counts itself in and out on its CPU's cell of a tgm_gate (TegraGM20BSleepGate.h), so an entry writes no
// line another CPU writes, and close() shuts the gate and sleeps under `lock` until the last entry is out. An entry that
// meets the shut gate is refused, never queued: a nested one (mapToken's clientMemoryForType) cannot wait on the close
// that waits on its outer entry.
class SwitchGpuMethodGate
{
public:
	bool initialize()
	{
		gate_ = static_cast<tgm_gate *>(
			IOMallocAligned(sizeof(tgm_gate), alignof(tgm_gate)));
		if (gate_)
			bzero(gate_, sizeof(*gate_));
		return gate_ != nullptr;
	}

	void destroy()
	{
		if (gate_)
			IOFreeAligned(gate_, sizeof(*gate_));
		gate_ = nullptr;
	}

	bool enter(IOLock *lock)
	{
		_disable_preemption();
		const tgm_gate_entry entry =
			tgm_gate_try_enter(gate_, static_cast<unsigned int>(cpu_number()));
		_enable_preemption();
		if (entry == TGM_GATE_SHUT_WAKE)
			wakeCloser(lock);
		return entry == TGM_GATE_IN;
	}

	// `lock` not held
	void leave(IOLock *lock)
	{
		_disable_preemption();
		const int wake = tgm_gate_leave(gate_, static_cast<unsigned int>(cpu_number()));
		_enable_preemption();
		if (wake)
			wakeCloser(lock);
	}

	// `lock` held
	void close(IOLock *lock)
	{
		tgm_gate_close(gate_);
		while (!tgm_gate_drained(gate_))
			IOLockSleep(lock, gate_, THREAD_UNINT);
	}

private:
	void wakeCloser(IOLock *lock)
	{
		IOLockLock(lock);
		IOLockWakeup(lock, gate_, false);
		IOLockUnlock(lock);
	}

	tgm_gate *gate_ = nullptr;
};
