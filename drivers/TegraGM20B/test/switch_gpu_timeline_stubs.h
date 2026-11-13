// SPDX-License-Identifier: MIT
#pragma once
#include <cstdlib>
#include <mutex>

inline thread_local bool switchTimelineAtomicContext;

inline void switchTimelineMayBlock()
{
	if (switchTimelineAtomicContext)
		std::abort();
}

struct IOLock {
	std::mutex mutex;
};

inline IOLock *IOLockAlloc()
{
	switchTimelineMayBlock();
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	switchTimelineMayBlock();
	delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	switchTimelineMayBlock();
	lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

// Held, interrupts are off: nothing under it may block (switchTimelineMayBlock aborts).
struct IOSimpleLock {
	std::mutex mutex;
};

using IOInterruptState = bool;

inline IOSimpleLock *IOSimpleLockAlloc()
{
	switchTimelineMayBlock();
	return new IOSimpleLock;
}

inline void IOSimpleLockFree(IOSimpleLock *lock)
{
	delete lock;
}

inline void IOSimpleLockLock(IOSimpleLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
	switchTimelineAtomicContext = true;
}

inline void IOSimpleLockUnlock(IOSimpleLock *lock) __attribute__((no_thread_safety_analysis))
{
	switchTimelineAtomicContext = false;
	lock->mutex.unlock();
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
	IOSimpleLockLock(lock);
	return true;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
	IOSimpleLockUnlock(lock);
}

inline void IOLog(const char *, ...)
{
	switchTimelineMayBlock();
}

template <class T> T *switchTimelineNew(unsigned long count)
{
	switchTimelineMayBlock();
	return new T[count];
}

template <class T> void switchTimelineDelete(T *value)
{
	switchTimelineMayBlock();
	delete[] value;
}

#define IONew(type, count) switchTimelineNew<type>(count)
#define IODelete(value, type, count) switchTimelineDelete(value)
