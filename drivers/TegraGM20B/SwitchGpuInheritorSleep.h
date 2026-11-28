// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef KERNEL
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
extern "C" {
wait_result_t lck_mtx_sleep_with_inheritor(lck_mtx_t *lock, lck_sleep_action_t lck_sleep_action,
					   event_t event, thread_t inheritor,
					   wait_interrupt_t interruptible, uint64_t deadline);
kern_return_t wakeup_all_with_inheritor(event_t event, wait_result_t result);
}

using SwitchGpuThreadRef = thread_t;

static inline SwitchGpuThreadRef switch_gpu_current_thread()
{
	return current_thread();
}

// A turnstile sleep needs a thread to push: stackshot's kdp_sleep_with_inheritor_find_owner asserts one (MACH_ASSERT,
// on in DEVELOPMENT kernels), and a null inheritor replaces an earlier sleeper's push on the same event. With no owner
// the sleep is a plain one on the event's global wait queue, so every wake reaches both queues.
static inline void switch_gpu_sleep_inheritor(IOLock *lock, void *event,
					      SwitchGpuThreadRef inheritor)
{
	if (!inheritor) {
		IOLockSleep(lock, event, THREAD_UNINT);
		return;
	}
	(void)lck_mtx_sleep_with_inheritor(IOLockGetMachLock(lock), LCK_SLEEP_DEFAULT,
					   reinterpret_cast<event_t>(event), inheritor,
					   THREAD_UNINT, TIMEOUT_WAIT_FOREVER);
}

static inline void switch_gpu_wakeup_inheritor(IOLock *lock, void *event)
{
	(void)wakeup_all_with_inheritor(reinterpret_cast<event_t>(event), THREAD_AWAKENED);
	IOLockWakeup(lock, event, false);
}
#else
using SwitchGpuThreadRef = void *;

static inline SwitchGpuThreadRef switch_gpu_current_thread()
{
	return nullptr;
}

static inline void switch_gpu_sleep_inheritor(IOLock *lock, void *event, SwitchGpuThreadRef)
{
	IOLockSleep(lock, event, THREAD_UNINT);
}

static inline void switch_gpu_wakeup_inheritor(IOLock *lock, void *event)
{
	IOLockWakeup(lock, event, false);
}
#endif
