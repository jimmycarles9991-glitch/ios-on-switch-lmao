// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <vector>
#define IONew(type, count) static_cast<type *>(calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) free(pointer)

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

extern std::atomic<unsigned> switch_zero_sleepers;

inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock held(lock->mutex, std::adopt_lock);
	++switch_zero_sleepers;
	switch_zero_sleepers.notify_all();
	lock->changed.wait(held);
	--switch_zero_sleepers;
	held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->changed.notify_all();
}

constexpr int THREAD_UNINT = 0, THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
using thread_call_param_t = void *;

struct SwitchZeroCall {
	void (*fn)(void *, void *);
	void *arg;
	std::atomic<bool> pending{false};
	uint64_t deadline = 0;
};

using thread_call_t = SwitchZeroCall *;
extern std::vector<thread_call_t> switch_zero_calls;

inline thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int,
						       int)
{
	auto call = new SwitchZeroCall{fn, arg};
	switch_zero_calls.push_back(call);
	return call;
}

inline void thread_call_free(thread_call_t call)
{
	delete call;
}

inline void thread_call_enter(thread_call_t call)
{
	call->pending.store(true);
}

inline void thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
	call->deadline = deadline;
}

inline void thread_call_cancel(thread_call_t call)
{
	call->deadline = 0;
}

extern uint64_t switch_zero_now;

inline uint64_t mach_absolute_time()
{
	return switch_zero_now;
}

inline void clock_interval_to_deadline(unsigned interval, unsigned scale, uint64_t *out)
{
	*out = switch_zero_now + uint64_t(interval) * scale;
}

void switch_zero_cache(const char *, uintptr_t);
#define SWITCH_DCACHE_OP(op, address) switch_zero_cache(op, address)
#define SWITCH_DCACHE_BARRIER() ((void)0)
[[noreturn]] void panic(const char *, ...);
