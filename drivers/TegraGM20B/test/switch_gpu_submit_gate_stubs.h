// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>

struct IOLock {
	std::mutex mutex;
	std::condition_variable ready;
	std::function<void()> sleeping;
};

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	if (lock->sleeping)
		lock->sleeping();
	std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
	lock->ready.wait(held);
	held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->ready.notify_all();
}

#define THREAD_UNINT 0
