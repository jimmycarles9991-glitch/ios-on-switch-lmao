// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offers, producers' filter writes and completions (also inside the frame-end action), their passes, boundaries
// anywhere, folded frame ends and SC7 in random order against switch_completion_flip_harness.h's Display. Every latch
// is read as the ring models it (the harness's checkFlip throws otherwise), and the ring's front is what window A
// scanned at some boundary up to each frame end. A filter writes only a frame whose completion is a success and only
// before that completion, as the GPU's and host1x's filters guarantee.
#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "switch_completion_flip_harness.h"

namespace completion_flip_test {

struct Walk {
    unsigned completed = 0, fromCompletions = 0, fromFilters = 0;
};

inline uint64_t gRng = 0x9e3779b97f4a7c15ull;

inline unsigned roll(unsigned n)
{
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return static_cast<unsigned>(gRng % n);
}

inline void randomWalk(Display &d, unsigned steps, Walk &walk)
{
    constexpr int kSlot = Display::kSlot0;
    std::vector<int> activeAt = {d.activeEntry()};
    unsigned pendingTicks = 0;
    bool offered = false, done = false, fails = false;
    const auto filter = [&] {
        if (!offered || done || fails)
            return;
        d.model.written.clear();
        if (roll(2)) {
            d.gpuFilter();
        } else {
            d.armWaiter(static_cast<uint32_t>(d.nextSwap));
            d.vicFilter();
        }
        walk.fromFilters += !d.model.written.empty();
    };
    const auto complete = [&] {
        if (offered && !done) {
            d.model.written.clear();
            d.complete(fails ? -5 : 0);
            walk.fromCompletions += !d.model.written.empty();
            done = true;
        }
    };
    const auto boundary = [&] {
        d.boundary();
        activeAt.push_back(d.activeEntry());
        ++pendingTicks;
    };
    for (unsigned step = 0; step < steps; ++step) {
        switch (roll(9)) {
        case 0:
        case 1: {
            // The kick's own rule: no direct offer while a frame is held or a replaced flip is unsettled.
            const int entry = kSlot + static_cast<int>(roll(4));
            if (!d.transferred && !d.hwSuspended && d.ring.held == Display::kNoBuffer &&
                d.ring.superseded == Display::kNoBuffer && !d.ring.busy(entry)) {
                offered = (roll(4) ? d.offer(entry) : d.offerCopy()) == 0;
                done = false;
                fails = roll(16) == 0;
            }
            break;
        }
        case 2:
            complete();
            break;
        case 3:
            d.donePass();
            break;
        case 4:
            if (!d.hwSuspended)
                boundary();
            break;
        case 5:
        case 6: {
            if (!pendingTicks || d.hwSuspended)
                break;
            const size_t seenAtStart = activeAt.size() - 1;
            if (roll(4) == 0)
                d.midTick = [&] {
                    if (roll(2))
                        filter();
                    else
                        complete();
                    if (roll(2))
                        boundary();
                };
            pendingTicks = 0;
            ASSERT_NO_THROW(d.tick()) << "step " << step;
            d.midTick = nullptr;
            bool caughtUp = false;
            for (size_t b = seenAtStart; b < activeAt.size(); ++b)
                caughtUp = caughtUp || activeAt[b] == d.ring.front;
            ASSERT_TRUE(caughtUp) << "step " << step << ": the ring's front " << d.ring.front
                                  << " is not what window A scanned at the frame end (" << activeAt[seenAtStart] << ")";
            break;
        }
        case 7:
            if (d.hwSuspended ? roll(4) == 0 : roll(512) == 0) {
                if (d.hwSuspended) {
                    d.resume();
                } else {
                    ASSERT_NO_THROW(d.suspend()) << "step " << step;
                    pendingTicks = 0;
                }
            }
            break;
        case 8:
            filter();
            break;
        }
        if (offered && done && !d.transferred) {
            offered = false;
            ++walk.completed;
        }
    }
}

}  // namespace completion_flip_test
