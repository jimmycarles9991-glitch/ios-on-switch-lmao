// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../TegraMipiCal.h"

namespace {
// MIPI_CAL as the TRM has it (30.2.1-30.2.4): STARTCAL starts the state machine, which reads ACTIVE until it finishes
// `steps` milliseconds later with AUTO_CAL_DONE and, if `clockLane`, the clock lane's done bit; the status words are
// write-1-to-clear.
struct MipiCal {
    unsigned steps = 2, starts = 0;
    bool clockLane = true, running = false;
    uint32_t status = 0, status2 = 0, ctrl = 0;
    std::vector<uint32_t> writes;

    struct Word {
        MipiCal &cal;
        uint32_t index;

        operator uint32_t() const
        {
            return index == tegra_mipi_cal::kStatus    ? cal.status
                   : index == tegra_mipi_cal::kStatus2 ? cal.status2
                                                       : cal.ctrl;
        }

        Word &operator=(uint32_t value)
        {
            cal.writes.push_back(index);
            if (index == tegra_mipi_cal::kStatus) {
                cal.status &= ~(value & ~tegra_mipi_cal::kActive);
            } else if (index == tegra_mipi_cal::kStatus2) {
                cal.status2 &= ~value;
            } else {
                cal.ctrl = value & ~1u;
                if (value & 1u) {
                    ++cal.starts;
                    cal.running = true;
                    cal.status |= tegra_mipi_cal::kActive;
                }
            }
            return *this;
        }
    };

    Word operator[](uint32_t index)
    {
        return {*this, index};
    }

    void millisecond()
    {
        if (!running || !steps || --steps)
            return;
        running = false;
        status = (status & ~tegra_mipi_cal::kActive) | tegra_mipi_cal::kDone;
        if (clockLane)
            status2 |= tegra_mipi_cal::kClockDoneDsia;
    }
};

TEST(TegraMipiCal, OneStartWaitsForEveryDoneBit)
{
    MipiCal cal;
    cal.status2 = tegra_mipi_cal::kClockDoneDsia;  // left from an earlier calibration
    cal.steps = 3;
    unsigned waited = 99;
    EXPECT_TRUE(tegra_mipi_cal::calibrate(
        cal,
        [&] {
            cal.millisecond();
        },
        waited));
    EXPECT_EQ(cal.starts, 1u);
    EXPECT_EQ(waited, 3u) << "done once the state machine finished, not before and not a fixed time";
    EXPECT_EQ(cal.ctrl, tegra_mipi_cal::kStart & ~1u);
    ASSERT_GE(cal.writes.size(), 2u);
    EXPECT_EQ(cal.writes[0], uint32_t(tegra_mipi_cal::kStatus2)) << "a stale clock-lane done bit is cleared first";
}

// A calibration that never reports the clock lane done is waited for 10 ms, then reported: never started again.
TEST(TegraMipiCal, AClockLaneNeverDoneIsBoundedAndNotStartedAgain)
{
    MipiCal cal;
    cal.clockLane = false;
    unsigned waited = 0, sleeps = 0;
    EXPECT_FALSE(tegra_mipi_cal::calibrate(
        cal,
        [&] {
            ++sleeps;
            cal.millisecond();
        },
        waited));
    EXPECT_EQ(waited, tegra_mipi_cal::kBoundMs);
    EXPECT_EQ(sleeps, tegra_mipi_cal::kBoundMs);
    EXPECT_EQ(cal.starts, 1u);
}

TEST(TegraMipiCal, StillActiveIsNotDone)
{
    MipiCal cal;
    cal.steps = 0;
    unsigned waited = 0;
    EXPECT_FALSE(tegra_mipi_cal::calibrate(
        cal,
        [&] {
            cal.millisecond();
        },
        waited));
    EXPECT_TRUE(cal.status & tegra_mipi_cal::kActive);
    EXPECT_EQ(cal.starts, 1u);
}
}  // namespace
