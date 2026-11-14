// Host stand-ins for the IOKit pieces SwitchFalconInterrupt.cpp uses: the spin lock, a filter interrupt event source the
// test fires by hand (filter with switch_test_interrupts_off set, then the action when it returned true, as IOKit masks
// a level line until then), the work loop and the GIC nub. The including stubs provide IOLog and IOLock; a spin lock
// taken with interrupts disabled, a work-loop call or an allocation throws while interrupts are off.
#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using IOInterruptState = bool;
#ifndef SWITCH_TEST_IORETURN
#define SWITCH_TEST_IORETURN
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
#endif

inline bool switch_test_interrupts_off;

inline void switch_test_interrupts_off_forbids(const char *what)
{
    if (switch_test_interrupts_off)
        throw std::runtime_error(std::string(what) + " in an interrupt filter");
}

struct IOSimpleLock {
    bool held;
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    switch_test_interrupts_off_forbids("IOSimpleLockAlloc");
    return new IOSimpleLock{};
}

inline void IOSimpleLockLock(IOSimpleLock *lock)
{
    if (lock->held)
        throw std::runtime_error("spin lock taken twice: a deadlock on the device");
    lock->held = true;
}

inline void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    switch_test_interrupts_off_forbids("IOSimpleLockLockDisableInterrupt");
    IOSimpleLockLock(lock);
    return true;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    IOSimpleLockUnlock(lock);
}

class OSObject {};

class IOService : public OSObject {};

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    void enable()
    {
        switch_test_interrupts_off_forbids("IOInterruptEventSource::enable");
        enabled = true;
    }

    void disable()
    {
        switch_test_interrupts_off_forbids("IOInterruptEventSource::disable");
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
        return source;
    }

    // An asserted line reaches the filter only while the source is enabled.
    bool fire()
    {
        if (!enabled)
            return false;
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

    IOReturn addEventSource(IOInterruptEventSource *source)
    {
        last() = static_cast<IOFilterInterruptEventSource *>(source);
        return kIOReturnSuccess;
    }

    static IOFilterInterruptEventSource *&last()
    {
        static IOFilterInterruptEventSource *source;
        return source;
    }
};

inline IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

inline void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size)
{
    snprintf(buffer, size, "INTID %u model", intid);
}
