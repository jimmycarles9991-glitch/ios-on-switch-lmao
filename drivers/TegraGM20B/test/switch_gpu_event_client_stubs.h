// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdlib>
#include <mutex>
using IOLock = std::mutex;

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

inline void __attribute__((no_thread_safety_analysis)) IOLockLock(IOLock *lock)
{
	lock->lock();
}

inline void __attribute__((no_thread_safety_analysis)) IOLockUnlock(IOLock *lock)
{
	lock->unlock();
}

#define IONew(type, count) static_cast<type *>(calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) free(pointer)
