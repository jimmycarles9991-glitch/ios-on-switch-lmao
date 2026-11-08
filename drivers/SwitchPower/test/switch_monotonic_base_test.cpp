// SwitchMonotonicBase.h: kern.monotonicclock carries on across boots the way J120's PMU clock does, so timed keeps its
// cached UTC instead of setting the time from a filesystem timestamp.
#include "drivers/SwitchPower/SwitchMonotonicBase.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr uint64_t kSec = 1000000;
constexpr int64_t k2026 = 1794077434;  // 2026-11-07, the RTC in the 19:12 boot

// One boot as the clock sees it: the MAX77620 read at `rtcExact` (its registers truncate to the second), the Tegra RTC
// at `tegra` and mach time at `mach` when the clock first latches, `sinceRead` after that read.
struct Boot {
    double rtcExact;
    uint64_t tegra;
    uint64_t mach;
    uint64_t sinceRead = 16000;

    uint64_t Base() const
    {
        return switch_monotonic_base_usec(static_cast<int64_t>(rtcExact), tegra, sinceRead, mach);
    }

    // The clock `after` microseconds past the first latch, as kern.monotonicclock_usecs reads it.
    uint64_t At(uint64_t after) const
    {
        return tegra + after + Base();
    }
};

TEST(SwitchMonotonicBase, StartsAtTheRtcSecondsSince2000)
{
    const Boot boot{static_cast<double>(k2026), 1295482000, 1000000};
    EXPECT_EQ(boot.At(0) - boot.sinceRead, static_cast<uint64_t>(k2026 - SWITCH_RTC_EPOCH_SECS) * kSec);
}

TEST(SwitchMonotonicBase, ARebootNeverReadsBelowThePreviousBoot)
{
    // Boot A runs ten minutes from a Tegra RTC that had counted 21 minutes; boot B comes up after a cold boot (the
    // Tegra RTC from zero), a warm one (it kept counting) or a long time off, 3 s to a day after A's last reading.
    const Boot a{k2026 + 0.999, 1295482000, 900000};
    const uint64_t aLast = a.At(600 * kSec);
    for (const double gap : {3.0, 60.0, 86400.0}) {
        const double bRtc = k2026 + 0.999 + 600 + gap + 0.016;
        for (const uint64_t bTegra : {uint64_t{3000000}, uint64_t{1295482000 + 620 * kSec}}) {
            const Boot b{bRtc, bTegra, 1100000};
            EXPECT_GT(b.At(0), aLast) << "gap " << gap << " s, Tegra RTC " << bTegra;
        }
    }
}

TEST(SwitchMonotonicBase, TheCalendarOffsetIsTheEpochNotZero)
{
    const Boot boot{static_cast<double>(k2026), 40000000, 1000000};
    const uint64_t utc = static_cast<uint64_t>(k2026) * kSec + boot.sinceRead;
    EXPECT_EQ(static_cast<int64_t>(utc - boot.At(0)), SWITCH_RTC_EPOCH_SECS * static_cast<int64_t>(kSec));
}

TEST(SwitchMonotonicBase, StaysAboveMachTime)
{
    const Boot boot{static_cast<double>(k2026), 0, 30 * kSec};
    EXPECT_GT(boot.At(0), boot.mach);
}

TEST(SwitchMonotonicBase, WithoutTheMax77620AsBefore)
{
    EXPECT_EQ(switch_monotonic_base_usec(-1, 1295482000, 0, 1000000), 0u);
    EXPECT_EQ(switch_monotonic_base_usec(-1, 500000, 0, 2000000), 2000000u - 500000u + kSec);
    EXPECT_EQ(switch_monotonic_base_usec(0, 500000, 0, 2000000), 2000000u - 500000u + kSec);
}

}  // namespace
