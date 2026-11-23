// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace switch_display {
struct RetiredFrame {
    uint64_t sequence = 0;
    uint64_t shownAt = 0;
};

class Retirement {
public:
    void observe(uint64_t sequence, uint64_t shownAt)
    {
        if (!sequence || sequence <= retired)
            return;
        if (sequence > pending.sequence)
            pending.sequence = sequence;
        if (shownAt > pending.shownAt)
            pending.shownAt = shownAt;
    }

    RetiredFrame take(uint64_t oldestPending)
    {
        if (!pending.sequence || (oldestPending && oldestPending <= pending.sequence))
            return {};
        const auto ready = pending;
        retired = ready.sequence;
        pending = {};
        return ready;
    }

private:
    RetiredFrame pending;
    uint64_t retired = 0;
};

// The accepted swap (the family's swap id) the kick last left at the queue head, first deferred at frame end `since`.
// A kick at a later frame end that defers it again finds it held past a frame: reported once. Its hold ends when it is
// taken or when another swap is at the head (the family let it go): reported too, with how long it lasted.
class HeldHead {
public:
    struct Ended {
        uint32_t swap = 0;  // 0: no reported hold ended
        uint64_t ticks = 0;
        bool taken = false;
    };

    // True when this deferral finds `swap` held past a frame; `ended` gets a reported hold of an earlier head.
    bool defer(uint32_t swap, uint64_t frameEnd, uint64_t now, Ended &ended)
    {
        ended = settle(swap, false, now);
        if (swap != held) {
            held = swap;
            since = frameEnd;
            at = now;
        }
        if (reported || frameEnd == since)
            return false;
        reported = true;
        ++count;
        return true;
    }

    // `swap` was taken: the reported hold this ends.
    Ended take(uint32_t swap, uint64_t now)
    {
        return settle(swap, true, now);
    }

    bool recording() const
    {
        return held != 0;
    }

    uint32_t heldSinceBoot() const
    {
        return count;
    }

private:
    Ended settle(uint32_t swap, bool taken, uint64_t now)
    {
        Ended ended;
        if (!held || (swap == held && !taken))
            return ended;
        if (reported)
            ended = {held, now - at, swap == held};
        held = 0;
        reported = false;
        return ended;
    }

    uint32_t held = 0, count = 0;
    uint64_t since = 0, at = 0;
    bool reported = false;
};
}  // namespace switch_display
