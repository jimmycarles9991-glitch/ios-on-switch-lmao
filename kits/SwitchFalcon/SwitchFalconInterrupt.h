// SPDX-License-Identifier: BSD-2-Clause-Patent
// One engine's falcon error line (SwitchFalconError.h), kernel side: an IOFilterInterruptEventSource on the engine's
// GIC line whose filter takes the error with interrupts off and whose action, on a work loop the four engines share,
// hands it to the engine (which fails the job in flight and has the engine cleared and reset, TRM 15.4.1.4: errors
// that reach the CPU "need software intervention (often a full engine soft-reset)").
//
// The line follows the engine's power: on() once the falcon is booted, off() before any reset, clock gate or power-off,
// as L4T enables the IRQ in finalize_poweron and disables it in prepare_poweroff. off() also waits out a filter running
// on another CPU (a GIC disable does not), so no filter reads an engine being gated: that is the reason for the spin
// lock the filter takes. taken() counts the errors the filter took (its release store follows the error's record, which
// nothing rewrites before the next on()); failed() describes the error taken since the last on(), if one was.
#pragma once
#include <stdint.h>
#include <IOKit/IOLocks.h>
#include "SwitchFalconError.h"

class IOFilterInterruptEventSource;

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
    // at both checks while the filter took none since on() was never delivered: a panic naming it. A taken error
    // stays pending (the falcon is parked on it) until the cancellation clears it.
    void lostCheckStart();
    void lostCheckEnd();

    // The source's filter and action (IOKit's signatures).
    bool filter();
    void action();

private:
    const char *name_ = nullptr;
    uint32_t intid_ = 0;
    volatile uint8_t *engine_ = nullptr;
    Failed failed_ = nullptr;
    IOFilterInterruptEventSource *source_ = nullptr;
    IOSimpleLock *lock_ = nullptr;
    bool powered_ = false;
    uint32_t taken_ = 0, onAt_ = 0;
    bool checkPending_ = false;
    switch_falcon_error error_ = {};
};
