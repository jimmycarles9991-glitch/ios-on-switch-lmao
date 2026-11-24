/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_TEST_A3_RADIO_STUBS_H
#define SWITCH_TEST_A3_RADIO_STUBS_H

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

#define enqueue(queue, elt) enqueue_tail(queue, elt)

inline uint64_t switch_test_now_ns = 10000000000ULL;
inline unsigned switch_test_adapter_locks;
inline bool switch_test_core_lock;
inline std::vector<std::string> switch_test_log;

inline void IOLog(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    switch_test_log.emplace_back(line);
}

struct IOLock {
    bool held = false;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void *IOMallocZero(size_t size)
{
    return calloc(1, size);
}

inline void IOFree(void *allocation, size_t)
{
    free(allocation);
}

inline void IOLockLock(IOLock *lock)
{
    assert(lock && !lock->held);
    lock->held = true;
    ++switch_test_adapter_locks;
}

inline void IOLockUnlock(IOLock *lock)
{
    assert(lock && lock->held && switch_test_adapter_locks);
    lock->held = false;
    --switch_test_adapter_locks;
}

inline constexpr int THREAD_UNINT = 0;
inline constexpr int THREAD_TIMED_OUT = 1;
inline std::function<void()> switch_test_sleep_action;
inline bool switch_test_run_one();

inline void IOLockWakeup(IOLock *, const void *, bool)
{}

using thread_call_param_t = void *;

struct switch_test_thread_call {
    void (*function)(void *, void *);
    void *context;
    bool pending = false;
    uint64_t deadline = 0;
};

using thread_call_t = switch_test_thread_call *;
inline std::vector<thread_call_t> switch_test_calls;

inline thread_call_t thread_call_allocate(void (*function)(void *, void *), void *context)
{
    auto *call = new switch_test_thread_call{function, context};
    switch_test_calls.push_back(call);
    return call;
}

inline void thread_call_free(thread_call_t call)
{
    delete call;
}

inline void thread_call_enter(thread_call_t call)
{
    call->deadline = 0;
    call->pending = true;
}

inline void thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
    call->deadline = deadline;
    call->pending = true;
}

inline void thread_call_cancel(thread_call_t call)
{
    call->pending = false;
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *ticks)
{
    *ticks = ns;
}

inline constexpr uint32_t kMillisecondScale = 1000000;

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = switch_test_now_ns + static_cast<uint64_t>(interval) * scale;
}

inline uint64_t mach_absolute_time()
{
    return switch_test_now_ns;
}

inline void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns)
{
    *ns = ticks;
}

inline bool switch_test_run_one()
{
    assert(!switch_test_core_lock && !switch_test_adapter_locks);
    for (auto *call : switch_test_calls) {
        if (call->pending && call->deadline <= switch_test_now_ns) {
            call->pending = false;
            call->function(call->context, nullptr);
            return true;
        }
    }
    return false;
}

inline int IOLockSleep(IOLock *lock, const void *, int)
{
    IOLockUnlock(lock);
    if (switch_test_sleep_action) {
        auto action = std::move(switch_test_sleep_action);
        switch_test_sleep_action = {};
        action();
    }
    if (!switch_test_run_one()) {
        uint64_t deadline = UINT64_MAX;
        for (const auto *call : switch_test_calls)
            if (call->pending && call->deadline < deadline)
                deadline = call->deadline;
        assert(deadline != UINT64_MAX);
        switch_test_now_ns = deadline;
        assert(switch_test_run_one());
    }
    IOLockLock(lock);
    return 0;
}

inline int IOLockSleepDeadline(IOLock *lock, const void *, uint64_t deadline, int)
{
    IOLockUnlock(lock);
    if (switch_test_sleep_action) {
        auto action = std::move(switch_test_sleep_action);
        switch_test_sleep_action = {};
        action();
    }
    bool progress = switch_test_run_one();
    if (!progress) {
        uint64_t next = UINT64_MAX;
        for (const auto *call : switch_test_calls)
            if (call->pending && call->deadline < next)
                next = call->deadline;
        if (next <= deadline) {
            if (next > switch_test_now_ns)
                switch_test_now_ns = next;
            progress = switch_test_run_one();
        } else if (deadline > switch_test_now_ns) {
            switch_test_now_ns = deadline;
        }
    }
    IOLockLock(lock);
    return progress ? 0 : THREAD_TIMED_OUT;
}
#endif
