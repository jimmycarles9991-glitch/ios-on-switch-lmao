// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <stdexcept>
#include "../NativePlanes.h"

namespace {
using event_t = void *;
constexpr int THREAD_UNINT = 0, THREAD_AWAKENED = 1, THREAD_TIMED_OUT = 2, THREAD_CONTINUE_NULL = 0;
constexpr unsigned kMillisecondScale = 1000000;
std::function<void()> onAssert, onBlock;
unsigned registrations, blocks, clears;
int blockResult;

void clock_interval_to_deadline(unsigned, unsigned, uint64_t *deadline)
{
    *deadline = 100000;
}

void assert_wait_deadline(event_t, int, uint64_t)
{
    ++registrations;
    if (onAssert)
        onAssert();
}

int current_thread()
{
    return 1;
}

void clear_wait(int, int)
{
    ++clears;
}

int thread_block(int)
{
    ++blocks;
    if (onBlock)
        onBlock();
    return blockResult;
}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("unsafe scanout release");
}

struct Display {
    static constexpr uint32_t kDcStateAccess = 0x40, kDcStateControl = 0x41, kDcWindowHeader = 0x42;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWindowASelect = 1;
    static constexpr uint32_t kDcDisplayCommand = 0x32, kDcModeMask = 3;
    static constexpr uint32_t kWinbufStartAddr = 0x800, kWinOptions = 0x700, kWinLineStride = 0x70a;
    static constexpr uint32_t kWinbufSurfaceKind = 0x80b, kWinbufHOffset = 0x806;
    static constexpr uint32_t kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10,
                              kWinCUpdate = 1 << 11;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr unsigned kFlipDeadlineMS = 100;
    std::array<uint32_t, 4096> registers = {};
    uint32_t *dc = registers.data();
    uint32_t pitchOptions = 1, pitchStride = 2880, pitchKind = 0, pitchHOffset = 0;
    bool scanoutStopped = false, hwSuspended = false;
    unsigned restores = 0, unmasks = 0;
    bool flagBeforeRequest = false;

    uint32_t surfaceAddress(int)
    {
        return 0x98000000;
    }

    void restorePitchWindow()
    {
        ++restores;
        flagBeforeRequest = scanoutQuiescing;
    }

    void scanoutMemoryBarrier()
    {}

    void unmaskFrameEnd()
    {
        ++unmasks;
    }

#include "../SwitchScanoutRetirement.h"

    Display()
    {
        dc[kDcDisplayCommand] = 1;
        dc[kWinbufStartAddr] = 0x10000000;
    }

    void takePitch()
    {
        dc[kWinbufStartAddr] = surfaceAddress(0);
        dc[kWinOptions] = pitchOptions;
        dc[kWinLineStride] = pitchStride;
        dc[kWinbufSurfaceKind] = pitchKind;
        dc[kWinbufHOffset] = pitchHOffset;
        dc[kWinBBase + native_planes::kOptions] = dc[kWinCBase + native_planes::kOptions] = 0;
        dc[kDcStateControl] = 0;
        ++scanoutStopFrames;
    }
};

class ScanoutRetirement : public testing::Test {
protected:
    void SetUp() override
    {
        onAssert = onBlock = {};
        registrations = blocks = clears = 0;
        blockResult = THREAD_AWAKENED;
    }
};

TEST_F(ScanoutRetirement, WaitsForInterruptBeforeStorageCanBeReleased)
{
    Display d;
    onBlock = [&] {
        d.takePitch();
    };
    d.retireScanoutStorage();
    EXPECT_EQ(blocks, 1u);
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(d.restores, 1u);
    EXPECT_EQ(d.unmasks, 1u);
    EXPECT_TRUE(d.flagBeforeRequest);
    EXPECT_FALSE(d.scanoutQuiescing);
    EXPECT_TRUE(d.scanoutStorageRetired());
}

TEST_F(ScanoutRetirement, InterruptDuringWaitRegistrationCannotBeLost)
{
    Display d;
    onAssert = [&] {
        d.takePitch();
    };
    d.retireScanoutStorage();
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(clears, 1u);
    EXPECT_EQ(blocks, 0u);
}

TEST_F(ScanoutRetirement, UnrelatedFrameWakeCannotAuthorizeRelease)
{
    Display d;
    onBlock = [&] {
        if (blocks == 1)
            ++d.scanoutStopFrames;
        else
            d.takePitch();
    };
    d.retireScanoutStorage();
    EXPECT_EQ(blocks, 2u);
    EXPECT_TRUE(d.scanoutStorageRetired());
}

TEST_F(ScanoutRetirement, FullPitchIdentityBothOverlaysAndRequestBitsAreRequired)
{
    Display d;
    d.takePitch();
    EXPECT_TRUE(d.scanoutStorageRetired());
    const unsigned offsets[] = {Display::kWinbufStartAddr, Display::kWinOptions, Display::kWinLineStride,
                                Display::kWinbufSurfaceKind, Display::kWinbufHOffset};
    for (unsigned offset : offsets) {
        const auto value = d.dc[offset];
        d.dc[offset] = value + 1;
        EXPECT_FALSE(d.scanoutStorageRetired());
        d.dc[offset] = value;
    }
    for (unsigned bank : {Display::kWinBBase, Display::kWinCBase}) {
        d.dc[bank + native_planes::kOptions] = native_planes::kWinEnable;
        EXPECT_FALSE(d.scanoutStorageRetired());
        d.dc[bank + native_planes::kOptions] = 0;
    }
    for (unsigned request : {Display::kWinAActReq, Display::kWinBActReq, Display::kWinCActReq}) {
        d.dc[Display::kDcStateControl] = request;
        EXPECT_FALSE(d.scanoutStorageRetired());
    }
}

TEST_F(ScanoutRetirement, TimeoutRetainsQuiescingStateAndCannotReturnSuccess)
{
    Display d;
    blockResult = THREAD_TIMED_OUT;
    EXPECT_THROW(d.retireScanoutStorage(), std::runtime_error);
    EXPECT_TRUE(d.scanoutQuiescing);
    EXPECT_EQ(blocks, 1u);
}

TEST_F(ScanoutRetirement, FinalLatchAtTimeoutIsAccepted)
{
    Display d;
    blockResult = THREAD_TIMED_OUT;
    onBlock = [&] {
        d.takePitch();
    };
    EXPECT_NO_THROW(d.retireScanoutStorage());
    EXPECT_FALSE(d.scanoutQuiescing);
}

TEST_F(ScanoutRetirement, ActualStoppedControllerNeedsNoFutureFrameInterrupt)
{
    Display d;
    d.scanoutStopped = true;
    d.dc[Display::kDcDisplayCommand] = 0;
    d.retireScanoutStorage();
    EXPECT_EQ(blocks, 0u);
    EXPECT_EQ(d.restores, 1u);
    d.dc[Display::kDcDisplayCommand] = 1;
    EXPECT_FALSE(d.scanoutStorageRetired());
}

TEST_F(ScanoutRetirement, PoweredOffControllerIsNeverReadOrWritten)
{
    Display d;
    d.hwSuspended = true;
    EXPECT_THROW(d.retireScanoutStorage(), std::runtime_error);
    EXPECT_EQ(d.restores, 0u);
    EXPECT_EQ(d.unmasks, 0u);
    EXPECT_EQ(registrations, 0u);
}
}
