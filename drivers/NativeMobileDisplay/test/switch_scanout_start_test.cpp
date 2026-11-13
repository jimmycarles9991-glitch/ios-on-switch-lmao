// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutRing.h"

namespace {

struct PanicRestore {
    bool armed = false;
} gPanicRestore;

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("display panic");
}

void IOLog(const char *, ...)
{}

struct Display {
    static constexpr const char *kBlockScanName = "portrait";
    static constexpr int kNoBuffer = TegraScanoutRing::kNone;
    static constexpr int kSlot0 = TegraScanoutRing::kMaxBuffers;
    static constexpr uint32_t kBlockSurfaceKind = 0x22, kBlockLineStride = 3072, kBlockHOffset = 0;
    static constexpr uint32_t kBlockScan = 0, kWinAActHcntrSel = 1 << 2;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWindowASelect = 0x10;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kWinBActReq = 4;
    static constexpr uint32_t kWinAUnderflowInt = 1 << 8, kWinBUnderflowInt = 1 << 9;
    static constexpr unsigned kLatchFrames = 6, kDirectUnderflowFrames = 3, kUnderflowFrames = 12;

    enum Reg {
        kDcRegActControl,
        kDcStateAccess,
        kDcWindowHeader,
        kWinLineStride,
        kWinbufSurfaceKind,
        kWinbufHOffset,
        kWinOptions,
        kWinbufStartAddr,
        kDcIntStatus,
        kDcIntEnable,
        kDcStateControl,
        kWinbufUnderflow,
        kRegCount
    };

    struct Registers {
        std::array<uint32_t, kRegCount> values{};
        std::vector<uint32_t> requests;

        struct Reference {
            Registers &registers;
            unsigned index;

            operator uint32_t() const
            {
                return registers.values[index];
            }

            void operator=(uint32_t value)
            {
                if (index == kDcStateControl)
                    registers.requests.push_back(value);
                registers.values[index] = index == kDcIntStatus ? registers.values[index] & ~value : value;
            }
        };

        Reference operator[](unsigned index)
        {
            return {*this, index};
        }
    } dc, windowB;

    std::array<uint32_t, kRegCount> active{}, activeB{};
    bool uiOnB = false;  // landscape: the UI's flips go to window B's own registers

    struct Layout {
        uint32_t kind, stride, hOffset;
    };

    std::array<Layout, TegraScanoutRing::kMaxEntries> layouts{};

    struct Plane {
        uint64_t seq = 0;
        uint32_t address = 0;
    };

    struct Probe {
        bool active() const
        {
            return false;
        }
    } probe;

    TegraScanoutRing ring;
    std::array<uint64_t, TegraScanoutRing::kMaxEntries> entrySeq{}, entryPendingSeq{}, marks{};
    std::array<bool, TegraScanoutRing::kMaxEntries> entryPlaneSet{};
    std::array<Plane, TegraScanoutRing::kMaxEntries> entryPlane{};
    Plane planeNext{}, planeTarget{}, stagedPlane{};
    bool doubleBuffer = true, directAttached = true, directOn = true;
    bool rotated = false, rotationLatched = false, rotationAnnounced = false;
    bool planeNextSet = false, stats = false, probeUnderflow = false;
    uint32_t pitchActControl = kWinAActHcntrSel | 0x40, pitchOptions = 0x40000000;
    uint64_t frameEnds = 10, feStamp = 100, rotationRequested = 0, flipRequested = 0, flipRequest = 41;
    uint64_t retired = 0, retiredAt = 0, stReplaced = 0, stUnderflows = 0;
    unsigned underflowRun = 0, pitchChecks = 0, hooks = 0, barriers = 0, deadlineArms = 0;
    unsigned marksWritten = 0, tapeStops = 0, presents = 0, latched = 0, planeStages = 0;
    uint32_t notedUnderflow = 0, underflowCounts[3] = {};
    unsigned memoryPlans = 0;

    Display()
    {
        gPanicRestore = {};
        ring.reset(3);
        for (auto &layout : layouts)
            layout = {kBlockSurfaceKind, kBlockLineStride, kBlockHOffset};
        layouts[kSlot0] = {0, 2944, 0};
        active[kWinbufStartAddr] = surfaceAddress(0);
        active[kWinOptions] = pitchOptions;
        dc.values[kDcStateAccess] = kReadMuxActive | kWriteMuxActive;
    }

    uint32_t surfaceAddress(int i) const
    {
        return i >= kSlot0 ? 0x10000000 + static_cast<uint32_t>(i - kSlot0) * 0x1000000
                           : 0xc0000000 + static_cast<uint32_t>(i) * 0x1000000;
    }

    int surfaceIndex(uint32_t address) const
    {
        for (int i = 0; i < static_cast<int>(layouts.size()); ++i)
            if (surfaceAddress(i) == address)
                return i;
        return kNoBuffer;
    }

    uint32_t expectedStart() const
    {
        return surfaceAddress(doubleBuffer ? (ring.pending == kNoBuffer ? ring.front : ring.pending) : 0);
    }

    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &offset) const
    {
        const auto &layout = layouts.at(i);
        kind = layout.kind;
        stride = layout.stride;
        offset = layout.hOffset;
    }

    bool sameLayout(int a, int b) const
    {
        const auto &x = layouts.at(a), &y = layouts.at(b);
        return x.kind == y.kind && x.stride == y.stride && x.hOffset == y.hOffset;
    }

    uint64_t seqOf(uint64_t request) const
    {
        return request;
    }

    void markFlip(int i)
    {
        marks.at(i) = flipRequest;
        ++marksWritten;
    }

    void checkPitchWindow()
    {
        ++pitchChecks;
        if (active[kWinbufStartAddr] != surfaceAddress(0))
            panic("initial pitch window changed");
    }

    void chainPanicHook()
    {
        ++hooks;
    }

    void scanoutMemoryBarrier()
    {
        ++barriers;
    }

    bool presentedByCompletion(int)
    {
        return false;
    }

    void flipFromGate()
    {}

    void armFlipDeadline()
    {
        ++deadlineArms;
    }

    void planeStage(const Plane &plane, uint32_t &update, uint32_t &request)
    {
        ++planeStages;
        stagedPlane = plane;
        update |= kWinBUpdate;
        request |= kWinBActReq;
    }

    uint32_t windowActive(Reg reg) const
    {
        return active[reg];
    }

    Registers::Reference uiWord(unsigned index)
    {
        return uiOnB ? windowB[index] : dc[index];
    }

    uint32_t uiActive(unsigned index) const
    {
        return uiOnB ? activeB[index] : active[index];
    }

    uint32_t uiUpdate() const
    {
        return uiOnB ? kWinBUpdate : kWinAUpdate;
    }

    uint32_t uiRequest() const
    {
        return uiOnB ? kWinBActReq : kWinAActReq;
    }

    uint32_t uiUnderflow() const
    {
        return uiOnB ? kWinBUnderflowInt : kWinAUnderflowInt;
    }

    char uiName() const
    {
        return uiOnB ? 'B' : 'A';
    }

    void landscapeRunUnderflow(uint32_t)
    {}

    void stopConsoleTape()
    {
        ++tapeStops;
    }

    void notePresent(uint64_t, uint64_t, uint64_t)
    {
        ++presents;
    }

    void retireThrough(uint64_t seq, uint64_t stamp)
    {
        retired = seq;
        retiredAt = stamp;
    }

    void flipLatched()
    {
        ++latched;
    }

    void releaseIdleDirectReads()
    {}

    void noteLatchedUnderflow(uint32_t latched)
    {
        notedUnderflow |= latched;
    }

    void applyUiMemoryPlan()
    {
        ++memoryPlans;
    }

    void checkFlip(bool)
    {
        const int asked = ring.pending;
        if (ring.latch(surfaceIndex(uiActive(kWinbufStartAddr)), dc.values[kDcStateControl] != 0) ==
            TegraScanoutRing::kTaken) {
            retireThrough(entrySeq[asked], feStamp);
            flipLatched();
        }
    }

    void frameEnd(bool latch)
    {
        if (latch) {
            active = dc.values;
            activeB = windowB.values;
            dc.values[kDcStateControl] = 0;
        }
        ++frameEnds;
        feStamp += 16667;
        if (rotated)
            checkRotation();
    }

#define SWITCH_FRAME_TIMELINE 0
#define KDBG(...)
#include "../SwitchScanoutStart.h"
#undef KDBG
#undef SWITCH_FRAME_TIMELINE
};

TEST(ScanoutStart, FirstPitchDirectInitializesThenRetiresAtFrameEnd)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    EXPECT_TRUE(d.rotated);
    EXPECT_FALSE(d.rotationLatched);
    EXPECT_EQ(d.ring.pending, Display::kSlot0);
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], 0x10000000u);
    EXPECT_EQ(d.dc.values[Display::kWinbufSurfaceKind], 0u);
    EXPECT_EQ(d.dc.values[Display::kWinLineStride], 2944u);
    EXPECT_EQ(d.dc.values[Display::kDcRegActControl], 0x40u);
    EXPECT_EQ(d.dc.values[Display::kDcStateAccess], 0u);
    EXPECT_NE(d.dc.values[Display::kDcIntEnable] & Display::kWinAUnderflowInt, 0u);
    EXPECT_TRUE(gPanicRestore.armed);
    EXPECT_EQ(d.hooks, 1u);
    EXPECT_EQ(d.pitchChecks, 1u);
    EXPECT_EQ(d.barriers, 1u);
    EXPECT_EQ(d.deadlineArms, 1u);
    EXPECT_EQ(d.marksWritten, 1u);
    EXPECT_EQ(d.retired, 0u);
    EXPECT_EQ(d.tapeStops, 0u);
    d.flipRequest = 999;
    d.frameEnd(true);
    EXPECT_TRUE(d.rotationLatched);
    EXPECT_EQ(d.ring.front, Display::kSlot0);
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);
    EXPECT_EQ(d.retired, 41u);
    EXPECT_EQ(d.retiredAt, d.feStamp);
    EXPECT_EQ(d.tapeStops, 1u);
    EXPECT_EQ(d.latched, 1u);
    EXPECT_EQ(d.presents, 1u);
}

TEST(ScanoutStart, FirstTiledDirectUsesItsOwnLayout)
{
    Display d;
    d.presentEntry(Display::kSlot0 + 1);
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], d.surfaceAddress(Display::kSlot0 + 1));
    EXPECT_EQ(d.dc.values[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    EXPECT_EQ(d.dc.values[Display::kWinLineStride], Display::kBlockLineStride);
    d.frameEnd(true);
    EXPECT_EQ(d.ring.front, Display::kSlot0 + 1);
    EXPECT_EQ(d.retired, 41u);
}

TEST(ScanoutStart, FirstCopiedFrameKeepsItsRingAndLayout)
{
    Display d;
    d.ring.claim(1);
    d.startRotation();
    EXPECT_EQ(d.ring.pending, 1);
    EXPECT_EQ(d.ring.superseded, Display::kNoBuffer);
    EXPECT_EQ(d.ring.drawing, Display::kNoBuffer);
    EXPECT_EQ(d.marksWritten, 1u);
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], d.surfaceAddress(1));
    EXPECT_EQ(d.dc.values[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    d.frameEnd(true);
    EXPECT_EQ(d.ring.front, 1);
    EXPECT_EQ(d.retired, 41u);
}

TEST(ScanoutStart, CopyAfterDirectUsesNormalFlipWithoutRecheckingBootWindow)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.frameEnd(true);
    const int copied = d.ring.target();
    ASSERT_NE(copied, Display::kNoBuffer);
    d.flipRequest = 42;
    d.presentEntry(copied);
    EXPECT_EQ(d.pitchChecks, 1u);
    EXPECT_EQ(d.hooks, 1u);
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], d.surfaceAddress(copied));
    EXPECT_EQ(d.dc.values[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    EXPECT_EQ(d.dc.values[Display::kWinLineStride], Display::kBlockLineStride);
    d.frameEnd(true);
    EXPECT_EQ(d.ring.front, copied);
    EXPECT_EQ(d.retired, 42u);
}

TEST(ScanoutStart, FirstDirectArmsItsPairedPlaneInOneTransaction)
{
    Display d;
    d.planeNextSet = true;
    d.planeNext = {777, 0x20000000};
    d.presentEntry(Display::kSlot0);
    EXPECT_FALSE(d.planeNextSet);
    EXPECT_FALSE(d.entryPlaneSet[Display::kSlot0]);
    EXPECT_EQ(d.planeStages, 1u);
    EXPECT_EQ(d.stagedPlane.seq, 41u);
    EXPECT_EQ(d.stagedPlane.address, 0x20000000u);
    EXPECT_EQ(d.planeTarget.seq, 41u);
    ASSERT_EQ(d.dc.requests.size(), 2u);
    EXPECT_EQ(d.dc.requests[0], Display::kGeneralUpdate | Display::kWinAUpdate | Display::kWinBUpdate);
    EXPECT_EQ(d.dc.requests[1], Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq);
    d.frameEnd(true);
    EXPECT_EQ(d.retired, 41u);
}

TEST(ScanoutStart, FirstCopyArmsItsPairedPlane)
{
    Display d;
    d.planeNextSet = true;
    d.planeNext = {777, 0x20000000};
    d.startRotation();
    EXPECT_FALSE(d.planeNextSet);
    EXPECT_EQ(d.planeStages, 1u);
    EXPECT_EQ(d.stagedPlane.seq, 41u);
    ASSERT_EQ(d.dc.requests.size(), 2u);
    EXPECT_EQ(d.dc.requests[1], Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq);
}

TEST(ScanoutStart, WrongActiveAddressDoesNotRetire)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.active = d.dc.values;
    d.active[Display::kWinbufStartAddr] = d.surfaceAddress(0);
    d.dc.values[Display::kDcStateControl] = 0;
    EXPECT_THROW(d.frameEnd(false), std::runtime_error);
    EXPECT_FALSE(d.rotationLatched);
    EXPECT_EQ(d.retired, 0u);
    EXPECT_EQ(d.tapeStops, 0u);
    EXPECT_EQ(d.ring.pending, Display::kSlot0);
}

TEST(ScanoutStart, WrongActiveLayoutDoesNotRetire)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.active = d.dc.values;
    d.active[Display::kWinbufSurfaceKind] = Display::kBlockSurfaceKind;
    d.dc.values[Display::kDcStateControl] = 0;
    EXPECT_THROW(d.frameEnd(false), std::runtime_error);
    EXPECT_FALSE(d.rotationLatched);
    EXPECT_EQ(d.retired, 0u);
}

TEST(ScanoutStart, UnlatchedRequestKeepsTheDeadlineAndRejectsAtLimit)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    for (unsigned i = 1; i < Display::kLatchFrames; ++i)
        EXPECT_NO_THROW(d.frameEnd(false));
    EXPECT_EQ(d.retired, 0u);
    EXPECT_FALSE(d.rotationLatched);
    EXPECT_THROW(d.frameEnd(false), std::runtime_error);
    EXPECT_EQ(d.ring.pending, Display::kSlot0);
}

TEST(ScanoutStart, PreparedEntryKeepsItsSequenceEvenIfCurrentRequestChanges)
{
    Display d;
    d.ring.ready(Display::kSlot0);
    d.entrySeq[Display::kSlot0] = 123;
    d.startRotation(Display::kSlot0);
    EXPECT_EQ(d.ring.superseded, Display::kNoBuffer);
    EXPECT_EQ(d.entrySeq[Display::kSlot0], 123u);
    EXPECT_EQ(d.marksWritten, 0u);
    d.frameEnd(true);
    EXPECT_EQ(d.retired, 123u);
}

TEST(ScanoutStart, LandscapeFlipsWriteWindowBAndLeaveWindowAToTheVideo)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.frameEnd(true);
    ASSERT_TRUE(d.rotationLatched);
    d.uiOnB = true;
    d.windowB.values = d.dc.values;
    d.activeB = d.active;
    const auto windowA = d.dc.values;
    const int copied = d.ring.target();
    ASSERT_NE(copied, Display::kNoBuffer);
    d.dc.requests.clear();
    d.flipRequest = 43;
    d.presentEntry(copied);
    EXPECT_EQ(d.windowB.values[Display::kWinbufStartAddr], d.surfaceAddress(copied));
    EXPECT_EQ(d.windowB.values[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    EXPECT_EQ(d.windowB.values[Display::kWinLineStride], Display::kBlockLineStride);
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], windowA[Display::kWinbufStartAddr]);
    EXPECT_EQ(d.dc.values[Display::kWinbufSurfaceKind], windowA[Display::kWinbufSurfaceKind]);
    ASSERT_EQ(d.dc.requests.size(), 2u);
    EXPECT_EQ(d.dc.requests[0], Display::kGeneralUpdate | Display::kWinBUpdate);
    EXPECT_EQ(d.dc.requests[1], Display::kGeneralActReq | Display::kWinBActReq);
    d.frameEnd(true);
    EXPECT_EQ(d.ring.front, copied);
    EXPECT_EQ(d.retired, 43u);
}

TEST(ScanoutStart, LandscapeCountsOnlyWindowBUnderflowsAgainstTheUi)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.frameEnd(true);
    d.uiOnB = true;
    d.dc.values[Display::kDcIntStatus] = Display::kWinAUnderflowInt;
    d.frameEnd(false);
    EXPECT_EQ(d.underflowRun, 0u);
    EXPECT_EQ(d.dc.values[Display::kDcIntStatus], Display::kWinAUnderflowInt);
    d.dc.values[Display::kDcIntStatus] = Display::kWinBUnderflowInt;
    d.frameEnd(false);
    EXPECT_EQ(d.underflowRun, 1u);
    EXPECT_EQ(d.dc.values[Display::kDcIntStatus], 0u);
}

TEST(ScanoutStart, SingleBufferCopiedStartupStillLatches)
{
    Display d;
    d.doubleBuffer = false;
    d.startRotation();
    EXPECT_EQ(d.dc.values[Display::kWinbufStartAddr], d.surfaceAddress(0));
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);
    EXPECT_EQ(d.deadlineArms, 0u);
    d.frameEnd(true);
    EXPECT_TRUE(d.rotationLatched);
    EXPECT_EQ(d.tapeStops, 1u);
}

TEST(ScanoutStart, WindowAsMemoryPlanAtEachFirstLatch)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    EXPECT_EQ(d.memoryPlans, 0u);
    d.frameEnd(true);
    EXPECT_EQ(d.memoryPlans, 1u);
    d.frameEnd(false);
    d.frameEnd(false);
    EXPECT_EQ(d.memoryPlans, 1u);
    // SC7's resume (SwitchDisplayResume.h): window A latches the restored state again.
    d.rotationLatched = false;
    d.rotationRequested = d.frameEnds;
    d.frameEnd(true);
    EXPECT_EQ(d.memoryPlans, 2u);
}

TEST(ScanoutStart, LatchedUnderflowAfterTheRotationIsNotedForTheFrame)
{
    Display d;
    d.presentEntry(Display::kSlot0);
    d.frameEnd(true);
    d.dc.values[Display::kDcIntStatus] = Display::kWinAUnderflowInt;
    d.frameEnd(false);
    EXPECT_EQ(d.notedUnderflow, Display::kWinAUnderflowInt);
    EXPECT_EQ(d.dc.values[Display::kDcIntStatus], 0u);
    EXPECT_EQ(d.underflowRun, 1u);
}

}  // namespace
