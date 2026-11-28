// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>

struct SwitchSdWriteFence {
    struct Barrier {
        uint64_t target;
        bool failed;
        Barrier *next;
        uint64_t beganNs;
    };

    uint64_t submitted = 0, completed = 0;
    Barrier *waiters = nullptr;
    bool unreportedFailure = false;

    void begin(Barrier &barrier, uint64_t nowNs)
    {
        barrier = {submitted, unreportedFailure, waiters, nowNs};
        waiters = &barrier;
        unreportedFailure = false;
    }

    void finish(uint32_t requests, bool failed)
    {
        if (failed) {
            bool fullyReported = false;
            for (Barrier *b = waiters; b; b = b->next) {
                if (b->target > completed)
                    b->failed = true;
                if (b->target >= completed + requests)
                    fullyReported = true;
            }
            if (!fullyReported)
                unreportedFailure = true;
        }
        completed += requests;
    }

    // Every write submitted before the newest waiting barrier began is in that barrier's count; 0 while none waits.
    // A write is stamped before it is queued, so one stamped earlier than this may be queued just after it and not be
    // counted: the time can only take in an uncounted write, never leave out a counted one.
    uint64_t awaitedBeforeNs() const
    {
        return waiters ? waiters->beganNs : 0;
    }

    void end(Barrier &barrier, bool preserveFailure)
    {
        Barrier **link = &waiters;
        while (*link != &barrier)
            link = &(*link)->next;
        *link = barrier.next;
        if (barrier.failed && preserveFailure)
            unreportedFailure = true;
    }
};
