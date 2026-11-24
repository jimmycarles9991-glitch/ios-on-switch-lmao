// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraDisplayFlip/SwitchFrameTimeline.h"

class SwitchFrameTickets {
public:
    static constexpr unsigned kCapacity = 32;

    struct Ticket {
        const void *request = nullptr;
        uint64_t id = 0, take = 0, gpu = 0;
        // Requested presentation and the latch use continuous time; acceptance and render origin use absolute time.
        uint64_t accepted = 0, requested = 0, origin = 0, latched = 0;
        uint32_t swapId = 0;
    };
#if SWITCH_FRAME_TIMELINE
    struct Totals {
        uint64_t accepted = 0, shown = 0, unshown = 0, unknownOrigin = 0, unknownDeadline = 0;
        uint64_t missedDeadline = 0, overflow = 0;
    } totals;

    // Since the last take (the display's stats window): each shown swap's frame end minus its requested presentation, in
    // refresh periods rounded, from kLagEarliest or earlier to kLagEarliest + kLagBins - 1 or later; unknown, a swap
    // with no request.
    static constexpr int kLagEarliest = -2;
    static constexpr unsigned kLagBins = 7;

    struct Lag {
        uint32_t bins[kLagBins] = {};
        uint32_t unknown = 0;
    };

    void lagged(uint64_t requested, uint64_t shownAt, uint64_t period)
    {
        if (!requested || !period) {
            ++lag.unknown;
            return;
        }
        const int64_t p = static_cast<int64_t>(period);
        const int64_t late = static_cast<int64_t>(shownAt - requested);
        const int64_t periods = late >= 0 ? (late + p / 2) / p : -((p / 2 - late) / p);
        int64_t bin = periods - kLagEarliest;
        if (bin < 0)
            bin = 0;
        if (bin >= static_cast<int64_t>(kLagBins))
            bin = kLagBins - 1;
        ++lag.bins[bin];
    }

    Lag takeLag()
    {
        const Lag taken = lag;
        lag = {};
        return taken;
    }

    Ticket *find(const void *request)
    {
        for (auto &ticket : entries) {
            if (request && ticket.request == request)
                return &ticket;
        }
        return nullptr;
    }

    Ticket *byTake(uint64_t take)
    {
        for (auto &ticket : entries) {
            if (take && ticket.take == take)
                return &ticket;
        }
        return nullptr;
    }

    uint64_t accept(const void *request, uint32_t swapId, uint64_t requested, uint64_t accepted)
    {
        if (!request)
            return 0;
        if (const auto *existing = find(request))
            return existing->id;
        ++totals.accepted;
        for (auto &ticket : entries) {
            if (!ticket.request) {
                ticket = {request, ++next, 0, 0, accepted, requested, 0, 0, swapId};
                return ticket.id;
            }
        }
        ++totals.overflow;
        return 0;
    }

    void take(const void *request, uint64_t sequence)
    {
        if (auto *ticket = find(request))
            ticket->take = sequence;
    }

    void gpu(uint64_t sequence, uint64_t gpuSequence)
    {
        if (auto *ticket = byTake(sequence))
            ticket->gpu = gpuSequence;
    }

    Ticket shown(uint64_t sequence, uint64_t continuousStamp, uint64_t tolerance = 0, bool *first = nullptr)
    {
        if (first)
            *first = false;
        if (!sequence || sequence <= lastShown)
            return {};
        lastShown = sequence;
        if (first)
            *first = true;
        auto *ticket = byTake(sequence);
        ++totals.shown;
        if (!ticket || !ticket->origin)
            ++totals.unknownOrigin;
        if (!ticket || !ticket->requested)
            ++totals.unknownDeadline;
        else if (continuousStamp > ticket->requested && continuousStamp - ticket->requested > tolerance)
            ++totals.missedDeadline;
        if (!ticket)
            return {};
        ticket->latched = continuousStamp;
        return *ticket;
    }

    void retire(const void *request)
    {
        if (auto *ticket = find(request)) {
            if (!ticket->latched)
                ++totals.unshown;
            *ticket = {};
        }
    }

private:
    Ticket entries[kCapacity] = {};
    uint64_t next = 0, lastShown = 0;
    Lag lag;
#else
    Ticket *find(const void *)
    {
        return nullptr;
    }

    Ticket *byTake(uint64_t)
    {
        return nullptr;
    }

    uint64_t accept(const void *, uint32_t, uint64_t, uint64_t)
    {
        return 0;
    }

    void take(const void *, uint64_t)
    {}

    void gpu(uint64_t, uint64_t)
    {}

    Ticket shown(uint64_t, uint64_t, uint64_t = 0, bool *first = nullptr)
    {
        if (first)
            *first = false;
        return {};
    }

    void retire(const void *)
    {}
#endif
};
