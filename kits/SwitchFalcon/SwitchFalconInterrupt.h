// SPDX-License-Identifier: BSD-2-Clause-Patent
// One engine's falcon error line (SwitchFalconError.h), kernel side: a direct primary interrupt handler on the
// engine's GIC line that reads the parked error, masks the level line via disableInterrupt, and schedules a
// thread_call to notify the engine (which fails the job in flight and has the engine cleared and reset, TRM 15.4.1.4:
// errors that reach the CPU "need software intervention (often a full engine soft-reset)").
//
// The line follows the engine's power: on() publishes powered_ before enabling the THI mask and GIC line; off()
// disables the GIC line (IOInterruptController::disableInterrupt waits out any active primary handler on another CPU)
// before clearing powered_. taken() counts the errors the handler took (its release store follows the error's record,
// which nothing rewrites before the next on()); failed() describes the error taken since the last on(), if one was.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SwitchFalconError.h"

class IOService;
class OSObject;
struct thread_call;

class SwitchFalconInterrupt {
public:
    using Failed = void (*)(const switch_falcon_error &error);

    // Thread context, at bring-up; false with one line when the line cannot be had.
    bool start(const char *name, uint32_t intid, volatile uint8_t *engine, Failed failed);
    void on();
    void off();

    uint32_t taken() const
    {
        return __atomic_load_n(&taken_, __ATOMIC_ACQUIRE);
    }

    const uint32_t *takenWord() const
    {
        return &taken_;
    }

    uint32_t onAt() const
    {
        return onAt_;
    }

    bool failed(char *text, size_t size) const
    {
        if (taken() == onAt_)
            return false;
        switch_falcon_error_describe(&error_, text, size);
        return true;
    }

    // At a cancellation, after the channel teardown and the memory-client flush and before the reset: an error pending
    // at both checks while the handler took none since on() was never delivered: a panic naming it. A taken error
    // stays pending (the falcon is parked on it) until the cancellation clears it.
    void lostCheckStart();
    void lostCheckEnd();

private:
    static void interruptHandler(OSObject *, void *refCon, IOService *nub, int source);
    static void deferredAction(void *param, void *);

    const char *name_ = nullptr;
    uint32_t intid_ = 0;
    volatile uint8_t *engine_ = nullptr;
    Failed failed_ = nullptr;
    IOService *nub_ = nullptr;
    struct thread_call *call_ = nullptr;
    bool powered_ = false;
    uint32_t taken_ = 0, onAt_ = 0;
    bool checkPending_ = false;
    switch_falcon_error error_ = {};
};
