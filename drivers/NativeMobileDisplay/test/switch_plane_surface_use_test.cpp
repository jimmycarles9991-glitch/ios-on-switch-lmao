// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include "../SwitchSurfaceUse.h"
#include "bazel/testing/source_text.h"

namespace {
struct Lease {};

struct Display {
    static constexpr unsigned kPlaneSlots = 3;

    struct Native {
        Lease *lease;
    };

    struct Slot {
        Native *native;
        uint64_t mappedAllocation, mappedGeneration;
    };

    struct PlaneSet {
        int slot = -1;

        bool uses(int k) const
        {
            return slot == k;
        }
    };

    struct Latch {
        int active = -1;
    };

    Slot planeSlots[kPlaneSlots] = {};
    PlaneSet sets[2] = {};
    PlaneSet *planeSets = sets;
    Latch planeLatch;

    PlaneSet &planeSetOf(int set)
    {
        return sets[set >= 0 ? set : 1];
    }

#include "../SwitchPlaneSurfaceUseDisplay.h"

    ~Display()
    {
        releasePlaneUses();
    }
};

class PlaneSurfaceUse : public testing::Test {
protected:
    Display display;
    Lease lease;
    Display::Native natives[Display::kPlaneSlots] = {{&lease}, {&lease}, {&lease}};
    OSObject buffers[3];

    void SetUp() override
    {
        usesChanged = 0;
        for (unsigned k = 0; k < Display::kPlaneSlots; ++k)
            display.planeSlots[k] = {&natives[k], 100 + k, 1};
    }

    void map(int slot, OSObject &surface)
    {
        EXPECT_EQ(display.planeUseNoted(slot, &surface), slot);
    }

    // The latch that shows `slot` on window B, as planeFrameEnd's planeReleaseIdle sees it.
    void latch(int slot)
    {
        display.sets[0].slot = slot;
        display.planeLatch.active = 0;
        display.planeUseHoldScanned();
    }
};

TEST_F(PlaneSurfaceUse, TheSurfaceOnScreenReadsInUseFromItsLatchUntilItsLeaseGoes)
{
    map(0, buffers[0]);
    EXPECT_EQ(buffers[0].uses, 0);
    latch(0);
    EXPECT_EQ(buffers[0].uses, 1);
    EXPECT_EQ(buffers[0].refs, 3);
    latch(0);
    EXPECT_EQ(buffers[0].uses, 1);
    display.planeUseRelease(0);
    EXPECT_EQ(buffers[0].uses, 0);
    EXPECT_EQ(buffers[0].refs, 1);
}

TEST_F(PlaneSurfaceUse, ThreePoolBuffersGoRoundAndOnlyTheOneShownStaysInUse)
{
    for (int frame = 0; frame < 9; ++frame) {
        const int slot = frame % 3;
        map(slot, buffers[slot]);
        latch(slot);
        const int previous = (frame + 2) % 3;
        if (frame)
            display.planeUseRelease(unsigned(previous));
        EXPECT_EQ(buffers[slot].uses, 1) << frame;
        EXPECT_EQ(buffers[(frame + 1) % 3].uses, 0) << frame;
        if (frame)
            EXPECT_EQ(buffers[previous].uses, 0) << frame;
    }
    for (unsigned k = 0; k < Display::kPlaneSlots; ++k)
        display.planeUseRelease(k);
    for (auto &buffer : buffers) {
        EXPECT_EQ(buffer.uses, 0);
        EXPECT_EQ(buffer.refs, 1);
    }
}

TEST_F(PlaneSurfaceUse, ASetWrittenButNotLatchedHoldsNothing)
{
    map(1, buffers[1]);
    display.sets[0].slot = 1;
    display.planeLatch.active = -1;
    display.planeUseHoldScanned();
    EXPECT_EQ(buffers[1].uses, 0);
    display.planeUseRelease(1);
    EXPECT_EQ(buffers[1].refs, 1);
}

TEST_F(PlaneSurfaceUse, ASlotRemappedToAnotherAllocationOrWithoutItsLeaseHoldsNothing)
{
    map(0, buffers[0]);
    display.planeSlots[0].mappedAllocation = 900;
    latch(0);
    EXPECT_EQ(buffers[0].uses, 0);
    map(2, buffers[2]);
    natives[2].lease = nullptr;
    latch(2);
    EXPECT_EQ(buffers[2].uses, 0);
    display.planeUseRelease(0);
    display.planeUseRelease(2);
    EXPECT_EQ(buffers[0].refs, 1);
    EXPECT_EQ(buffers[2].refs, 1);
}

TEST_F(PlaneSurfaceUse, ASurfaceThatProhibitsUseCountsIsHeldWithoutTouchingItsCount)
{
    reinterpret_cast<unsigned char *>(&buffers[0])[OriginalIOSurface::useCountFlags] =
        switch_display::kProhibitUseCount;
    map(0, buffers[0]);
    latch(0);
    display.planeUseRelease(0);
    EXPECT_EQ(usesChanged, 0u);
    EXPECT_EQ(buffers[0].refs, 1);
}

TEST_F(PlaneSurfaceUse, EveryPathALeaseEndsByReleasesItsCount)
{
    const std::string slots = switch_ios::test::SourceText("../SwitchPlaneSlots.h");
    const std::string native = switch_ios::test::SourceText("../SwitchNativePlaneDisplay.h");
    const std::string display = switch_ios::test::SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_THAT(slots, testing::HasSubstr("void planeDropSlot(unsigned k)\n{\n    planeUseRelease(k);"));
    EXPECT_THAT(slots, testing::HasSubstr("void planeReleaseLease(unsigned k)\n{\n    planeUseRelease(k);"));
    EXPECT_THAT(slots, testing::HasSubstr("void planeReleaseIdle()\n{\n    planeUseHoldScanned();"));
    EXPECT_THAT(native, testing::HasSubstr("= planeUseNoted(planeMapSnapshot(scanout, image), surface);"));
    EXPECT_THAT(native, testing::HasSubstr("planeUseRelease(unsigned(mapped));\n"
                                           "        switch_native_scanout_deliver(slot.native);\n"
                                           "        switch_native_scanout_release(slot.native);"));
    EXPECT_THAT(display, testing::HasSubstr("#include \"SwitchPlaneSurfaceUseDisplay.h\""));
}
}  // namespace
