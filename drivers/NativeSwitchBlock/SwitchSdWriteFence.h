// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>

struct SwitchSdWriteFence {
    struct Barrier {
        uint64_t target;
        bool failed;
        Barrier *next;
    };

    uint64_t submitted = 0, completed = 0;
    Barrier *waiters = nullptr;
    bool unreportedFailure = false;

    void begin(Barrier &barrier)
    {
        barrier = {submitted, unreportedFailure, waiters};
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
