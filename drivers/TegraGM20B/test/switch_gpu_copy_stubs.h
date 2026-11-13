// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>
using IOLock = std::mutex;

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *p)
{
	delete p;
}

inline void IOLockLock(IOLock *p) __attribute__((no_thread_safety_analysis))
{
	p->lock();
}

inline void IOLockUnlock(IOLock *p) __attribute__((no_thread_safety_analysis))
{
	p->unlock();
}

#define IONew(t, n) static_cast<t *>(calloc(n, sizeof(t)))
#define IODelete(p, t, n) ((void)(n), free(p))
constexpr int kIOReturnSuccess = 0, kIOMemoryMapperNone = 0;

class IOMemoryDescriptor
{
public:
	std::vector<uint64_t> pages;
	uint64_t length = 0;
	unsigned refs = 1, prepared = 1, walks = 0, prepares = 0;
	bool refuse = false;

	int prepare()
	{
		if (refuse)
			return -1;
		++prepared;
		++prepares;
		return 0;
	}

	void complete()
	{
		--prepared;
	}

	void retain()
	{
		++refs;
	}

	void release()
	{
		--refs;
	}

	uint64_t getLength() const
	{
		return length;
	}

	uint64_t getPhysicalSegment(uint64_t offset, void *, int)
	{
		++walks;
		if (offset >= length)
			return 0;
		return pages.at(size_t(offset / 4096)) + (offset % 4096);
	}
};

constexpr int THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
using thread_call_param_t = void *;

struct SwitchCopyCall {
	void (*fn)(void *, void *);
	void *arg;
	bool pending = false;
};

using thread_call_t = SwitchCopyCall *;

inline thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int,
						       int)
{
	return new SwitchCopyCall{fn, arg};
}

inline void thread_call_free(thread_call_t p)
{
	delete p;
}

inline void thread_call_enter(thread_call_t p)
{
	p->pending = true;
}
