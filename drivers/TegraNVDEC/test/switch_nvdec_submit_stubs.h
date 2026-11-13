// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <gtest/gtest.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <new>
#include <cstdlib>
#include <stdexcept>

inline bool switch_test_queue_locked = false;

struct IOLock {
    bool locked = false;
};

inline IOLock *switch_test_queue_lock = nullptr;

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    EXPECT_FALSE(lock->locked);
    lock->locked = true;
    if (lock == switch_test_queue_lock)
        switch_test_queue_locked = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    EXPECT_TRUE(lock->locked);
    lock->locked = false;
    if (lock == switch_test_queue_lock)
        switch_test_queue_locked = false;
}

// Set by a test that expects one sleep: what wakes the sleeper, run with the lock dropped as IOLockSleep drops it.
inline void (*switch_test_while_asleep)() = nullptr;

inline void IOLockSleep(IOLock *lock, void *, int)
{
    auto *wake = switch_test_while_asleep;
    if (!wake)
        throw std::runtime_error("unexpected CPU sleep");
    switch_test_while_asleep = nullptr;
    IOLockUnlock(lock);
    wake();
    IOLockLock(lock);
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

#define IONew(type, count)             static_cast<type *>(std::malloc(sizeof(type) * (count)))
#define IONewZero(type, count)         static_cast<type *>(std::calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) ((void)(count), std::free(pointer))

class IOMemoryDescriptor {};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};

using IOOptionBits = uint32_t;
using wait_result_t = int;
using thread_t = void *;
constexpr thread_t THREAD_NULL = nullptr;
constexpr int THREAD_UNINT = 0, KERN_SUCCESS = 0;

inline int kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *)
{
    return 0;
}

inline void thread_deallocate(thread_t)
{}

[[noreturn]] inline void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000;

inline void nanoseconds_to_absolutetime(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline uint64_t mach_absolute_time()
{
    return 1000000;
}

inline void clock_interval_to_deadline(uint32_t value, uint32_t scale, uint64_t *out)
{
    *out = uint64_t(value) * scale;
}

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);

struct thread_call {
    thread_call_func_t fire;
    thread_call_param_t param;
    uint32_t entered;
};

using thread_call_t = thread_call *;
constexpr int THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1;

inline thread_call_t thread_call_allocate_with_options(thread_call_func_t fire, thread_call_param_t param, int, int)
{
    return new thread_call{fire, param, 0};
}

inline bool thread_call_enter(thread_call_t call)
{
    ++call->entered;
    return true;
}

inline std::vector<std::string> &switch_test_log()
{
    static std::vector<std::string> lines;
    return lines;
}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    switch_test_log().emplace_back(line);
}
