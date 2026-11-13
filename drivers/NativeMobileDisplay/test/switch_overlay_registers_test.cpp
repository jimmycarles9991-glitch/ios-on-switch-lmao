// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include "../SwitchDisplayComposition.h"
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchLandscapeWindows.h"

namespace {
uint64_t reserved, reserveLimit = UINT64_MAX;
switch_display_memory_head reservedHead;
int reserveError;

int switch_emc_display_memory_ready()
{
    return 1;
}

int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    reserved = request->peak_bytes_per_second;
    reservedHead = *request;
    return request->peak_bytes_per_second > reserveLimit ? -1 : reserveError;
}

void IOLog(const char *, ...)
{}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("scanout remained active");
}

using SwitchMMIOWords = uint32_t *;

struct Display {
    bool entryPlaneSet[4] = {};
    using PlaneSet = switch_display::Composition;
    using Window = switch_landscape::Window;
    using Commits = switch_display::Compositions<PlaneSet>;
    static constexpr uint32_t kWinABase = 0xb80, kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kWinbufCBase = 0xfc0, kWincBBase = 0xc00, kWincCBase = 0xe00;
    static constexpr uint32_t kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10, kWinCUpdate = 1 << 11;
    static constexpr uint32_t kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kGeneralActReq = 1;
    static constexpr uint32_t kWinBActHcntrSel = 1 << 4, kWinCActHcntrSel = 1 << 6;
    static constexpr uint32_t kWinBUnderflowInt = 1 << 9, kWinCUnderflowInt = 1 << 10;
    static constexpr uint32_t kDcRegActControl = 0x43, kDcStateControl = 0x41;
    static constexpr uint32_t kDcIntEnable = 0x39, kDcIntStatus = 0x37;
    static constexpr uint32_t kDcStateAccess = 0x40, kReadMuxActive = 1, kWriteMuxActive = 4;
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
    uint32_t planeBlendSaved[3] = {}, pitchActControl = UINT32_MAX;
    uint64_t planeRequestedAt = 0, frameEnds = 10;
    unsigned stPlaneFlips = 0, stPlaneDropped = 0, deadlines = 0, withdrawals = 0, releases = 0, planeUnderflowRun = 0;
    static constexpr unsigned kLatchFrames = 4;

    void scanoutMemoryBarrier()
    {}

    void planeReleaseIdle()
    {
        ++releases;
    }

    Display()
    {
        landscapeState = {};
        planeLatch.values = sets;
        reserved = 0;
        reserveError = 0;
        reserveLimit = UINT64_MAX;
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        EXPECT_TRUE(displayMemory.configure(mode, 720, 1280, false));
    }

    const void *firstUntaken() const
    {
        return nullptr;
    }

    bool planeWindowIdle() const
    {
        return false;
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

    const PlaneSet &planeSetOf(int id) const
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    void armFlipDeadline()
    {
        ++deadlines;
    }

#include "../SwitchLandscapeState.h"

    bool landscapeHolds(PlaneSet &)
    {
        return false;
    }

    void landscapeLeaveNow(const char *)
    {}

    void landscapeAbandonWant()
    {}

    void withdrawCompletionFlip()
    {
        ++withdrawals;
    }

    void mediaFlipFromGate()
    {}

#include "../SwitchDisplayBandwidthDisplay.h"
#include "../SwitchOverlayRegisters.h"
#include "../SwitchPlaneAdmissionDrop.h"
#include "../SwitchCompositionRegisters.h"
#include "../SwitchOverlayRecovery.h"
#include "../SwitchPlanesResume.h"

    bool stage(const PlaneSet &p, uint32_t *updates = nullptr, uint32_t *requests = nullptr)
    {
        uint32_t update = 0, request = 0;
        const bool result = planeStage(p, update, request);
        if (updates)
            *updates = update;
        if (requests)
            *requests = request;
        return result;
    }

    void latch()
    {
        ASSERT_TRUE(planeLatch.latched(false, planeScannedSet()));
        planeRetireBandwidth();
    }
};

Display::LandscapeState Display::landscapeState;

Display::PlaneSet video(native_planes::Matrix matrix, int slot = 1)
{
    Display::PlaneSet p = {};
    p.window[0].on = true;
    p.window[0].slot = slot;
    p.seq = 1;
    p.window[0].image.visible = p.window[0].image.yuv = true;
    p.window[0].image.matrix = matrix;
    p.window[0].image.options = native_planes::kWinEnable;
    p.window[0].image.size = (1280u << 16) | 720;
    p.window[0].image.prescaled = p.window[0].image.size;
    p.window[0].image.lineStride = 768;
    return p;
}

TEST(OverlayRegisters, FirstYuvCommitProgramsCscBeforeItBecomesLive)
{
    Display display;
    const auto p = video(native_planes::kBt709);
    ASSERT_TRUE(display.stage(p));
    EXPECT_TRUE(display.planeLatch.requested());
    EXPECT_EQ(display.planeCsc[0][0], int(native_planes::kBt709));
    for (unsigned i = 0; i < 8; ++i)
        EXPECT_EQ(display.dc[Display::kWincBBase + native_planes::kCscYof + i],
                  native_planes::kCsc[p.window[0].image.matrix][0][i]);
    EXPECT_EQ(display.sets[display.planeLatch.armed].pendingSeq, 1u);
}

TEST(OverlayRegisters, MatrixTransitionUsesIdleCWithoutRecoloringLiveB)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    display.latch();
    const auto oldCsc = display.dc[Display::kWincBBase + native_planes::kCscYof + 1];
    uint32_t updates, requests;
    ASSERT_TRUE(display.stage(video(native_planes::kBt709, 2), &updates, &requests));
    EXPECT_TRUE(display.planeTarget.window[1].on);
    EXPECT_EQ(display.planeTarget.window[1].slot, 2);
    EXPECT_EQ(display.dc[Display::kWincBBase + native_planes::kCscYof + 1], oldCsc);
    EXPECT_EQ(display.planeCsc[1][0], int(native_planes::kBt709));
    EXPECT_EQ(updates & (Display::kWinBUpdate | Display::kWinCUpdate), Display::kWinBUpdate | Display::kWinCUpdate);
    EXPECT_EQ(requests & (Display::kWinBActReq | Display::kWinCActReq), Display::kWinBActReq | Display::kWinCActReq);
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], 0u);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kOptions], native_planes::kWinEnable);
}

TEST(OverlayRegisters, BothWindowLatchesAndIdentityAreRequiredBeforeOldMappingRetires)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    display.latch();
    ASSERT_TRUE(display.stage(video(native_planes::kBt709, 2)));
    EXPECT_FALSE(display.planeLatch.latched(true, display.planeScannedSet()));
    EXPECT_TRUE(display.planeLatch.uses(1));
    display.dc[Display::kWinBBase + native_planes::kOptions] = native_planes::kWinEnable;
    EXPECT_FALSE(display.planeLatch.latched(false, display.planeScannedSet()));
    EXPECT_TRUE(display.planeLatch.uses(1));
    display.dc[Display::kWinBBase + native_planes::kOptions] = 0;
    display.latch();
    EXPECT_FALSE(display.planeLatch.uses(1));
    EXPECT_TRUE(display.planeLatch.uses(2));
}

// The video column-scanned on window A behind the UI on B (SwitchLandscape.h): 1920x1080 turned onto the 720x1280 panel.
Display::PlaneSet landscapeVideo(int slot = 1)
{
    auto p = video(native_planes::kBt709, slot);
    p.landscape = true;
    p.window[0].depth = 1;
    p.window[0].image.options = native_planes::kWinEnable | native_planes::kCscEnable | switch_landscape::kScanColumn |
                                native_planes::kHFilterOn | native_planes::kVFilterOn;
    p.window[0].image.size = (1280u << 16) | 720;
    p.window[0].image.prescaled = (1920u << 16) | 1080;
    p.window[0].image.dda = (6144u << 16) | 6144;
    p.window[0].image.surfaceKind = 2 | 1 << 4;
    return p;
}

void enterLandscape(Display &display)
{
    display.sets[0] = {};
    display.sets[0].landscape = true;
    display.planeLatch.active = 0;
}

TEST(OverlayRegisters, LandscapeOverlayGoesToWindowAWithItsOwnCscAndLeavesTheUiOnB)
{
    Display display;
    enterLandscape(display);
    const uint32_t ui = native_planes::kWinEnable | 0x40;
    display.dc[Display::kWinBBase + native_planes::kOptions] = ui;
    uint32_t updates = 0, requests = 0;
    const auto p = landscapeVideo();
    ASSERT_TRUE(display.stage(p, &updates, &requests));
    EXPECT_EQ(updates, Display::kGeneralUpdate | Display::kWinAUpdate | Display::kWinCUpdate);
    EXPECT_EQ(requests, Display::kGeneralActReq | Display::kWinAActReq | Display::kWinCActReq);
    EXPECT_EQ(display.dc[Display::kWinABase + native_planes::kOptions], p.window[0].image.options);
    EXPECT_EQ(display.dc[Display::kWinABase + native_planes::kPrescaled], p.window[0].image.prescaled);
    EXPECT_EQ(display.dc[Display::kWinABase + native_planes::kBlendLayer], native_planes::kBlendBypass | 1u);
    EXPECT_EQ(display.dc[0xbc0 + native_planes::kSurfaceKind], p.window[0].image.surfaceKind);
    EXPECT_EQ(display.dc[0xbc0 + native_planes::kStartAddr], display.planeSlotIova(1));
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], ui);
    for (unsigned i = 0; i < 8; ++i)
        EXPECT_EQ(display.dc[0xa00 + native_planes::kCscYof + i], native_planes::kCsc[native_planes::kBt709][0][i]);
    EXPECT_EQ(display.landscapeState.csc[0], int(native_planes::kBt709));
    EXPECT_TRUE(display.landscapeState.filtersWritten);
    EXPECT_EQ(display.planeCsc[0][0], -1);
    EXPECT_EQ(display.dc[Display::kWincBBase + native_planes::kCscYof], 0u);
    display.latch();
    EXPECT_TRUE(display.planeLatch.uses(1));
}

// L4T t21x/bandwidth.c for a rotated semi-planar window: 16 lines of the source's height, 17 when it scales, 2 bytes
// each; the UI on B keeps the line its own window asks for.
TEST(OverlayRegisters, LandscapeReservesWindowARotatedAndTheUiOnB)
{
    Display display;
    enterLandscape(display);
    ASSERT_TRUE(display.stage(landscapeVideo()));
    EXPECT_EQ(reservedHead.windows[0].active, 1u);
    EXPECT_EQ(reservedHead.windows[0].required_buffering_bytes, 1080u * 17 * 2);
    EXPECT_EQ(reservedHead.windows[0].latency_buffering_bytes, 0u);
    EXPECT_EQ(reservedHead.windows[0].peak_bytes_per_second, 78000000ull * 2 * 1080 / 720 * 1920 / 1280);
    EXPECT_EQ(reservedHead.windows[1].active, 1u);
    EXPECT_EQ(reservedHead.windows[1].required_buffering_bytes, 720u * 4);
    EXPECT_EQ(reservedHead.windows[2].active, 0u);
}

TEST(OverlayRegisters, PortraitKeepsTheUiOnAInTheReservation)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    EXPECT_EQ(reservedHead.windows[0].required_buffering_bytes, 720u * 4);
    EXPECT_EQ(reservedHead.windows[1].active, 1u);
    EXPECT_EQ(reservedHead.windows[2].active, 0u);
}

TEST(OverlayRegisters, MixedTransitionReservesBothOldAndNewBeforeProgramming)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    display.latch();
    const auto single = display.displayBandwidth.bytes();
    ASSERT_TRUE(display.stage(video(native_planes::kBt709, 2)));
    EXPECT_GT(display.displayBandwidth.bytes(), single);
    const auto transition = display.displayBandwidth.bytes();
    display.planeRetireBandwidth();
    EXPECT_EQ(display.displayBandwidth.bytes(), transition);
    display.latch();
    EXPECT_EQ(display.displayBandwidth.bytes(), single);
}

TEST(OverlayRegisters, BandwidthRefusalWithNoLatchComingArmsTheOverlaysOffInsteadOfHolding)
{
    Display display;
    display.stats = true;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    display.latch();
    reserveError = -1;
    auto next = video(native_planes::kBt709, 2);
    next.seq = 2;
    ASSERT_TRUE(display.stage(next));
    EXPECT_FALSE(display.planeHeldSet);
    ASSERT_TRUE(display.planeLatch.requested());
    EXPECT_FALSE(display.planeSetOf(display.planeLatch.armed).on());
    EXPECT_EQ(display.planeSetOf(display.planeLatch.armed).seq, 2u);
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], 0u);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kOptions], 0u);
    EXPECT_EQ(display.stPlaneDropped, 1u);
}

// Two overlays over the budget with no latch coming: the heavier goes, the lighter one stays on window B.
TEST(OverlayRegisters, BandwidthRefusalOfTwoOverlaysDropsOnlyTheHeavierWhenOneFits)
{
    Display display;
    display.stats = true;
    auto two = video(native_planes::kBt709, 1);
    two.window[1] = two.window[0];
    two.window[1].slot = 2;
    two.window[1].depth = 2;
    two.window[1].image.prescaled = (1280u << 17) | 2880;  // twice the lines and four times the bytes across
    ASSERT_TRUE(display.stage(video(native_planes::kBt709, 3)));
    display.latch();
    native_planes::Image light = two.window[0].image, heavy = two.window[1].image;
    const uint64_t lightBytes = display.planeAdmissionDemand(light), heavyBytes = display.planeAdmissionDemand(heavy);
    ASSERT_LT(lightBytes, heavyBytes);
    reserveLimit = 0;
    display.stage(two);
    reserveLimit = UINT64_MAX;
    EXPECT_GE(display.stPlaneDropped, 1u);
    Display fits;
    fits.stats = true;
    ASSERT_TRUE(fits.stage(video(native_planes::kBt709, 3)));
    fits.latch();
    fits.stage(video(native_planes::kBt709, 1));
    const uint64_t oneReserved = reserved;
    fits.latch();
    reserveLimit = oneReserved + (heavyBytes - lightBytes) / 2;
    two.seq = 3;
    ASSERT_TRUE(fits.stage(two));
    EXPECT_EQ(fits.stPlaneDropped, 1u);
    const auto &armed = fits.planeSetOf(fits.planeLatch.armed);
    EXPECT_TRUE(armed.window[0].on);
    EXPECT_FALSE(armed.window[1].on);
}

TEST(OverlayRegisters, BandwidthRefusalWithALatchComingWaitsForItThenGoes)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    reserveError = -1;
    const auto before = display.registers;
    EXPECT_FALSE(display.stage(video(native_planes::kBt709, 2)));
    EXPECT_EQ(display.registers, before);
    EXPECT_TRUE(display.planeHeldSet);
    reserveError = 0;
    display.latch();
    display.planeHeldSet = false;
    ASSERT_TRUE(display.stage(display.planeHeld));
    EXPECT_TRUE(display.planeSetOf(display.planeLatch.armed).on());
}

TEST(OverlayRegisters, ConversionNeededWithNoLatchComingDropsThatOverlayInsteadOfHolding)
{
    Display display;
    display.stats = true;
    auto both = video(native_planes::kBt601, 1);
    both.window[1] = video(native_planes::kBt709, 2).window[0];
    ASSERT_TRUE(display.stage(both));
    display.latch();
    ASSERT_TRUE(display.stage(video(native_planes::kBt2020, 3)));
    EXPECT_FALSE(display.planeHeldSet);
    EXPECT_FALSE(display.planeSetOf(display.planeLatch.armed).on());
    EXPECT_EQ(display.stPlaneDropped, 1u);
}

TEST(OverlayRegisters, TwoPossiblyLiveMatricesCannotBeChangedBeforeRetirement)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt601)));
    display.latch();
    ASSERT_TRUE(display.stage(video(native_planes::kBt709, 2)));
    const auto before = display.registers;
    EXPECT_FALSE(display.stage(video(native_planes::kBt2020, 3)));
    EXPECT_EQ(display.registers, before);
    EXPECT_TRUE(display.planeHeldSet);
}

TEST(OverlayRegisters, MatchingWindowAndStaticContentDoNotTouchCsc)
{
    Display display;
    const auto p = video(native_planes::kBt709);
    ASSERT_TRUE(display.stage(p));
    display.latch();
    display.dc[Display::kWincBBase + native_planes::kCscYof] = 1234;
    ASSERT_TRUE(display.stage(p));
    EXPECT_TRUE(display.planeTarget.window[0].on);
    EXPECT_EQ(display.planeTarget.window[0].slot, 1);
    EXPECT_EQ(display.dc[Display::kWincBBase + native_planes::kCscYof], 1234u);
}

TEST(OverlayRegisters, InitializationSetsBothFiltersAndFrameBoundaryModes)
{
    Display display;
    display.dc[Display::kDcRegActControl] = UINT32_MAX;
    display.planeStartRegisters();
    EXPECT_EQ(display.dc[Display::kDcRegActControl] & (Display::kWinBActHcntrSel | Display::kWinCActHcntrSel), 0u);
    EXPECT_TRUE(display.planeFiltersWritten[0]);
    EXPECT_TRUE(display.planeFiltersWritten[1]);
    EXPECT_EQ(display.dc[Display::kDcIntEnable], Display::kWinBUnderflowInt | Display::kWinCUnderflowInt);
    for (unsigned i = 0; i < 16; ++i)
        EXPECT_EQ(display.dc[Display::kWincBBase + native_planes::kHFilter0 + i],
                  display.dc[Display::kWincCBase + native_planes::kHFilter0 + i]);
}

TEST(OverlayRegisters, TwoNativeVideosKeepIndependentCscAndDepthAcrossBankPermutation)
{
    Display display;
    auto both = video(native_planes::kBt601, 1);
    both.window[0].depth = 2;
    both.window[1] = video(native_planes::kBt709, 2).window[0];
    both.window[1].depth = 1;
    ASSERT_TRUE(display.stage(both));
    display.latch();
    EXPECT_EQ(display.planeCsc[0][0], int(native_planes::kBt601));
    EXPECT_EQ(display.planeCsc[1][0], int(native_planes::kBt709));
    EXPECT_TRUE(display.planeLatch.uses(1));
    EXPECT_TRUE(display.planeLatch.uses(2));
    auto next = both;
    next.window[0] = both.window[1];
    next.window[0].depth = 2;
    next.window[1] = both.window[0];
    next.window[1].depth = 1;
    ASSERT_TRUE(display.stage(next));
    EXPECT_EQ(display.planeTarget.window[0].slot, 1);
    EXPECT_EQ(display.planeTarget.window[0].depth, 1u);
    EXPECT_EQ(display.planeTarget.window[1].slot, 2);
    EXPECT_EQ(display.planeTarget.window[1].depth, 2u);
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kBlendLayer], native_planes::kBlendBypass | 1u);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kBlendLayer], native_planes::kBlendBypass | 2u);
}

TEST(OverlayRegisters, RgbOverlayUsesPremultipliedBlendOverLowerVideo)
{
    Display display;
    auto both = video(native_planes::kBt709, 1);
    both.window[0].depth = 2;
    both.window[1] = video(native_planes::kBt709, 2).window[0];
    both.window[1].depth = 1;
    both.window[1].image.yuv = false;
    ASSERT_TRUE(display.stage(both));
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kBlendLayer], native_planes::kBlendPremultiplied | 1u);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kBlendMatch], native_planes::kBlendOver);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kBlendNomatch], native_planes::kBlendOver);
    EXPECT_EQ(display.dc[Display::kWinABase + native_planes::kBlendLayer], native_planes::kBlendPremultiplied);
    // Window A's assembly copy changes: a completion flip armed for window A must not promote it early.
    EXPECT_EQ(display.withdrawals, 1u);
    display.latch();
}

TEST(OverlayRegisters, PartialDualLatchCannotRetireEitherOldBacking)
{
    Display display;
    auto both = video(native_planes::kBt709, 1);
    both.window[1] = video(native_planes::kBt601, 2).window[0];
    ASSERT_TRUE(display.stage(both));
    display.latch();
    const auto oldC = display.dc[Display::kWinbufCBase + native_planes::kStartAddr];
    both.window[0].slot = 3;
    both.window[1].slot = 4;
    ASSERT_TRUE(display.stage(both));
    const auto newC = display.dc[Display::kWinbufCBase + native_planes::kStartAddr];
    display.dc[Display::kWinbufCBase + native_planes::kStartAddr] = oldC;
    EXPECT_FALSE(display.planeLatch.latched(false, display.planeScannedSet()));
    for (int slot = 1; slot <= 4; ++slot)
        EXPECT_TRUE(display.planeLatch.uses(slot));
    display.dc[Display::kWinbufCBase + native_planes::kStartAddr] = newC;
    display.latch();
    EXPECT_FALSE(display.planeLatch.uses(1));
    EXPECT_FALSE(display.planeLatch.uses(2));
    EXPECT_TRUE(display.planeLatch.uses(3));
    EXPECT_TRUE(display.planeLatch.uses(4));
}
}  // namespace

namespace {
TEST(OverlayRecovery, StartingRecoveryKeepsEveryOldCommitAndProducerSnapshot)
{
    Display d;
    auto old = video(native_planes::kBt601, 1);
    ASSERT_TRUE(d.stage(old));
    d.latch();
    auto next = video(native_planes::kBt709, 2);
    next.seq = 2;
    ASSERT_TRUE(d.stage(next));
    d.beginPlaneRecovery("test");
    EXPECT_TRUE(d.planeRecovering);
    EXPECT_TRUE(d.planeLatch.uses(1));
    EXPECT_TRUE(d.planeLatch.uses(2));
    EXPECT_TRUE(d.planeRecoveryTarget.uses(2));
    EXPECT_EQ(d.releases, 0u);
    EXPECT_EQ(d.dc[Display::kDcStateControl], Display::kGeneralActReq | Display::kWinBActReq | Display::kWinCActReq);
}

TEST(OverlayRecovery, NeitherRequestClearNorOneDisabledWindowIsEnough)
{
    Display d;
    ASSERT_TRUE(d.stage(video(native_planes::kBt601)));
    d.latch();
    d.beginPlaneRecovery("test");
    d.dc[Display::kDcStateControl] = 0;
    for (unsigned bank : {Display::kWinBBase, Display::kWinCBase}) {
        d.dc[bank + native_planes::kOptions] = native_planes::kWinEnable;
        EXPECT_TRUE(d.planeRecoveryFrameEnd());
        EXPECT_TRUE(d.planeRecovering);
        EXPECT_EQ(d.releases, 0u);
        d.dc[bank + native_planes::kOptions] = 0;
    }
    d.dc[Display::kDcStateControl] = Display::kWinCActReq;
    EXPECT_TRUE(d.planeRecoveryFrameEnd());
    EXPECT_TRUE(d.planeRecovering);
    EXPECT_EQ(d.releases, 0u);
}

TEST(OverlayRecovery, ObservedOffReinitializesLiveStateAndArmsNewestReadySet)
{
    Display d;
    ASSERT_TRUE(d.stage(video(native_planes::kBt601)));
    d.latch();
    d.beginPlaneRecovery("test");
    auto newest = video(native_planes::kBt709, 3);
    newest.seq = 9;
    EXPECT_FALSE(d.stage(newest));
    EXPECT_TRUE(d.planeRecoveryTarget.uses(3));
    EXPECT_EQ(d.planeRecoveryTarget.pendingSeq, 1u);
    d.dc[Display::kDcStateControl] = 0;
    d.dc[Display::kDcStateAccess] = 0x80;
    ASSERT_TRUE(d.planeRecoveryFrameEnd());
    EXPECT_FALSE(d.planeRecovering);
    EXPECT_EQ(d.dc[Display::kDcStateAccess], 0x80u);
    EXPECT_EQ(d.planeCsc[0][0], int(native_planes::kBt709));
    EXPECT_TRUE(d.planeFiltersWritten[0]);
    EXPECT_TRUE(d.planeLatch.requested());
    EXPECT_TRUE(d.planeLatch.uses(3));
    EXPECT_FALSE(d.planeLatch.uses(1));
    EXPECT_EQ(d.sets[d.planeLatch.armed].seq, 9u);
    EXPECT_EQ(d.sets[d.planeLatch.armed].pendingSeq, 1u);
    EXPECT_EQ(d.releases, 1u);
}

TEST(OverlayRecovery, LateOlderReadyCompositionCannotReplaceNewerOne)
{
    Display d;
    auto p = video(native_planes::kBt601, 3);
    p.seq = 9;
    ASSERT_TRUE(d.stage(p));
    d.beginPlaneRecovery("test");
    auto earlier = video(native_planes::kBt601, 1);
    earlier.seq = 4;
    EXPECT_FALSE(d.stage(earlier));
    EXPECT_TRUE(d.planeRecoveryTarget.uses(3));
    EXPECT_EQ(d.planeRecoveryTarget.seq, 9u);
    EXPECT_EQ(d.planeRecoveryTarget.pendingSeq, 4u);
}

TEST(OverlayRecovery, NeverDisabledHardwareTurnsOverlaysOffForTheBootAndKeepsTheirMemory)
{
    Display d;
    ASSERT_TRUE(d.stage(video(native_planes::kBt601)));
    d.latch();
    d.beginPlaneRecovery("test");
    d.frameEnds += Display::kLatchFrames;
    EXPECT_NO_THROW(d.planeRecoveryFrameEnd());
    EXPECT_TRUE(d.planesAbandoned);
    EXPECT_FALSE(d.planesOn);
    EXPECT_FALSE(d.planeRecovering);
    EXPECT_TRUE(d.planeLatch.uses(1));
    EXPECT_EQ(d.releases, 0u);
    const auto before = d.registers;
    EXPECT_FALSE(d.stage(video(native_planes::kBt709, 2)));
    EXPECT_EQ(d.registers, before);
}

TEST(OverlayRecovery, UnderflowOnTheFirstOverlayRecoversItAndKeepsOverlaysOn)
{
    Display d;
    ASSERT_TRUE(d.stage(video(native_planes::kBt601)));
    d.latch();
    d.beginPlaneRecovery("overlay underflowed 3 frames in a row");
    EXPECT_TRUE(d.planesOn);
    EXPECT_TRUE(d.planeRecovering);
    d.dc[Display::kDcStateControl] = 0;
    ASSERT_TRUE(d.planeRecoveryFrameEnd());
    EXPECT_TRUE(d.planeSetOf(d.planeLatch.armed).on());
}

TEST(OverlayRecovery, ResumeRestartsBoundFromTheFreshOffRequest)
{
    Display d;
    ASSERT_TRUE(d.stage(video(native_planes::kBt601)));
    d.latch();
    d.beginPlaneRecovery("test");
    d.frameEnds += 100;
    d.writePlaneRecoveryOff();
    EXPECT_EQ(d.planeRecoveryAt, d.frameEnds);
    EXPECT_NO_THROW(d.planeRecoveryFrameEnd());
    EXPECT_TRUE(d.planeRecovering);
}
}

TEST(OverlayRegisters, ResumeRewritesBothAcceptedWindowsBeforeRestart)
{
    Display display;
    auto p = video(native_planes::kBt709);
    p.window[1] = video(native_planes::kBt601, 2).window[0];
    p.window[1].depth = 2;
    ASSERT_TRUE(display.stage(p));
    display.latch();
    display.registers.fill(0);
    display.planeCsc[0][0] = display.planeCsc[0][1] = -1;
    display.planeCsc[1][0] = display.planeCsc[1][1] = -1;
    display.planesResume();
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], native_planes::kWinEnable);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kOptions], native_planes::kWinEnable);
    EXPECT_EQ(display.dc[Display::kDcStateControl],
              Display::kGeneralActReq | Display::kWinAActReq | Display::kWinBActReq | Display::kWinCActReq);
    EXPECT_TRUE(display.planeRegistersMatch(display.planeSetOf(display.planeLatch.active)));
}

TEST(OverlayRegisters, ResumeReservationFailureResumesTheOverlaysOffAndTheUiAlone)
{
    Display display;
    auto p = video(native_planes::kBt709);
    p.seq = 5;
    ASSERT_TRUE(display.stage(p));
    display.latch();
    display.registers.fill(0);
    display.displayMemory.invalidate();
    EXPECT_NO_THROW(display.planesResume());
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], 0u);
    EXPECT_EQ(display.dc[Display::kWinCBase + native_planes::kOptions], 0u);
    ASSERT_TRUE(display.planeLatch.requested());
    EXPECT_FALSE(display.planeSetOf(display.planeLatch.armed).on());
    EXPECT_EQ(display.planeSetOf(display.planeLatch.armed).seq, 5u);
    EXPECT_FALSE(display.planeRecovering);
    display.latch();
    EXPECT_FALSE(display.planeLatch.uses(1));
}

TEST(OverlayRegisters, SuspendDuringRecoveryRestoresTargetWithoutAnOffOnlyFirstFrame)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt709)));
    display.latch();
    display.beginPlaneRecovery("test");
    display.planeRecoveryTarget.pendingSeq = 7;
    display.planeRecoveryTarget.seq = 9;
    display.registers.fill(0);
    display.planeCsc[0][0] = display.planeCsc[0][1] = -1;
    display.planesResume();
    EXPECT_FALSE(display.planeRecovering);
    EXPECT_TRUE(display.planeLatch.requested());
    EXPECT_EQ(display.planeSetOf(display.planeLatch.armed).pendingSeq, 7u);
    EXPECT_EQ(display.planeSetOf(display.planeLatch.armed).seq, 9u);
    EXPECT_TRUE(display.planeLatch.uses(1));
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], native_planes::kWinEnable);
    display.latch();
    EXPECT_TRUE(display.planeLatch.uses(1));
}

TEST(OverlayRegisters, ResumeFailureDuringRecoveryKeepsItsSwapsAndArmsTheOffTarget)
{
    Display display;
    ASSERT_TRUE(display.stage(video(native_planes::kBt709)));
    display.latch();
    display.beginPlaneRecovery("test");
    display.planeRecoveryTarget.pendingSeq = 7;
    display.registers.fill(0);
    display.displayMemory.invalidate();
    EXPECT_NO_THROW(display.planesResume());
    EXPECT_FALSE(display.planeRecovering);
    ASSERT_TRUE(display.planeLatch.requested());
    EXPECT_FALSE(display.planeSetOf(display.planeLatch.armed).on());
    EXPECT_EQ(display.planeSetOf(display.planeLatch.armed).pendingSeq, 7u);
    EXPECT_EQ(display.dc[Display::kWinBBase + native_planes::kOptions], 0u);
}
