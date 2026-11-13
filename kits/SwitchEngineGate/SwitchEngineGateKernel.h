// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEngineGate's kernel platform: an IOLock (the engine's own, or one two engines share when each one's power-on
// borrows the other's clock), the idle timer as a one-shot thread call, mach absolute time and os_log.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include "SwitchEngineGate.h"
#include "../SwitchTime/SwitchTime.h"

class SwitchEngineGateKernel {
public:
    bool start(void (*fire)(void *), void *context, IOLock *shared = nullptr)
    {
        fire_ = fire;
        context_ = context;
        lock_ = shared ? shared : IOLockAlloc();
        if (lock_)
            timer_ = thread_call_allocate_with_options(expired, this, THREAD_CALL_PRIORITY_KERNEL, 0);
        return timer_ != nullptr;
    }

    void lock()
    {
        IOLockLock(lock_);
    }

    void unlock()
    {
        IOLockUnlock(lock_);
    }

    void sleep(void *event)
    {
        IOLockSleep(lock_, event, THREAD_UNINT);
    }

    void wake(void *event)
    {
        IOLockWakeup(lock_, event, false);
    }

    // A pending call is moved to the new deadline: the timer runs from the latest arm.
    void arm(uint32_t ms)
    {
        thread_call_enter_delayed(timer_, mach_absolute_time() + switch_abs_from_ms(ms));
    }

    uint64_t nowUs()
    {
        return switch_now_us();
    }

    void log(const char *line)
    {
        os_log(OS_LOG_DEFAULT, "%s", line);
    }

private:
    static void expired(thread_call_param_t self, thread_call_param_t)
    {
        auto *platform = static_cast<SwitchEngineGateKernel *>(self);
        platform->fire_(platform->context_);
    }

    void (*fire_)(void *) = nullptr;
    void *context_ = nullptr;
    IOLock *lock_ = nullptr;
    thread_call_t timer_ = nullptr;
};
