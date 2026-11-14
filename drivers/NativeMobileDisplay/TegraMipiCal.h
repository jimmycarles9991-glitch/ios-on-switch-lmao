// SPDX-License-Identifier: BSD-2-Clause-Patent
// The DSI pads' MIPI calibration, once its configuration is written (TRM 30.1.3; registers 30.2.1-30.2.4, word offsets):
// one STARTCAL, then the calibration is done once MIPI_CAL_ACTIVE reads 0 with MIPI_AUTO_CAL_DONE set for every selected
// brick and the clock lane's MIPI_CLK_AUTO_CAL_DONE_DSIA set (the only clock lane the configuration selects). No interrupt
// reports it, so the status is read each millisecond for at most 10 ms: hekate's settle after its calibration (the TRM
// gives no time). Plain logic over a register accessor, no IOKit.
#pragma once
#include <stdint.h>

namespace tegra_mipi_cal {

enum : uint32_t {
    kCtrl = 0x00,
    kStatus = 0x02,
    kStatus2 = 0x03,
};

constexpr uint32_t kActive = 1u << 0, kDone = 1u << 16, kClockDoneDsia = 1u << 3;
constexpr uint32_t kStart = 0x2A000001;  // NOISE_FLT 0xa, PRESCALE 1.0 us steps, STARTCAL
constexpr unsigned kBoundMs = 10;

template <class Words> bool done(Words &cal)
{
    const uint32_t status = cal[kStatus];
    return !(status & kActive) && (status & kDone) && (cal[kStatus2] & kClockDoneDsia);
}

// The clock lane's done bit is write-1-to-clear like the status word the configuration clears. True once done; `waitedMs`
// says how long it took, or the bound.
template <class Words, class Sleep> bool calibrate(Words &cal, Sleep sleepMs, unsigned &waitedMs)
{
    cal[kStatus2] = kClockDoneDsia;
    cal[kCtrl] = kStart;
    for (waitedMs = 0; !done(cal) && waitedMs < kBoundMs; ++waitedMs)
        sleepMs();
    return done(cal);
}

}  // namespace tegra_mipi_cal
