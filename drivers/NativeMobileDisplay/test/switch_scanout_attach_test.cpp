// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchScanoutAttach.h against a model of windows B and C: a window firmware left on is turned off and the scanout
// domain attaches at the first frame end that reads both off with no request out, never while one may still fetch.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace native_planes {
constexpr uint32_t kOptions = 0;
}  // namespace native_planes

namespace {

unsigned gLogs = 0;

void IOLog(const char *, ...)
{
    ++gLogs;
}

struct Display {
    static constexpr uint32_t kWinBBase = 0xd80, kWinCBase = 0xf80, kDcStateControl = 0x41;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kWinBUpdate = 1 << 10, kWinCUpdate = 1 << 11;
    static constexpr uint32_t kGeneralActReq = 1, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr unsigned kLatchFrames = 4;

    std::array<uint32_t, 4096> registers = {};
    uint32_t *dc = registers.data();
    bool activeB = false, activeC = false;  // the windows' active OPTIONS enable bits
    bool hwSuspended = false;
    uint64_t frameEnds = 10;
    unsigned attaches = 0, requests = 0;

    bool otherWindowEnabled() const
    {
        return activeB || activeC;
    }

    void attachScanoutDomain()
    {
        EXPECT_FALSE(otherWindowEnabled()) << "attached while a window may fetch";
        ++attaches;
    }

    // A frame boundary: an outstanding request makes the assembly OPTIONS active.
    void boundary(bool takesRequest = true)
    {
        if (takesRequest && (dc[kDcStateControl] & (kWinBActReq | kWinCActReq))) {
            activeB = dc[kWinBBase] & 1;
            activeC = dc[kWinCBase] & 1;
            dc[kDcStateControl] = 0;
            ++requests;
        }
        ++frameEnds;
        scanoutAttachFrameEnd();
    }

#include "../SwitchScanoutAttach.h"
};

TEST(ScanoutAttach, AFirmwareWindowIsTurnedOffAndTheDomainAttachesOnceItReadsOff)
{
    Display d;
    d.activeB = true;
    d.registers[Display::kWinBBase] = 1;
    d.scanoutAttachWaits = true;
    d.stopFirmwareWindows();
    EXPECT_EQ(d.registers[Display::kWinBBase], 0u);
    EXPECT_EQ(d.attaches, 0u);
    d.boundary();
    EXPECT_FALSE(d.activeB);
    EXPECT_EQ(d.attaches, 1u);
    EXPECT_FALSE(d.scanoutAttachWaits);
    d.boundary();
    EXPECT_EQ(d.attaches, 1u);
}

// The readback's bound passed: one line, no request written again, frame ends no longer held for it, and the attach
// still waits for a frame end that reads the window off.
TEST(ScanoutAttach, AWindowThatDoesNotStopIsOneLineAndTheDomainWaitsWithoutAskingAgain)
{
    Display d;
    d.activeC = true;
    d.scanoutAttachWaits = true;
    d.stopFirmwareWindows();
    EXPECT_TRUE(d.scanoutAttachHoldsFrameEnds());
    gLogs = 0;
    for (int i = 0; i < 12; ++i)
        d.boundary(false);
    EXPECT_EQ(gLogs, 1u);
    EXPECT_EQ(d.scanoutAttachAskedAt, 10u) << "no request written again";
    EXPECT_FALSE(d.scanoutAttachHoldsFrameEnds()) << "a window that does not stop keeps no frame end unmasked";
    EXPECT_EQ(d.attaches, 0u);
    d.boundary();
    EXPECT_EQ(d.attaches, 1u);
}

}  // namespace
