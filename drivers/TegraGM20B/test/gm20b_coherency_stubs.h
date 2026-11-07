// SPDX-License-Identifier: MIT
// The kernel calls TegraGM20BCoherency.cpp and TegraGM20BExecSurfaces.cpp make, on the host: IOLock as a real mutex (the
// concurrency test runs traps against releases on two threads), IONew/IODelete, IOLog counted, and tgm_cpu_clean_pages
// recording each call's pages.
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

struct IOLock {
	std::mutex m;
};

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->m.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->m.unlock();
}

#define IONew(type, count) (new type[(count)])
#define IODelete(ptr, type, count) (delete[] (ptr))

inline void bzero(void *p, size_t n)
{
	std::memset(p, 0, n);
}

// IOLog's lines, counted (TegraGM20BExecSurfaces.cpp's one-shot line).
inline int &ioLogLines()
{
	static int lines;
	return lines;
}

inline void IOLog(const char *, ...)
{
	ioLogLines()++;
}

// Every tgm_cpu_clean_pages call: its page list.
std::vector<std::vector<uint64_t>> &cleanCalls();
