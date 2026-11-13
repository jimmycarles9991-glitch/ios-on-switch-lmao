// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchLandscapeDisplay.h's change of mode against DC A's register file: held until a quiet frame end, then the UI's
// registers onto the other window and the overlays onto theirs in one request for all three windows; the latch
// verified on the UI window; the exits that cannot wait.
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include "../SwitchDisplayComposition.h"
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchLandscapeWindows.h"
#include "../SwitchLandscape.h"

namespace {
int switch_emc_display_memory_ready()
{
    return 1;
}

int reserveError;

int switch_emc_display_memory_reserve(const switch_display_memory_head *)
{
    return reserveError;
}

void IOLog(const char *, ...)
{}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("display panic");
}

using SwitchMMIOWords = uint32_t *;
using namespace native_planes;

struct Display {
    bool entryPlaneSet[4] = {};
    using PlaneSet = switch_display::Composition;
    using Window = switch_landscape::Window;
    using Commits = switch_display::Compositions<PlaneSet>;
    static constexpr uint32_t kWinABase = 0xb80, kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10, kWinCUpdate = 1 << 11;
    static constexpr uint32_t kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kGeneralActReq = 1;
    static constexpr uint32_t kWinBActHcntrSel = 1 << 4, kWinCActHcntrSel = 1 << 6;
    static constexpr uint32_t kWinBUnderflowInt = 1 << 9, kWinCUnderflowInt = 1 << 10;
    static constexpr uint32_t kDcRegActControl = 0x43, kDcStateControl = 0x41;
    static constexpr uint32_t kDcIntEnable = 0x39, kDcIntStatus = 0x37;
    static constexpr uint32_t kDcStateAccess = 0x40, kReadMuxActive = 1, kWriteMuxActive = 4;
    static constexpr uint32_t kWinOptions = 0x700, kWinbufStartAddr = 0x800, kWinEnable = 1u << 30;
    static constexpr int kNoBuffer = -1;
    static constexpr unsigned kLatchFrames = 4;
    std::array<uint32_t, 4096> registers = {};
    uint32_t *dc = registers.data();

    struct Slot {
        uint64_t low = 0;
    } planeSlots[8];

    PlaneSet sets[Commits::kSlots + 1] = {};
    Commits planeLatch;
    PlaneSet planeTarget = {}, planeHeld = {};
    bool planeHeldSet = false, planeBlending = false, planeFiltersWritten[2] = {};
    bool stats = false, gpuScanout = false, planesOn = true;
    int planeCsc[2][2] = {{-1, -1}, {-1, -1}};
    uint32_t planeBlendSaved[3] = {0x00ff0000u | 255, 0, 0}, pitchActControl = UINT32_MAX;
    uint64_t planeRequestedAt = 0, frameEnds = 10;
    unsigned stPlaneFlips = 0, stPlaneDropped = 0, deadlines = 0, planeUnderflowRun = 0, kicks = 0, withdrawals = 0,
             mediaFromGate = 0;
    uint64_t retiredUnshown = 0;

    struct Ring {
        int front = 0, pending = kNoBuffer, drawing = kNoBuffer, held = kNoBuffer, superseded = kNoBuffer;
    } ring;

    struct Probe {
        bool on = false;

        bool active() const
        {
            return on;
        }
    } probe, planeProbe;

    struct Panic {
        bool panicking = false, landscape = false;
        uint32_t uiDepth = 0;
    } gPanicRestore;

    bool transferred = false, copyQueued = false, planeFencePending = false, probeRequest = false;
    bool tapeAttached = false, tapeSettle = false, hwSuspended = false, scanoutStopped = false, kickDeferred = false;

    Display()
    {
        landscapeState = {};
        reserveError = 0;
        planeLatch.values = sets;
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        EXPECT_TRUE(displayMemory.configure(mode, 720, 1280, false));
        for (unsigned i = 0; i < 3; ++i)
            dc[kWinABase + kBlendLayer + i] = planeBlendSaved[i];
        ui(kWinABase);
    }

    // The UI's block-linear portrait frame as window A scans it (hekate's pitch window with the block layout).
    void ui(uint32_t base)
    {
        dc[base + kColorDepth] = kDepthB8G8R8A8;
        dc[base + kSize] = 1280u << 16 | 720;
        dc[base + kPrescaled] = 1280u << 16 | 2880;
        dc[base + kDdaIncrement] = 0x10001000;
        dc[base + kLineStride] = 3072;
        dc[base + 0x40 + kStartAddr] = frontAddress;
        dc[base + 0x40 + kSurfaceKind] = 2 | 4 << 4;
        dc[base + kOptions] = kWinEnable;
    }

    static constexpr uint32_t frontAddress = 0xc0000000;

    uint32_t expectedStart() const
    {
        return frontAddress;
    }

    uint32_t uiActive(uint32_t index) const
    {
        const uint32_t base = landscapeOn() ? kWinBBase : kWinABase;
        return dc[index >= 0x800 ? base + 0x40 + (index - 0x800) : base + (index - 0x700)];
    }

    char uiName() const
    {
        return landscapeOn() ? 'B' : 'A';
    }

    static bool columnScanBusy()
    {
        return false;
    }

    static bool windowProbeHolds()
    {
        return false;
    }

    void withdrawCompletionFlip()
    {
        ++withdrawals;
    }

    void mediaFlipFromGate()
    {
        ++mediaFromGate;
    }

    static bool mediaFlipIdle()
    {
        return true;
    }

    static bool planeWindowIdle()
    {
        return false;
    }

    void scanoutMemoryBarrier()
    {}

    void planeReleaseIdle()
    {}

    void chainPanicHook()
    {}

    void armFlipDeadline()
    {
        ++deadlines;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    void retireUnshown(uint64_t seq)
    {
        retiredUnshown = seq;
    }

    const void *firstUntaken() const
    {
        return nullptr;
    }

    void getDisplayDimensions(uint32_t *width, uint32_t *height)
    {
        *width = 720;
        *height = 1280;
    }

    uint32_t planeSlotIova(int slot) const
    {
        return 0x10000000 + uint32_t(slot) * 0x100000;
    }

    PlaneSet &planeSetOf(int id)
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    const PlaneSet &planeSetOf(int id) const
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

#include "../SwitchLandscapeState.h"
#include "../SwitchDisplayBandwidthDisplay.h"
#include "../SwitchOverlayRegisters.h"
#include "../SwitchPlaneAdmissionDrop.h"
#include "../SwitchCompositionRegisters.h"
#include "../SwitchOverlayRecovery.h"
#include "../SwitchLandscapeDisplay.h"

    bool stage(const PlaneSet &p)
    {
        uint32_t update = 0, request = 0;
        if (!planeStage(p, update, request))
            return false;
        dc[kDcStateControl] = update;
        dc[kDcStateControl] = request;
        return true;
    }

    // A frame boundary takes every request; the frame end's latch of the overlays (planeFrameEnd's).
    bool boundary()
    {
        dc[kDcStateControl] = 0;
        ++frameEnds;
        if (!planeLatch.requested() || !planeLatch.latched(false, planeScannedSet()))
            return false;
        landscapeLatched();
        return true;
    }
};

Display::LandscapeState Display::landscapeState;

Display::PlaneSet portraitVideo(uint64_t seq, int slot = 2)
{
    Display::PlaneSet p = {};
    p.seq = seq;
    p.window[0].on = true;
    p.window[0].slot = slot;
    p.window[0].depth = 1;
    p.window[0].image.visible = p.window[0].image.yuv = true;
    p.window[0].image.matrix = kBt709;
    p.window[0].image.colorDepth = kDepthYCbCr422SP;
    p.window[0].image.options = kWinEnable | kCscEnable;
    p.window[0].image.size = 1280u << 16 | 720;
    p.window[0].image.prescaled = 1280u << 16 | 720;
    p.window[0].image.dda = 0x10001000;
    p.window[0].image.lineStride = 768u << 16 | 768;
    return p;
}

Display::PlaneSet landscapeVideo(uint64_t seq, int slot = 1)
{
    auto p = portraitVideo(seq, slot);
    p.landscape = true;
    auto &m = p.window[0].image;
    m.colorDepth = kDepthYCbCr420SP;
    m.options = kWinEnable | kCscEnable | switch_landscape::kScanColumn | kHFilterOn | kVFilterOn;
    m.prescaled = 1920u << 16 | 1080;
    m.dda = 6144u << 16 | 6144;
    m.surfaceKind = 2 | 1 << 4;
    m.lineStride = 2048u << 16 | 2048;
    m.start[1] = 2048u * 1088;
    return p;
}

// The windows in portrait with the VIC-turned copy of the video on B, latched.
void portrait(Display &d)
{
    ASSERT_TRUE(d.stage(portraitVideo(1)));
    ASSERT_TRUE(d.boundary());
    ASSERT_FALSE(d.landscapeOn());
}

void landscape(Display &d)
{
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.landscapeFrameEnd();
    ASSERT_TRUE(d.boundary());
    ASSERT_TRUE(d.landscapeOn());
}

TEST(LandscapeDisplay, ADifferentModeIsHeldAndTheKicksStop)
{
    Display d;
    portrait(d);
    const uint32_t bOptions = d.dc[Display::kWinBBase + kOptions];
    d.dc[Display::kDcStateControl] = 0;
    EXPECT_FALSE(d.stage(landscapeVideo(2)));
    EXPECT_TRUE(d.landscapeState.wanted);
    EXPECT_TRUE(d.landscapeHoldsKicks());
    EXPECT_TRUE(d.planeTarget.landscape);
    EXPECT_EQ(d.dc[Display::kDcStateControl], 0u);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], bOptions);
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], Display::kWinEnable);
}

TEST(LandscapeDisplay, EnterWaitsForAQuietFrameEndThenMovesBothWindowsInOneRequest)
{
    Display d;
    portrait(d);
    const auto uiA = d.registers;
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.ring.pending = 1;
    d.landscapeFrameEnd();
    EXPECT_TRUE(d.landscapeState.wanted);
    EXPECT_FALSE(d.planeLatch.requested());
    d.ring.pending = Display::kNoBuffer;
    const unsigned withdrawals = d.withdrawals;
    d.landscapeFrameEnd();
    EXPECT_GT(d.withdrawals, withdrawals) << "window A's assembly copy changes: no completion flip may promote it";
    EXPECT_GT(d.mediaFromGate, 0u) << "windows B and C change: no media flip may be left armed";
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_TRUE(d.landscapeState.switching);
    EXPECT_TRUE(d.landscapeHoldsKicks());
    EXPECT_EQ(d.dc[Display::kDcStateControl],
              Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq | Display::kWinCActReq);
    for (const uint32_t word : {kColorDepth, kSize, kPrescaled, kDdaIncrement, kLineStride, kOptions})
        EXPECT_EQ(d.dc[Display::kWinBBase + word], uiA[Display::kWinABase + word]) << word;
    for (const uint32_t word : {kStartAddr, kSurfaceKind})
        EXPECT_EQ(d.dc[Display::kWinBBase + 0x40 + word], uiA[Display::kWinABase + 0x40 + word]) << word;
    EXPECT_EQ(d.dc[Display::kWinBBase + kBlendLayer], kBlendPremultiplied);
    EXPECT_EQ(d.dc[Display::kWinBBase + kBlendMatch], kBlendOver);
    const auto &video = landscapeVideo(2).window[0].image;
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], video.options);
    EXPECT_EQ(d.dc[Display::kWinABase + kBlendLayer], kBlendBypass | 1u);
    EXPECT_EQ(d.dc[Display::kWinABase + 0x40 + kStartAddrU], d.planeSlotIova(1) + video.start[1]);
    for (unsigned i = 0; i < 8; ++i)
        EXPECT_EQ(d.dc[0xa00 + kCscYof + i], kCsc[kBt709][0][i]);
    EXPECT_EQ(d.dc[0xa00 + kHFilter0 + 1], kHFilterTaps[1]);
    EXPECT_EQ(d.dc[Display::kWinCBase + kOptions], 0u);
    EXPECT_TRUE(d.gPanicRestore.landscape);
    EXPECT_EQ(d.gPanicRestore.uiDepth, kDepthB8G8R8A8);
    EXPECT_FALSE(d.landscapeOn());
    ASSERT_TRUE(d.boundary());
    EXPECT_TRUE(d.landscapeOn());
    EXPECT_FALSE(d.landscapeState.switching);
    EXPECT_FALSE(d.landscapeHoldsKicks());
}

TEST(LandscapeDisplay, ANewerCompositionInTheCurrentModeCancelsTheChangeAndKeepsItsSwaps)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    ASSERT_TRUE(d.stage(portraitVideo(3, 3)));
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_EQ(d.planeSetOf(d.planeLatch.armed).pendingSeq, 2u);
    EXPECT_EQ(d.planeSetOf(d.planeLatch.armed).seq, 3u);
}

TEST(LandscapeDisplay, AnOlderCompositionHeldForALatchNeverCancelsANewerChange)
{
    Display d;
    portrait(d);
    d.planeHeld = portraitVideo(2, 3);
    d.planeHeldSet = true;
    ASSERT_FALSE(d.stage(landscapeVideo(3)));
    EXPECT_FALSE(d.planeHeldSet);
    EXPECT_EQ(d.landscapeState.held.pendingSeq, 2u);
    EXPECT_FALSE(d.stage(portraitVideo(2, 3)));
    EXPECT_TRUE(d.landscapeState.wanted);
    EXPECT_EQ(d.landscapeState.held.seq, 3u);
    d.landscapeFrameEnd();
    ASSERT_TRUE(d.boundary());
    EXPECT_TRUE(d.landscapeOn());
}

TEST(LandscapeDisplay, CompositionsDuringTheChangeWaitForItsLatch)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.landscapeFrameEnd();
    EXPECT_FALSE(d.stage(landscapeVideo(3, 3)));
    EXPECT_TRUE(d.planeHeldSet);
    EXPECT_EQ(d.planeHeld.seq, 3u);
}

TEST(LandscapeDisplay, LandscapeVideoFramesGoToWindowAAlone)
{
    Display d;
    landscape(d);
    const auto before = d.registers;
    ASSERT_TRUE(d.stage(landscapeVideo(3, 3)));
    EXPECT_EQ(d.dc[Display::kDcStateControl], Display::kGeneralActReq | Display::kWinAActReq | Display::kWinCActReq);
    EXPECT_EQ(d.dc[Display::kWinABase + 0x40 + kStartAddr], d.planeSlotIova(3));
    for (const uint32_t word : {kOptions, kBlendLayer, kColorDepth})
        EXPECT_EQ(d.dc[Display::kWinBBase + word], before[Display::kWinBBase + word]);
    ASSERT_TRUE(d.boundary());
    EXPECT_TRUE(d.landscapeOn());
}

TEST(LandscapeDisplay, LeaveMovesTheUiBackOntoAAndTheOverlayOntoB)
{
    Display d;
    landscape(d);
    const auto uiB = d.registers;
    ASSERT_FALSE(d.stage(portraitVideo(3)));
    d.landscapeFrameEnd();
    EXPECT_EQ(d.dc[Display::kDcStateControl],
              Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq | Display::kWinCActReq);
    for (const uint32_t word : {kColorDepth, kSize, kPrescaled, kDdaIncrement, kLineStride, kOptions})
        EXPECT_EQ(d.dc[Display::kWinABase + word], uiB[Display::kWinBBase + word]) << word;
    EXPECT_EQ(d.dc[Display::kWinABase + kBlendLayer], kBlendPremultiplied);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], portraitVideo(3).window[0].image.options);
    EXPECT_EQ(d.dc[Display::kWinBBase + kBlendLayer], kBlendBypass | 1u);
    ASSERT_TRUE(d.boundary());
    EXPECT_FALSE(d.landscapeOn());
    EXPECT_FALSE(d.gPanicRestore.landscape);
}

TEST(LandscapeDisplay, LeavingWithNoOverlayPutsHekatesBlendBack)
{
    Display d;
    landscape(d);
    Display::PlaneSet none = {};
    none.seq = 3;
    ASSERT_FALSE(d.stage(none));
    d.landscapeFrameEnd();
    EXPECT_EQ(d.dc[Display::kWinABase + kBlendLayer], d.planeBlendSaved[0]);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], 0u);
    ASSERT_TRUE(d.boundary());
    EXPECT_FALSE(d.landscapeOn());
}

TEST(LandscapeDisplay, TheUiWindowMustShowTheFrontFrameAtTheLatch)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.landscapeFrameEnd();
    d.dc[Display::kWinBBase + 0x40 + kStartAddr] = 0xdead0000;
    EXPECT_THROW(d.boundary(), std::runtime_error);
}

TEST(LandscapeDisplay, NoQuietFrameEndAbandonsTheChangeForTheBoot)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.transferred = true;
    for (unsigned i = 0; i < Display::kLandscapeQuietFrames; ++i) {
        ++d.frameEnds;
        d.landscapeFrameEnd();
    }
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_TRUE(d.landscapeState.retired);
    EXPECT_EQ(d.retiredUnshown, 2u);
    EXPECT_FALSE(d.landscapeHoldsKicks());
    EXPECT_FALSE(d.landscapeOn());
}

TEST(LandscapeDisplay, AFailedRotatedReservationStaysPortrait)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    reserveError = -1;
    d.landscapeFrameEnd();
    EXPECT_TRUE(d.landscapeState.retired);
    EXPECT_FALSE(d.landscapeState.switching);
    EXPECT_FALSE(d.planeLatch.requested());
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], Display::kWinEnable);
    EXPECT_EQ(d.retiredUnshown, 2u);
}

TEST(LandscapeDisplay, LeaveNowPutsThePortraitWindowsIntoTheAssemblyAtOnce)
{
    Display d;
    landscape(d);
    const auto uiB = d.registers;
    d.withdrawals = 0;
    d.landscapeLeaveNow("test");
    EXPECT_GT(d.withdrawals, 0u);
    EXPECT_FALSE(d.landscapeOn());
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], uiB[Display::kWinBBase + kOptions]);
    EXPECT_EQ(d.dc[Display::kWinABase + 0x40 + kStartAddr], Display::frontAddress);
    EXPECT_EQ(d.dc[Display::kWinABase + kBlendLayer], d.planeBlendSaved[0]);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], 0u);
    EXPECT_EQ(d.dc[Display::kWinCBase + kOptions], 0u);
    EXPECT_FALSE(d.planeSetOf(d.planeLatch.active).on());
    EXPECT_FALSE(d.gPanicRestore.landscape);
    EXPECT_EQ(d.dc[Display::kDcStateControl],
              Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq | Display::kWinCActReq);
}

TEST(LandscapeDisplay, LeaveNowDuringAnEnterTakesTheUiBackFromB)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.landscapeFrameEnd();
    d.landscapeLeaveNow("test");
    EXPECT_FALSE(d.landscapeState.switching);
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], Display::kWinEnable);
    EXPECT_EQ(d.dc[Display::kWinABase + kColorDepth], kDepthB8G8R8A8);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], 0u);
    ASSERT_TRUE(d.boundary());
    EXPECT_FALSE(d.landscapeOn());
}

TEST(LandscapeDisplay, AStoppedScanoutLeavesAtOnceInsteadOfHolding)
{
    Display d;
    landscape(d);
    d.scanoutStopped = true;
    EXPECT_TRUE(d.stage(portraitVideo(3)));
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_FALSE(d.landscapeHoldsKicks());
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], Display::kWinEnable);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], portraitVideo(3).window[0].image.options);
    ASSERT_TRUE(d.boundary());
    EXPECT_FALSE(d.landscapeOn());
}

TEST(LandscapeDisplay, AHeldEnterIsDroppedWhenTheScanoutStops)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.scanoutStopped = true;
    d.landscapeScanoutStopped();
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_FALSE(d.landscapeState.retired);
    EXPECT_EQ(d.retiredUnshown, 2u);
    EXPECT_FALSE(d.landscapeHoldsKicks());
}

TEST(LandscapeDisplay, APortraitRecoveryDropsAHeldChangeAndKeepsLandscapeOn)
{
    Display d;
    portrait(d);
    ASSERT_FALSE(d.stage(landscapeVideo(2)));
    d.beginPlaneRecovery("test");
    EXPECT_FALSE(d.landscapeState.wanted);
    EXPECT_FALSE(d.landscapeState.retired);
}

TEST(LandscapeDisplay, AnOverlayRecoveryLeavesLandscapeFirstAndForGood)
{
    Display d;
    landscape(d);
    d.beginPlaneRecovery("test");
    EXPECT_TRUE(d.landscapeState.retired);
    EXPECT_FALSE(d.landscapeOn());
    EXPECT_FALSE(d.planeRecoveryTarget.landscape);
    EXPECT_FALSE(d.planeRecoveryTarget.on());
    EXPECT_EQ(d.dc[Display::kWinABase + kOptions], Display::kWinEnable);
    EXPECT_EQ(d.dc[Display::kWinBBase + kOptions], 0u);
}
}  // namespace
