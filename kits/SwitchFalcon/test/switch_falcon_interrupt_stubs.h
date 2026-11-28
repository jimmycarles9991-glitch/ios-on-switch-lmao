// Host stand-ins for the IOKit pieces SwitchFalconInterrupt.cpp uses: a GIC nub with direct interrupt registration
// (the test fires it with switch_test_interrupts_off set, then drains any thread_call entered during the handler)
// and thread_call primitives. The including stubs provide IOLog and IOLock; logging, blocking locks, enableInterrupt,
// or allocations throw while interrupts are off.
#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SWITCH_TEST_IORETURN
#define SWITCH_TEST_IORETURN
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
#endif

inline bool switch_test_interrupts_off;

inline void switch_test_interrupts_off_forbids(const char *what)
{
    if (switch_test_interrupts_off)
        throw std::runtime_error(std::string(what) + " in a primary interrupt handler");
}

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);

struct thread_call {
    thread_call_func_t fire;
    thread_call_param_t param;
    bool armed;
    bool pending;
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
    switch_test_interrupts_off_forbids("thread_call_allocate_with_options");
    auto *call = new thread_call{fire, param, false, false};
    switch_test_calls().push_back(call);
    return call;
}

inline void thread_call_free(thread_call_t call)
{
    switch_test_interrupts_off_forbids("thread_call_free");
    delete call;
}

inline bool thread_call_enter(thread_call_t call)
{
    const bool was = call->pending;
    call->pending = true;
    return was;
}

inline bool thread_call_enter_delayed(thread_call_t call, uint64_t)
{
    call->armed = true;
    return true;
}

inline bool switch_test_drain_calls()
{
    bool ran = false;
    for (thread_call_t call : switch_test_calls()) {
        if (!call->pending)
            continue;
        call->pending = false;
        ran = true;
        call->fire(call->param, nullptr);
    }
    return ran;
}

class OSObject {};

class IOService;
using IOInterruptAction = void (*)(OSObject *, void *, IOService *, int);

class IOService : public OSObject {
public:
    IOReturn registerInterrupt(int, OSObject *t, IOInterruptAction h, void *r)
    {
        switch_test_interrupts_off_forbids("IOService::registerInterrupt");
        target = t;
        handler = h;
        refCon = r;
        enabled = false;
        return kIOReturnSuccess;
    }

    IOReturn unregisterInterrupt(int)
    {
        switch_test_interrupts_off_forbids("IOService::unregisterInterrupt");
        target = nullptr;
        handler = nullptr;
        refCon = nullptr;
        enabled = false;
        return kIOReturnSuccess;
    }

    IOReturn enableInterrupt(int)
    {
        switch_test_interrupts_off_forbids("IOService::enableInterrupt");
        enabled = true;
        return kIOReturnSuccess;
    }

    IOReturn disableInterrupt(int)
    {
        enabled = false;
        return kIOReturnSuccess;
    }

    // An asserted line reaches the handler only while enabled; returns true when a deferred call was scheduled.
    bool fire()
    {
        if (!enabled || !handler)
            return false;
        switch_test_interrupts_off = true;
        try {
            handler(target, refCon, this, 0);
        } catch (...) {
            switch_test_interrupts_off = false;
            throw;
        }
        switch_test_interrupts_off = false;
        return switch_test_drain_calls();
    }

    OSObject *target = nullptr;
    IOInterruptAction handler = nullptr;
    void *refCon = nullptr;
    bool enabled = false;
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
