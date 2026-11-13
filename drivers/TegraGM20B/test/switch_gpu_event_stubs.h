// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#define IONewZero(type, count) static_cast<type *>(calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) \
	do {                           \
		(void)(count);         \
		free(pointer);         \
	} while (0)
#define OSDeclareDefaultStructors(type) \
public:                                 \
	type() = default;               \
	~type() override = default;
#define OSDefineMetaClassAndStructors(type, base)

class OSObject
{
public:
	std::atomic<unsigned> refs{1};
	virtual ~OSObject() = default;

	bool init()
	{
		return true;
	}

	virtual void free()
	{
		delete this;
	}

	void retain()
	{
		++refs;
	}

	void release()
	{
		if (--refs == 0)
			free();
	}
};

struct IOLock {
	std::mutex mutex;
	std::condition_variable changed;
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
	lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock held(lock->mutex, std::adopt_lock);
	lock->changed.wait(held);
	held.release();
}

inline uint64_t mach_absolute_time()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count());
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *absolute)
{
	*absolute = ns;
}

inline void IOLockSleepDeadline(IOLock *lock, void *, uint64_t deadline, int)
	__attribute__((no_thread_safety_analysis))
{
	std::unique_lock held(lock->mutex, std::adopt_lock);
	lock->changed.wait_until(
		held, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
	held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->changed.notify_all();
}

constexpr int THREAD_UNINT = 0, THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
using thread_call_param_t = void *;

struct SwitchEventCall {
	void (*fn)(void *, void *);
	void *arg;
	std::atomic<bool> pending{false};
};

using thread_call_t = SwitchEventCall *;
extern thread_call_t switch_event_call;

inline thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int,
						       int)
{
	return new SwitchEventCall{fn, arg};
}

inline void thread_call_free(thread_call_t call)
{
	delete call;
}

inline void thread_call_enter(thread_call_t call)
{
	switch_event_call = call;
	call->pending = true;
}
