// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../SwitchDisplayComposition.h"
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchLandscapeWindows.h"
#include "../../SwitchPower/SwitchDisplayBandwidth.h"

namespace {
std::vector<uint64_t> reservations;
std::vector<switch_display_memory_head> heads;
std::vector<std::string> logs;
int reserveError, memoryReady = 1;

void IOLog(const char *format, ...)
{
    logs.emplace_back(format);
}

int switch_emc_display_memory_ready()
{
    return memoryReady;
}

int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    reservations.push_back(request->peak_bytes_per_second);
    heads.push_back(*request);
    return reserveError;
}

struct Display {
    Display()
    {
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        EXPECT_TRUE(displayMemory.configure(mode, 720, 1280, false));
    }

    bool planeRecovering = false;
    bool entryPlaneSet[4] = {};
    using PlaneSet = switch_display::Composition;
    PlaneSet active;

    struct Latch {
        bool pending = false;
        int active = 0, armed = -1, superseded = -1;

        bool requested() const
        {
            return pending;
        }
    } planeLatch;

    bool planeHeldSet = false, gpuScanout = false, queued = false, overlaysIdle = true;

    bool planeWindowIdle() const
    {
        return overlaysIdle;
    }

    void getDisplayDimensions(uint32_t *width, uint32_t *height)
    {
        *width = 720;
        *height = 1280;
    }

    const void *firstUntaken()
    {
        return queued ? this : nullptr;
    }

    const PlaneSet &planeSetOf(int)
    {
        return active;
    }

    using Window = switch_landscape::Window;

    static Window planeWindowOf(const PlaneSet &set, unsigned index)
    {
        return switch_landscape::windowsOf(set.landscape).overlay[index];
    }

    bool landscapeOn() const
    {
        return active.landscape;
    }

#include "../SwitchDisplayBandwidthDisplay.h"
};

native_planes::Layer image(uint32_t width, uint32_t height)
{
    native_planes::Layer layer;
    layer.kind = native_planes::kKindBgra;
    layer.surface.width = width;
    layer.surface.height = height;
    layer.surface.stride[0] = width * 4;
    layer.surface.allocation = layer.surface.descriptorBytes = uint64_t(width) * height * 4;
    layer.src = {0, 0, int32_t(width), int32_t(height)};
    layer.dst = {0, 0, 720, 1280};
    return layer;
}

class DisplayAdmission : public testing::Test {
protected:
    Display display;
    native_planes::Layer layers[4];
    bool present[4] = {true, true, false, false};

    void SetUp() override
    {
        reservations.clear();
        heads.clear();
        logs.clear();
        reserveError = 0;
        memoryReady = 1;
        layers[0] = image(1440, 2560);
        layers[1] = image(720, 1280);
    }
};

TEST_F(DisplayAdmission, RaisesBeforeAcceptanceAndDoesNoExtraWorkAtArm)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    ASSERT_EQ(reservations.size(), 1u);
    EXPECT_EQ(reservations[0], 1872000000u);
    Display::PlaneSet set;
    set.window[0].on = true;
    ASSERT_TRUE(native_planes::imageOf(layers[0], 720, 1280, set.window[0].image));
    EXPECT_TRUE(display.planeReserveBandwidth(set));
    EXPECT_EQ(reservations.size(), 1u);
}

TEST_F(DisplayAdmission, FirmwareRefusalDoesNotCommitAdmissionOrChangeVisibleFrame)
{
    display.active.window[0].on = true;
    reserveError = -5;
    EXPECT_FALSE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    EXPECT_EQ(display.displayBandwidth.bytes(), 0u);
    EXPECT_TRUE(display.active.on());
}

TEST_F(DisplayAdmission, UiOnlySwapNeedsNoAdmissionAndUnavailableQualificationReservesNothing)
{
    present[0] = false;
    display.admitPlaneBandwidth(present, layers, 720, 1280);
    EXPECT_TRUE(reservations.empty());
    present[0] = true;
    for (int unavailable : {-19, -95}) {
        memoryReady = 0;
        reserveError = unavailable;
        display.admitPlaneBandwidth(present, layers, 720, 1280);
        EXPECT_TRUE(reservations.empty());
        EXPECT_FALSE(display.memoryAdmissionReady());
    }
    memoryReady = 1;
    display.displayMemory.invalidate();
    display.admitPlaneBandwidth(present, layers, 720, 1280);
    EXPECT_TRUE(reservations.empty());
    EXPECT_FALSE(display.memoryAdmissionReady());
}

TEST_F(DisplayAdmission, OverlayAdmissionIsTriedOnlyWhileQualificationIsAvailable)
{
    display.admitPlaneBandwidth(present, layers, 720, 1280);
    ASSERT_EQ(reservations.size(), 1u);
    EXPECT_EQ(display.displayBandwidth.bytes(), 1872000000u);
}

TEST_F(DisplayAdmission, TurningEveryOverlayOffNeedsNoReservation)
{
    display.displayMemory.invalidate();
    EXPECT_TRUE(display.planeReserveBandwidth(Display::PlaneSet{}));
    EXPECT_TRUE(reservations.empty());
}

TEST_F(DisplayAdmission, AllPendingConsumersPreserveOldReservationUntilRetirement)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    display.planeLatch.pending = true;
    display.planeRetireBandwidth();
    display.planeLatch.pending = false;
    display.planeHeldSet = true;
    display.planeRetireBandwidth();
    display.planeHeldSet = false;
    display.gpuScanout = true;
    display.planeRetireBandwidth();
    display.gpuScanout = false;
    display.queued = true;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 1u);
    display.queued = false;
    display.planeRetireBandwidth();
    ASSERT_EQ(reservations.size(), 2u);
    EXPECT_EQ(reservations[1], 312000000u);
}

TEST_F(DisplayAdmission, FailedLoweringKeepsHigherReservation)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    reserveError = -5;
    display.planeRetireBandwidth();
    EXPECT_EQ(display.displayBandwidth.bytes(), 1872000000u);
}

TEST_F(DisplayAdmission, InheritedCompanionAndRgbConversionAreReservedBeforeAcceptance)
{
    layers[0] = image(720, 1280);
    layers[1] = image(1440, 2560);
    present[1] = false;
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    present[0] = false;
    present[1] = true;
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    const auto admitted = display.displayBandwidth.bytes();
    Display::PlaneSet set;
    set.window[0].on = set.window[1].on = true;
    ASSERT_TRUE(native_planes::imageOf(layers[0], 720, 1280, set.window[0].image));
    ASSERT_TRUE(native_planes::imageOf(layers[1], 720, 1280, set.window[1].image));
    EXPECT_TRUE(display.planeReserveBandwidth(set));
    EXPECT_EQ(display.displayBandwidth.bytes(), admitted);
    EXPECT_EQ(admitted, 1872000000u);
}

TEST_F(DisplayAdmission, CarriedUiCommitKeepsItsAcceptedEnvelope)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    display.entryPlaneSet[2] = true;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 1u);
    display.entryPlaneSet[2] = false;
    display.planeRetireBandwidth();
    EXPECT_EQ(reservations.size(), 2u);
}

TEST_F(DisplayAdmission, WindowAAloneGetsItsPlanAtTheFirstLatch)
{
    display.applyUiMemoryPlan();
    ASSERT_EQ(heads.size(), 1u);
    EXPECT_EQ(heads[0].peak_bytes_per_second, 312000000u);
    EXPECT_EQ(heads[0].bandwidth_envelope, 0u);
    EXPECT_NE(heads[0].windows[0].active, 0u);
    EXPECT_EQ(heads[0].windows[1].active, 0u);
    EXPECT_EQ(heads[0].windows[2].active, 0u);
    EXPECT_EQ(logs.size(), 1u);
    display.applyUiMemoryPlan();
    EXPECT_EQ(heads.size(), 1u) << "held: the latch after a flip publishes nothing";
}

TEST_F(DisplayAdmission, AnOverlaysPlanIsNotReplacedAtTheFirstLatch)
{
    ASSERT_TRUE(display.validatePlaneBandwidth(present, layers, 720, 1280));
    ASSERT_EQ(heads.size(), 1u);
    display.applyUiMemoryPlan();
    EXPECT_EQ(heads.size(), 1u);
    Display other;
    other.overlaysIdle = false;
    other.applyUiMemoryPlan();
    EXPECT_EQ(heads.size(), 1u) << "an overlay armed with the first frame publishes its own plan";
}

TEST_F(DisplayAdmission, RefusedPlanIsLoggedAndTriedAtTheNextFirstLatch)
{
    reserveError = -95;
    display.applyUiMemoryPlan();
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_NE(logs[0].find("refused"), std::string::npos);
    EXPECT_FALSE(display.displayMemory.holding());
    reserveError = 0;
    display.applyUiMemoryPlan();
    EXPECT_EQ(heads.size(), 2u);
    EXPECT_TRUE(display.displayMemory.holding());
}

TEST_F(DisplayAdmission, NoModeNoPlan)
{
    display.displayMemory.invalidate();
    display.applyUiMemoryPlan();
    EXPECT_TRUE(heads.empty());
    EXPECT_TRUE(logs.empty()) << "a timing that cannot be read is logged where it is captured";
}

}  // namespace
