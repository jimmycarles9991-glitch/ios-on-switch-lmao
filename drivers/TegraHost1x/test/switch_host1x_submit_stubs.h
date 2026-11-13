// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraHost1x.cpp and SwitchHost1xChannels.cpp see it, for their host test: locks that only track
// ownership, a sleep that runs the test's hook in place of the scheduler, no interrupts.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr int THREAD_UNINT = 0, THREAD_TIMED_OUT = 1;
constexpr uint32_t kMicrosecondScale = 1000, kMillisecondScale = 1000000;
constexpr uint64_t NSEC_PER_USEC = 1000, NSEC_PER_MSEC = 1000000;
constexpr size_t PAGE_SIZE = 16384;

struct IOLock {
    bool held;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

inline void IOLockWakeup(IOLock *, void *, bool)
{}

inline int IOLockSleepDeadline(IOLock *, void *, uint64_t, int)
{
    throw std::runtime_error("a submission never sleeps");
}

inline std::function<void()> switch_test_sleep;

inline int IOLockSleep(IOLock *lock, void *, int)
{
    if (!switch_test_sleep)
        throw std::runtime_error("unexpected sleep");
    lock->held = false;
    switch_test_sleep();
    lock->held = true;
    return 0;
}

inline void IOLog(const char *, ...)
{}

inline void IODelay(unsigned)
{}

[[noreturn]] inline void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = uint64_t(interval) * scale;
}

class OSObject {};

class IOService : public OSObject {};

class IOBufferMemoryDescriptor : public OSObject {};

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    static IOInterruptEventSource *interruptEventSource(OSObject *, Action, IOService *, int)
    {
        static IOInterruptEventSource source;
        return &source;
    }

    void enable()
    {}
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    using Filter = bool (*)(OSObject *, IOFilterInterruptEventSource *);

    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *, Action, Filter, IOService *, int)
    {
        static IOFilterInterruptEventSource source;
        return &source;
    }
};

class IOWorkLoop : public OSObject {
public:
    static IOWorkLoop *workLoop()
    {
        static IOWorkLoop loop;
        return &loop;
    }

    IOReturn addEventSource(IOInterruptEventSource *)
    {
        return kIOReturnSuccess;
    }
};

// DEBUG builds of SwitchHost1xChannels.cpp write their stats line here.
inline std::string switch_test_log;
#define OS_LOG_DEFAULT   nullptr
#define os_log(log, ...) switch_test_os_log(__VA_ARGS__)

inline void switch_test_os_log(const char *format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    switch_test_log = text;
}

inline void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *out)
{
    *out = nanoseconds;
}

inline void absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *out)
{
    *out = absolute;
}
