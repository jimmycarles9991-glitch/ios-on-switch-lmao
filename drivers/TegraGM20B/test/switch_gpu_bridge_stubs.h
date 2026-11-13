// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

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

// No IOLockSleepDeadline: a bridge waiter sleeps until it is woken, never on a clock.
inline void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock held(lock->mutex, std::adopt_lock);
	lock->changed.wait(held);
	held.release();
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->changed.notify_all();
}

constexpr int THREAD_UNINT = 0, THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
inline uint64_t switch_bridge_now;

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
	*deadline = switch_bridge_now + uint64_t(interval) * scale;
}

using thread_call_param_t = void *;

struct SwitchBridgeCall {
	void (*fn)(void *, void *);
	void *arg;
	bool pending;
	uint64_t deadline;
};

using thread_call_t = SwitchBridgeCall *;
// In allocation order: the retirement, then the hang check.
inline std::vector<thread_call_t> switch_bridge_calls;
inline std::atomic<unsigned> switch_bridge_arms;

inline thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int,
						       int)
{
	switch_bridge_calls.push_back(new SwitchBridgeCall{fn, arg, false, 0});
	return switch_bridge_calls.back();
}

inline void thread_call_free(thread_call_t call)
{
	delete call;
}

inline void thread_call_enter(thread_call_t call)
{
	call->pending = true;
}

inline bool thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
	call->deadline = deadline;
	++switch_bridge_arms;
	switch_bridge_arms.notify_all();
	return false;
}

inline bool thread_call_cancel(thread_call_t call)
{
	call->deadline = 0;
	return true;
}

[[noreturn]] void panic(const char *, ...);
inline unsigned switch_bridge_logs;

inline void IOLog(const char *, ...)
{
	++switch_bridge_logs;
}
