// SPDX-License-Identifier: BSD-2-Clause-Patent
// What SwitchXUDC's stats line counts (SwitchXUDCStats.cpp). Only DEVELOPMENT and DEBUG kernels count: in release the
// struct is empty, its clock reads 0 and every method is an empty inline, so the interrupt filter, the event drain and
// the queueing carry none of it.
#pragma once
#include <kern/clock.h>
#include "XudcTransfer.h"

namespace xudc {

#if SWITCH_STATS_IO
struct XudcStats {
    // Bytes, requests, failed requests and bounced bytes per direction (0 OUT, 1 IN), requests by the serial path, why
    // packets were bounced (XudcTransfer.h PlanStats), events, time in the event drain and queueing (the filter's is
    // filterTicks until a stats line takes it); transfer events by kind (XudcTransfer.h Verdict): TDs ended at a short
    // packet before their last TRB, their second events absorbed, stale events after a cancel, events for no request.
    struct Counts {
        uint64_t bytes[2], requests[2], failed[2], bounced[2], serial[2];
        PlanStats why;
        uint32_t events, shortEarly, second, stale, unexplained;
        uint64_t ticks;
    };

    using Why = PlanStats;

    Counts now, logged;
    uint64_t at, window;
    uint32_t interruptsLogged;
    uint64_t filterTicksLogged;
    uint32_t mostTrbs[2];  // the most TRBs one request took per direction since the last stats line
    uint32_t interrupts;   // ST.IP acknowledged by the filter (single-SPI writer)
    uint64_t filterTicks;

    static uint64_t clock()
    {
        return mach_absolute_time();
    }

    void filtered(uint64_t entered)
    {
        __atomic_store_n(&interrupts, __atomic_load_n(&interrupts, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&filterTicks, __atomic_load_n(&filterTicks, __ATOMIC_RELAXED) + (clock() - entered),
                         __ATOMIC_RELAXED);
    }

    void busy(uint64_t entered)
    {
        now.ticks += clock() - entered;
    }

    void event()
    {
        ++now.events;
    }

    void completed(bool in, uint32_t actual, bool failed)
    {
        const uint32_t d = in ? 1 : 0;
        now.bytes[d] += actual;
        ++now.requests[d];
        if (failed)
            ++now.failed[d];
    }

    void failed(bool in)
    {
        ++now.failed[in ? 1 : 0];
    }

    void queued(bool in, uint32_t trbs, const Why &why)
    {
        const uint32_t d = in ? 1 : 0;
        if (trbs > mostTrbs[d])
            mostTrbs[d] = trbs;
        now.bounced[d] += why.bounceBytes;
        now.why.discontinuity += why.discontinuity;
        now.why.boundary += why.boundary;
        now.why.high += why.high;
        now.why.edge += why.edge;
    }

    void serial(bool in)
    {
        ++now.serial[in ? 1 : 0];
    }

    void serialMoved(bool in, uint32_t bytes)
    {
        now.bounced[in ? 1 : 0] += bytes;
    }

    void shortEarly()
    {
        ++now.shortEarly;
    }

    void second()
    {
        ++now.second;
    }

    void stale()
    {
        ++now.stale;
    }

    void unexplained()
    {
        ++now.unexplained;
    }
};
#else
struct XudcStats {
    using Why = NoPlanStats;

    static uint64_t clock()
    {
        return 0;
    }

    void filtered(uint64_t)
    {}

    void busy(uint64_t)
    {}

    void event()
    {}

    void completed(bool, uint32_t, bool)
    {}

    void failed(bool)
    {}

    void queued(bool, uint32_t, const Why &)
    {}

    void serial(bool)
    {}

    void serialMoved(bool, uint32_t)
    {}

    void shortEarly()
    {}

    void second()
    {}

    void stale()
    {}

    void unexplained()
    {}
};
#endif

}  // namespace xudc
