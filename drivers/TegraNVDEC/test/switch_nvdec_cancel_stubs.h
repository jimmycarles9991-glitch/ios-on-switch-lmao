// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel headers TegraNVDECCancel.cpp includes, for switch_nvdec_cancel_test: a real lock other threads can take,
// the cancellation's thread call kept so a test runs it on a thread of its own.
#pragma once
#include <cstdint>
#include <mutex>

struct IOLock {
    std::mutex mutex;
};

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.unlock();
}

using thread_call_param_t = void *;

struct SwitchCancelCall {
    void (*fn)(thread_call_param_t, thread_call_param_t);
};

using thread_call_t = SwitchCancelCall *;
constexpr int THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;
inline thread_call_t switch_cancel_entered;

inline thread_call_t thread_call_allocate_with_options(void (*fn)(thread_call_param_t, thread_call_param_t), void *,
                                                       int, int)
{
    return new SwitchCancelCall{fn};
}

inline void thread_call_enter(thread_call_t call)
{
    switch_cancel_entered = call;
}

inline void IOLog(const char *, ...)
{}
