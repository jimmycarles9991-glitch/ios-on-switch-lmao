// SPDX-License-Identifier: MIT
// The present watch's producer, read in the GPU nonstall interrupt's primary filter (shim/xnu_glue.cpp): when its
// writes are complete and visible the display's callout (TegraScanoutCopy.h tgm_present_filter_callout) runs there,
// ahead of the work loop's interrupt pass, whose tgm_present_pass delivers the present as before. The view is attached
// just before the watch is armed and detached at its claim (gpu_present.c), before its tokens drop and done runs; the
// filter holds the lock across its read and the callout, so the callout never runs for a present already delivered,
// and its token's timeline cannot go while it reads.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

namespace
{
IOSimpleLock *gLock;
switch_gpu_timeline_view gView;
bool gCalled;
void (*gCallout)(void *, int);
void *gContext;

IOSimpleLock *lock()
{
	if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE)) {
		IOSimpleLock *fresh = IOSimpleLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
		IOSimpleLock *expected = nullptr;
		if (fresh && !__atomic_compare_exchange_n(&gLock, &expected, fresh, false,
							  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOSimpleLockFree(fresh);
	}
	return __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
}

void setView(const switch_gpu_timeline_view &view)
{
	IOSimpleLock *held = lock();
	if (!held)
		return;
	const IOInterruptState interrupts = IOSimpleLockLockDisableInterrupt(held);
	gView = view;
	gCalled = false;
	IOSimpleLockUnlockEnableInterrupt(held, interrupts);
}
}  // namespace

extern "C" void tgm_present_filter_callout(void (*fn)(void *, int), void *ctx)
{
	IOSimpleLock *held = lock();
	if (!held) {
		IOLog("tegra-gm20b: no lock for the present's interrupt filter: presents complete "
		      "from the work loop\n");
		return;
	}
	const IOInterruptState interrupts = IOSimpleLockLockDisableInterrupt(held);
	gCallout = fn;
	gContext = ctx;
	IOSimpleLockUnlockEnableInterrupt(held, interrupts);
}

// A token whose status needs the timeline's lock (a semantic wait at or below it) is not watched here: the work
// loop's pass delivers it.
extern "C" void tgm_present_filter_attach(const struct switch_gpu_backing_token *token)
{
	switch_gpu_timeline_view view = {};
	if (!switch_gpu_timeline_view_open(token, &view))
		setView(view);
}

extern "C" void tgm_present_filter_detach(void)
{
	setView({});
}

extern "C" void tgm_present_filter(void)
{
	IOSimpleLock *held = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
	if (!held)
		return;
	IOSimpleLockLock(held);
	if (gView.timeline && gCallout && !gCalled &&
	    switch_gpu_timeline_view_visible(&gView) == 1) {
		gCalled = true;
		gCallout(gContext, 0);
	}
	IOSimpleLockUnlock(held);
}
