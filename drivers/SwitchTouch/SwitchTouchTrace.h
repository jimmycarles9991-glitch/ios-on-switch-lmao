// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels only: a contact's raw samples, logged when it lifts, so SwitchTouchFilter.h's constants can be checked
// against what the FTM4 reports for real taps, flicks and held fingers. The first kHead samples and the last kTail
// (a pause before a lift, and the lift itself).
#pragma once
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <stdint.h>

struct SwitchTouchTrace {
    static constexpr unsigned kHead = 24, kTail = 40, kPerLine = 16;

    struct Sample {
        uint16_t ms;
        int16_t dx, dy;
        uint16_t z;
    };

    Sample head[kHead], tail[kTail];
    unsigned count;

    void start(unsigned z)
    {
        count = 0;
        add(0, 0, 0, z);
    }

    void add(unsigned ms, int dx, int dy, unsigned z)
    {
        const Sample s = {static_cast<uint16_t>(ms > 0xffff ? 0xffff : ms), static_cast<int16_t>(dx),
                          static_cast<int16_t>(dy), static_cast<uint16_t>(z > 0xffff ? 0xffff : z)};
        if (count < kHead) {
            head[count] = s;
        } else {
            tail[(count - kHead) % kTail] = s;
        }
        ++count;
    }

    const Sample &at(unsigned i) const
    {
        return i < kHead ? head[i] : tail[(i - kHead) % kTail];
    }

    // Report-frame pixels from the first point; z as SwitchTouch.cpp strength(); `reported` where it lifted.
    void log(unsigned contact, int x, int y, unsigned ms, int reportedDx, int reportedDy) const
    {
        const unsigned skipped = count > kHead + kTail ? count - kHead - kTail : 0;
        const unsigned lines = (count - skipped + kPerLine - 1) / kPerLine;
        unsigned i = 0;
        unsigned line = 0;
        while (i < count) {
            char text[kPerLine * 20 + 1];
            size_t at = 0;
            for (unsigned n = 0; n < kPerLine && i < count; ++n) {
                const Sample &s = this->at(i);
                at +=
                    static_cast<size_t>(snprintf(text + at, sizeof(text) - at, " %u:%d,%d,%u", s.ms, s.dx, s.dy, s.z));
                if (at >= sizeof(text))
                    at = sizeof(text) - 1;
                i = i + 1 == kHead && skipped ? kHead + skipped : i + 1;
            }
            ++line;  // not inside os_log: its arguments are evaluated twice
            os_log(OS_LOG_DEFAULT,
                   "SwitchTouch trace: contact %u at (%d,%d) %u ms, %u samples (%u not kept), lifted at %+d,%+d; "
                   "%u/%u%s",
                   contact, x, y, ms, count, skipped, reportedDx, reportedDy, line, lines, text);
        }
    }
};
