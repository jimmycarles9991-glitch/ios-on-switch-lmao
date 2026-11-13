// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel headers TegraNVJPG.cpp includes, for switch_nvjpg_gate_test: locks that fail the test when taken twice,
// the idle gate's thread call captured so a test fires it, the bring-up thread run inline, a panic that throws, and
// IOLog lines kept for the test to read.
#pragma once
#include <gtest/gtest.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <strings.h>

struct IOLock {
    bool locked = false;
};

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
    if (lock->locked)
        throw std::runtime_error("lock taken twice: a deadlock on the device");
    lock->locked = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    EXPECT_TRUE(lock->locked);
    lock->locked = false;
}

inline void IOLockSleep(IOLock *, void *, int)
{
    throw std::runtime_error("a CPU sleep no event will end");
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

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

inline void IODelay(unsigned)
{}

[[noreturn]] inline void panic(const char *format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    throw std::runtime_error(std::string("panic: ") + line);
}

class IOMemoryDescriptor {};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};

using IOOptionBits = uint32_t;

using wait_result_t = int;
using thread_t = void *;
using kern_return_t = int;
constexpr int THREAD_UNINT = 0, KERN_SUCCESS = 0;

inline kern_return_t kernel_thread_start(void (*body)(void *, wait_result_t), void *context, thread_t *thread)
{
    *thread = nullptr;
    body(context, 0);
    return KERN_SUCCESS;
}

inline void thread_deallocate(thread_t)
{}

constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000;

inline uint64_t &switch_test_now()
{
    static uint64_t now = 1000000000;
    return now;
}

inline uint64_t mach_absolute_time()
{
    return switch_test_now();
}

inline void nanoseconds_to_absolutetime(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out)
{
    *out = value;
}

inline void clock_interval_to_deadline(uint32_t value, uint32_t scale, uint64_t *out)
{
    *out = mach_absolute_time() + uint64_t(value) * scale;
}

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);

struct thread_call {
    thread_call_func_t fire;
    thread_call_param_t param;
    bool armed;
};

using thread_call_t = thread_call *;
constexpr int THREAD_CALL_PRIORITY_KERNEL = 0;

inline std::vector<thread_call_t> &switch_test_calls()
{
    static std::vector<thread_call_t> calls;
    return calls;
}

inline thread_call_t thread_call_allocate_with_options(thread_call_func_t fire, thread_call_param_t param, int, int)
{
    auto *call = new thread_call{fire, param, false};
    switch_test_calls().push_back(call);
    return call;
}

inline bool thread_call_enter_delayed(thread_call_t call, uint64_t)
{
    call->armed = true;
    return true;
}

#define OS_LOG_DEFAULT                 nullptr
#define os_log(log, format, ...)       ((void)(log), IOLog(format "\n", ##__VA_ARGS__))
#define os_log_error(log, format, ...) ((void)(log), IOLog(format "\n", ##__VA_ARGS__))
