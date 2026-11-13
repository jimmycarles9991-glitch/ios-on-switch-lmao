// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: how the screen kept up during one touch interaction, for NativeLegacyDisplay's "SwitchFrames
// interaction" line (tools/frames reads it). A window opens at the first present after a run of touches began and
// closes kTailUs after the run's last contact, so a fling's deceleration, an app's launch animation or a panel settling
// after the finger lifts are inside it. Only intervals between two presents of the window count: the first present
// answers the touch-down and its interval spans the time before it. Times are microseconds of one monotonic clock. The
// including file declares snprintf.
#pragma once
#include <stddef.h>
#include <stdint.h>

class SwitchInteraction {
public:
    static constexpr uint64_t kTailUs = 1000000;
    static constexpr uint64_t kStillUs = 250000;  // the display's kGapUs: a longer interval is a screen that went still
    static constexpr unsigned kCauses = 6;        // NativeLegacyDisplay's long-present causes, in its order

    // Whether a window is open at nowUs for a run of touches that began at beganUs with its last contact at lastDownUs
    // (0: no touch yet).
    static bool active(uint64_t nowUs, uint64_t beganUs, uint64_t lastDownUs)
    {
        return beganUs != 0 && lastDownUs != 0 && nowUs >= beganUs && nowUs - lastDownUs < kTailUs;
    }

    // A present at nowUs, `periods` refresh periods after the previous present (`intervalUs`), charged to `cause` when
    // two or more. Call close() first when the window ended.
    void present(uint64_t nowUs, uint64_t intervalUs, uint32_t periods, unsigned cause, uint64_t beganUs)
    {
        if (!open) {
            *this = SwitchInteraction{};
            open = true;
            began = beganUs;
            last = nowUs;
            presents = 1;
            return;
        }
        ++presents;
        last = nowUs;
        if (intervalUs >= kStillUs)
            ++still;
        uint32_t bin = 3;
        if (periods <= 1) {
            bin = 0;
        } else if (periods < 4) {
            bin = periods - 1;
        }
        ++bins[bin];
        if (periods >= 2) {
            lost += periods - 1;
            if (cause < kCauses)
                ++causes[cause];
        }
        if (intervalUs > worstUs)
            worstUs = intervalUs;
    }

    bool isOpen() const
    {
        return open;
    }

    // The window's line into out, given its run's last contact, and the window closed. Returns the length.
    size_t close(uint64_t lastDownUs, char *out, size_t size)
    {
        open = false;
        if (!size)
            return 0;
        const int n = snprintf(
            out, size,
            "%llu ms (touch %llu ms), %u presents: 1:%u 2:%u 3:%u 4+:%u, %llu periods lost, worst %llu.%llu ms, "
            "still %u; long idle %u ca %u gpu %u queue %u latch %u rate %u",
            static_cast<unsigned long long>((last - began) / 1000),
            static_cast<unsigned long long>((lastDownUs - began) / 1000), presents, bins[0], bins[1], bins[2], bins[3],
            static_cast<unsigned long long>(lost), static_cast<unsigned long long>(worstUs / 1000),
            static_cast<unsigned long long>(worstUs / 100 % 10), still, causes[0], causes[1], causes[2], causes[3],
            causes[4], causes[5]);
        if (n < 0)
            return 0;
        const size_t length = static_cast<size_t>(n);
        return length < size ? length : size - 1;
    }

private:
    bool open = false;
    uint64_t began = 0, last = 0, worstUs = 0, lost = 0;
    uint32_t presents = 0, still = 0, bins[4] = {}, causes[kCauses] = {};
};
