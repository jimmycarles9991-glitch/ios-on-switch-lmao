// SPDX-License-Identifier: MIT
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSObject.h>
#include "OriginalIOSurfaceSharedEvent.h"
#include "SwitchGpuSharedEventNative.h"
#include "SwitchGpuEventPending.h"

bool switch_gpu_event_native_waiter_cancel(OSObject *event, OSObject *waiter)
{
	using ABI = OriginalIOSurfaceSharedEvent;
	auto *lock =
		*reinterpret_cast<IOLock **>(reinterpret_cast<uint8_t *>(event) + ABI::eventLock);
	IOLockLock(lock);
	const bool removed = SwitchGpuEventPending::remove(
		event, waiter,
		{ABI::pendingHead, ABI::pendingTail, ABI::waiterNext, ABI::waiterPrevious});
	IOLockUnlock(lock);
	if (removed)
		waiter->release();
	return removed;
}
