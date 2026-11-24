// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: when the screen stops presenting in the middle of an animation, NativeLegacyDisplay has the kernel
// snapshot the threads the next frame waits on (kernel/switch/diag/stall_frame.c). A stall is a frame end kPeriods or
// more refresh periods after a present that itself came within 1.5 periods of the one before it (the screen was
// changing every frame and then stopped), with a swap accepted since that present still not shown. Core Animation
// registers no vsync client on iOS 17 or 27, so the display learns that it wants to draw only from its swap; a render
// thread that stops before swapping leaves nothing out, the frame end is masked, and no snapshot is taken. Once per
// stall; the next present re-arms it. Times are ticks of one monotonic clock.
#pragma once
#include <stdint.h>

class SwitchFrameStall {
public:
    static constexpr uint64_t kPeriods = 3;

    void present(uint64_t now, uint64_t interval, uint64_t period)
    {
        live = interval != 0 && interval * 2 <= period * 3;
        last = now;
        fired = false;
    }

    // True at the first frame end of a stall; lastAccepted, when the display last accepted a swap.
    bool frameEnd(uint64_t now, uint64_t period, uint64_t lastAccepted)
    {
        if (fired || !live || lastAccepted <= last || now < last || now - last < kPeriods * period)
            return false;
        fired = true;
        return true;
    }

    uint64_t lastPresent() const
    {
        return last;
    }

private:
    uint64_t last = 0;
    bool live = false, fired = false;
};
