// Host stand-ins for what TegraMCErrors.cpp takes from the kernel in mc_errors_test: IOKit's filter interrupt event
// source (it hands the filter and action to the test), a work loop, the GIC nub, IOLog into a list, and the spin lock.
// While switch_test_interrupts_off is set (a filter's context), IOLog, a lock, a sleep or an allocation throws.
#ifndef SWITCH_TEST_MC_ERRORS_STUBS_H
#define SWITCH_TEST_MC_ERRORS_STUBS_H

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
using IOInterruptState = bool;

inline bool switch_test_interrupts_off;
inline std::vector<std::string> switch_test_lines;

inline void switch_test_may_block(const char *what)
{
    if (switch_test_interrupts_off)
        throw std::runtime_error(std::string(what) + " in an interrupt filter");
}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    switch_test_may_block("IOLog");
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    switch_test_lines.emplace_back(line);
}

struct IOSimpleLock {
    bool held;
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    switch_test_may_block("IOSimpleLockAlloc");
    return new IOSimpleLock{};
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    switch_test_may_block("IOSimpleLockLockDisableInterrupt");
    lock->held = true;
    return true;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    lock->held = false;
}

class OSObject {};

class IOService : public OSObject {};

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    void enable()
    {
        enabled = true;
    }

    void disable()
    {
        enabled = false;
    }

    Action action = nullptr;
    bool enabled = false;
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    using Filter = bool (*)(OSObject *, IOFilterInterruptEventSource *);

    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *, Action action, Filter filter,
                                                                    IOService *, int)
    {
        auto *source = new IOFilterInterruptEventSource;
        source->action = action;
        source->filter = filter;
        last() = source;
        return source;
    }

    static IOFilterInterruptEventSource *&last()
    {
        static IOFilterInterruptEventSource *source;
        return source;
    }

    // The primary interrupt: the filter with interrupts off, then the action on the work loop when it returned true.
    bool fire()
    {
        switch_test_interrupts_off = true;
        bool taken = false;
        try {
            taken = filter(nullptr, this);
        } catch (...) {
            switch_test_interrupts_off = false;
            throw;
        }
        switch_test_interrupts_off = false;
        if (taken)
            action(nullptr, this, 1);
        return taken;
    }

    Filter filter = nullptr;
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

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

#define NSEC_PER_USEC 1000u

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *deadline)
{
    *deadline = 0;
}

#endif
