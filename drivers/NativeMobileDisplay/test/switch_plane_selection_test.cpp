// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include "../SwitchDisplayComposition.h"
#include "../SwitchLandscapeWindows.h"
#include "../../TegraGM20B/SwitchGpuBacking.h"

namespace {
struct OSObject {
    OSObject *metaCast(const char *)
    {
        return this;
    }
} surfaces[4];

struct OriginalLegacySwapRequest {
    uint32_t updated = 0, ready = 0;
    OSObject *effective[4] = {};
};

struct Display {
    using PlaneSet = switch_display::Composition;
    static constexpr unsigned swapUpdated = 0, swapReady = 4, swapEffectiveSurfaces = 8;

    struct PlaneLayerRecord {
        OSObject *surface = nullptr;
        native_planes::Layer layer;
    } planeLayers[4];

    struct PlaneDecision {
        OSObject *ui;
        bool uiNew, blank, planeChange;
        bool waitSlot;
        PlaneSet set;
    };

    bool stats = false, planesOn = true;
    OSObject *planeShownUi = nullptr;
    unsigned stPlaneUnready = 0, stPlaneDropped = 0, conversions = 0;
    int mapError = 0, convertError = 0;
    uint32_t reservedOnSecond = 0;
    PlaneSet planeTarget;
    switch_display::WindowState live[2] = {};
    std::array<int, 4> mapping = {0, 1, 2, 3};

    void getDisplayDimensions(uint32_t *w, uint32_t *h)
    {
        *w = 720;
        *h = 1280;
    }

    native_planes::Layer planeLayerOf(const unsigned char *, unsigned i, OSObject *)
    {
        return planeLayers[i].layer;
    }

    native_planes::Surface planeSurfaceOf(OSObject *)
    {
        return {};
    }

    using Window = switch_landscape::Window;

    switch_display::WindowState planeWindowState(Window w) const
    {
        return live[w - 1];
    }

    int planeNativeSlotFor(OSObject *surface, const native_planes::Layer &, uint32_t, uint32_t,
                           native_planes::Image &image, bool &resolved)
    {
        resolved = true;
        if (mapError)
            return mapError;
        if (planeBuildingSlots)
            reservedOnSecond = planeBuildingSlots;
        const auto i = unsigned(surface - surfaces);
        image.visible = true;
        image.yuv = native_planes::isNv12(planeLayers[i].layer.kind);
        image.matrix = native_planes::Matrix(planeLayers[i].layer.surface.colorMatrix);
        image.options = native_planes::kWinEnable;
        return mapping[i];
    }

    int planeConvertSlot(int slot, const native_planes::Layer &, uint32_t, uint32_t, native_planes::Image &image,
                         const OriginalLegacySwapRequest *)
    {
        ++conversions;
        if (convertError)
            return convertError;
        image.yuv = false;
        return slot + 4;
    }

    bool planeLandscapeDecide(const native_planes::Roles &, uint32_t, uint32_t, PlaneDecision &)
    {
        return false;
    }

    int composite = -1;
    bool compositeWaits = false;
    uint32_t composedFrom = 0;

    bool planeComposeWaits(switch_display::Overlay wanted[2], const int *, uint32_t, uint32_t)
    {
        if (compositeWaits || composite < 0)
            return compositeWaits;
        composedFrom = planeBuildingSlots;
        native_planes::Image image;
        image.visible = true;
        image.options = native_planes::kWinEnable;
        wanted[0] = {true, composite, image, 1};
        wanted[1] = {};
        return false;
    }

#include "../SwitchPlaneSelection.h"
};

native_planes::Layer layer(native_planes::Kind kind, bool ui = false)
{
    native_planes::Layer result;
    result.kind = kind;
    result.surface.width = 720;
    result.surface.height = 1280;
    result.src = result.dst = {0, 0, ui ? 720 : 640, ui ? 1280 : 480};
    return result;
}

OriginalLegacySwapRequest frame(Display &display)
{
    OriginalLegacySwapRequest request;
    for (unsigned i = 0; i != 3; ++i) {
        request.effective[i] = display.planeLayers[i].surface = &surfaces[i];
        display.planeLayers[i].layer = layer(i == 2 ? native_planes::kKindNv12Video : native_planes::kKindBgra, i == 1);
    }
    return request;
}

TEST(PlaneSelection, UiAndTwoDetachedLayersPreserveFamilyStackDepth)
{
    Display display;
    const auto request = frame(display);
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    ASSERT_FALSE(decision.waitSlot);
    EXPECT_EQ(decision.ui, &surfaces[1]);
    EXPECT_TRUE(decision.uiNew);
    EXPECT_TRUE(decision.planeChange);
    EXPECT_EQ(decision.set.window[0].slot, 0);
    EXPECT_EQ(decision.set.window[0].depth, 1u);
    EXPECT_EQ(decision.set.window[1].slot, 2);
    EXPECT_EQ(decision.set.window[1].depth, 2u);
    EXPECT_EQ(display.reservedOnSecond, 1u << 0);
    EXPECT_EQ(display.planeBuildingSlots, 0u);
    EXPECT_EQ(display.stPlaneDropped, 0u);
}

TEST(PlaneSelection, ProducerUpdateKeepsUnchangedUiAndOtherOverlay)
{
    Display display;
    auto request = frame(display);
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    display.planeTarget = decision.set;
    display.planeShownUi = decision.ui;
    display.live[0].slot = 0;
    display.live[1].slot = 2;
    display.planeDecide(&request, decision);
    EXPECT_FALSE(decision.uiNew);
    EXPECT_FALSE(decision.planeChange);
    display.mapping[2] = 3;
    request.updated = request.ready = 1u << 2;
    display.planeDecide(&request, decision);
    EXPECT_FALSE(decision.uiNew);
    EXPECT_TRUE(decision.planeChange);
    EXPECT_TRUE(decision.set.uses(0));
    EXPECT_TRUE(decision.set.uses(3));
    EXPECT_FALSE(decision.set.uses(2));
}

TEST(PlaneSelection, BusySourceKeepsTheCurrentFrameForTheNextFrameEnd)
{
    for (int busy : {-16, -11, -115, int(SWITCH_GPU_BACKING_STALE)}) {
        Display display;
        const auto request = frame(display);
        display.mapError = busy;
        Display::PlaneDecision decision;
        display.planeDecide(&request, decision);
        EXPECT_TRUE(decision.waitSlot) << busy;
        EXPECT_FALSE(decision.planeChange);
        EXPECT_EQ(display.planeBuildingSlots, 0u);
    }
}

TEST(PlaneSelection, LayerThatCannotBePlacedIsDroppedAndTheUiStillShows)
{
    for (int permanent : {-19, -95, -22, -12, -1}) {
        Display display;
        display.stats = true;
        const auto request = frame(display);
        display.mapError = permanent;
        Display::PlaneDecision decision;
        display.planeDecide(&request, decision);
        EXPECT_FALSE(decision.waitSlot) << permanent;
        EXPECT_EQ(decision.ui, &surfaces[1]);
        EXPECT_TRUE(decision.uiNew);
        EXPECT_FALSE(decision.set.on());
        EXPECT_EQ(display.stPlaneDropped, 2u);
    }
}

TEST(PlaneSelection, ConversionThatCannotRunDropsOnlyItsOverlay)
{
    Display display;
    display.stats = true;
    const auto request = frame(display);
    display.live[0] = {true, 5, native_planes::kBt709, 0};
    display.live[1] = {true, 6, native_planes::kBt709, 0};
    display.convertError = -95;
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    EXPECT_FALSE(decision.waitSlot);
    EXPECT_TRUE(decision.set.uses(0));
    EXPECT_FALSE(decision.set.window[0].on && decision.set.window[1].on);
    EXPECT_EQ(display.stPlaneDropped, 1u);
}

TEST(PlaneSelection, LiveCscConflictQueuesConversionAndKeepsOldComposition)
{
    Display display;
    const auto request = frame(display);
    display.live[0] = {true, 5, native_planes::kBt709, 0};
    display.live[1] = {true, 6, native_planes::kBt709, 0};
    display.convertError = -115;
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    EXPECT_EQ(display.conversions, 1u);
    EXPECT_TRUE(decision.waitSlot);
    EXPECT_FALSE(decision.planeChange);
    EXPECT_EQ(display.planeBuildingSlots, 0u);
}

TEST(PlaneSelection, ConvertedOverlayStillKeepsStackOrderAndMappedReservation)
{
    Display display;
    const auto request = frame(display);
    display.live[0] = {true, 5, native_planes::kBt709, 0};
    display.live[1] = {true, 6, native_planes::kBt709, 0};
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    EXPECT_EQ(display.conversions, 1u);
    EXPECT_FALSE(decision.waitSlot);
    EXPECT_TRUE(decision.set.uses(6));
    EXPECT_FALSE(decision.set.window[1].image.yuv);
    EXPECT_EQ(decision.set.window[1].depth, 2u);
    EXPECT_EQ(display.planeBuildingSlots, 0u);
}

TEST(PlaneSelection, RefusedOverlaysGoOutAsTheirCompositeAlone)
{
    Display display;
    display.stats = true;
    const auto request = frame(display);
    display.live[0] = {true, 5, native_planes::kBt709, 0};
    display.live[1] = {true, 6, native_planes::kBt709, 0};
    display.composite = 7;
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    ASSERT_FALSE(decision.waitSlot);
    EXPECT_EQ(display.composedFrom, (1u << 0) | (1u << 2));
    EXPECT_EQ(display.conversions, 0u);
    EXPECT_TRUE(decision.set.uses(7));
    EXPECT_FALSE(decision.set.uses(0));
    EXPECT_FALSE(decision.set.uses(2));
    EXPECT_FALSE(decision.set.window[0].on && decision.set.window[1].on);
    EXPECT_EQ(display.stPlaneDropped, 0u);
}

TEST(PlaneSelection, ACompositeOnItsWayKeepsTheCurrentFrame)
{
    Display display;
    const auto request = frame(display);
    display.compositeWaits = true;
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    EXPECT_TRUE(decision.waitSlot);
    EXPECT_FALSE(decision.planeChange);
    EXPECT_EQ(display.planeBuildingSlots, 0u);
}

TEST(PlaneSelection, FractionalSamplingChangeIsNotMistakenForStaticContent)
{
    Display display;
    auto request = frame(display);
    Display::PlaneDecision decision;
    display.planeDecide(&request, decision);
    auto other = decision.set;
    other.window[0].image.hInitial = 1;
    EXPECT_FALSE(Display::planeSame(decision.set, other));
    other = decision.set;
    other.window[1].image.vInitial = 1;
    EXPECT_FALSE(Display::planeSame(decision.set, other));
}
}  // namespace
