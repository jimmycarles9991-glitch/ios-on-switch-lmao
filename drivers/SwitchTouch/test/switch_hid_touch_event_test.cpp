// Host tests for direct digitizer IOHIDEvent planning and calibrated coordinate scaling (SwitchHIDTouchEvent.h).
#include <gtest/gtest.h>
#include "../SwitchHIDTouchEvent.h"

namespace {
SwitchTouchReport makeReport()
{
    SwitchTouchReport r = {};
    r.id = 1;
    for (unsigned i = 0; i < kSwitchTouchSlots; ++i)
        r.slot[i].contact = static_cast<uint8_t>(i);
    return r;
}

void setSlot(SwitchTouchReport &r, unsigned slot, bool down, uint16_t x, uint16_t y)
{
    r.slot[slot].flags =
        static_cast<uint8_t>(kSwitchTouchValid | (down ? (kSwitchTouchTip | kSwitchTouchInRange) : 0u));
    r.slot[slot].x = x;
    r.slot[slot].y = y;
}
}  // namespace

TEST(SwitchHIDTouchEvent, ScalesCalibratedCoordinatesAndClamps)
{
    EXPECT_EQ(switch_hid_touch_scale(0, 719), 0);
    EXPECT_EQ(switch_hid_touch_scale(719, 719), 65536);
    EXPECT_EQ(switch_hid_touch_scale(900, 719), 65536);
    EXPECT_EQ(switch_hid_touch_scale(1279, 1279), 65536);
    EXPECT_EQ(switch_hid_touch_scale(360, 719), static_cast<int32_t>((360LL << 16) / 719));
}

TEST(SwitchHIDTouchEvent, EmptyReportProducesNoEvent)
{
    SwitchHIDTouchState state = {};
    SwitchHIDTouchPlan plan = {};
    const SwitchTouchReport r = makeReport();
    EXPECT_FALSE(switch_hid_touch_plan(r, 0, state, plan));
    EXPECT_EQ(plan.count, 0u);
}

TEST(SwitchHIDTouchEvent, SingleFingerEmitsOnlyOneChildThroughLifecycle)
{
    SwitchHIDTouchState state = {};
    SwitchHIDTouchPlan plan = {};

    SwitchTouchReport r = makeReport();
    setSlot(r, 3, true, 100, 200);
    ASSERT_TRUE(switch_hid_touch_plan(r, 1u << 3, state, plan));
    ASSERT_EQ(plan.count, 1u);
    EXPECT_TRUE(plan.anyDown);
    EXPECT_EQ(plan.fingers[0].index, 3u);
    EXPECT_TRUE(plan.fingers[0].down);
    EXPECT_EQ(plan.fingers[0].eventMask, kSwitchHidDigitizerMaskRange | kSwitchHidDigitizerMaskTouch |
                                             kSwitchHidDigitizerMaskPosition | kSwitchHidDigitizerMaskFromEdgeTip);
    EXPECT_EQ(plan.collectionMask, plan.fingers[0].eventMask);
    EXPECT_EQ(plan.centroidX, plan.fingers[0].fx);
    EXPECT_EQ(plan.centroidY, plan.fingers[0].fy);

    // Stationary repeat at same coordinates: no Range, Touch, or Position bits; edge bit stays.
    ASSERT_TRUE(switch_hid_touch_plan(r, 1u << 3, state, plan));
    ASSERT_EQ(plan.count, 1u);
    EXPECT_TRUE(plan.anyDown);
    EXPECT_EQ(plan.fingers[0].eventMask, kSwitchHidDigitizerMaskFromEdgeTip);

    // Move: Position bit set, no Range/Touch bits.
    setSlot(r, 3, true, 140, 260);
    ASSERT_TRUE(switch_hid_touch_plan(r, 1u << 3, state, plan));
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.fingers[0].eventMask, kSwitchHidDigitizerMaskPosition | kSwitchHidDigitizerMaskFromEdgeTip);
    const int32_t movedX = plan.centroidX;
    const int32_t movedY = plan.centroidY;

    // Lift: emits 1 lifted child with Range|Touch (no Position) and preserves last centroid.
    setSlot(r, 3, false, 140, 260);
    ASSERT_TRUE(switch_hid_touch_plan(r, 1u << 3, state, plan));
    ASSERT_EQ(plan.count, 1u);
    EXPECT_FALSE(plan.anyDown);
    EXPECT_FALSE(plan.fingers[0].down);
    EXPECT_EQ(plan.fingers[0].eventMask,
              kSwitchHidDigitizerMaskRange | kSwitchHidDigitizerMaskTouch | kSwitchHidDigitizerMaskFromEdgeTip);
    EXPECT_EQ(plan.centroidX, movedX);
    EXPECT_EQ(plan.centroidY, movedY);

    // Subsequent all-up report emits nothing (no duplicate untouch).
    r = makeReport();
    EXPECT_FALSE(switch_hid_touch_plan(r, 0, state, plan));
}

TEST(SwitchHIDTouchEvent, MultiFingerCentroidAndPartialLift)
{
    SwitchHIDTouchState state = {};
    SwitchHIDTouchPlan plan = {};

    SwitchTouchReport r = makeReport();
    setSlot(r, 0, true, 100, 200);
    setSlot(r, 2, true, 300, 600);
    ASSERT_TRUE(switch_hid_touch_plan(r, 0, state, plan));
    ASSERT_EQ(plan.count, 2u);
    EXPECT_TRUE(plan.anyDown);
    EXPECT_EQ(plan.fingers[0].index, 0u);
    EXPECT_EQ(plan.fingers[1].index, 2u);
    const int32_t expectedCx = (plan.fingers[0].fx + plan.fingers[1].fx) / 2;
    const int32_t expectedCy = (plan.fingers[0].fy + plan.fingers[1].fy) / 2;
    EXPECT_EQ(plan.centroidX, expectedCx);
    EXPECT_EQ(plan.centroidY, expectedCy);

    // Lift slot 0 while slot 2 moves: both appear in this report (one lifted, one moved), centroid follows slot 2.
    setSlot(r, 0, false, 100, 200);
    setSlot(r, 2, true, 350, 650);
    ASSERT_TRUE(switch_hid_touch_plan(r, 0, state, plan));
    ASSERT_EQ(plan.count, 2u);
    EXPECT_TRUE(plan.anyDown);
    EXPECT_FALSE(plan.fingers[0].down);
    EXPECT_EQ(plan.fingers[0].eventMask, kSwitchHidDigitizerMaskRange | kSwitchHidDigitizerMaskTouch);
    EXPECT_TRUE(plan.fingers[1].down);
    EXPECT_EQ(plan.fingers[1].eventMask, kSwitchHidDigitizerMaskPosition);
    EXPECT_EQ(plan.collectionMask,
              kSwitchHidDigitizerMaskRange | kSwitchHidDigitizerMaskTouch | kSwitchHidDigitizerMaskPosition);
    EXPECT_EQ(plan.centroidX, plan.fingers[1].fx);
    EXPECT_EQ(plan.centroidY, plan.fingers[1].fy);

    // Next report with only slot 2 still down: slot 0 is no longer emitted!
    r = makeReport();
    setSlot(r, 2, true, 350, 650);
    ASSERT_TRUE(switch_hid_touch_plan(r, 0, state, plan));
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.fingers[0].index, 2u);
    EXPECT_EQ(plan.fingers[0].eventMask, 0u);
}
