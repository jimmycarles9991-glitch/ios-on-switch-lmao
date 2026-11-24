// Guards on SwitchTouch's source for what switch_touch_test's models cannot see: nothing in the touch interrupt's
// action waits (the model's clock does not run while code spins), a leave's coordinates are never used (hekate: they
// are noise), the palm threshold is Linux's, SwitchHIDDevice's timed posts keep the edge gestures (the HID device is a
// stand-in in the scenarios), and its touch descriptor gives X and Y the panel's millimetres.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchTouchSource, InterruptActionNeverWaits)
{
    const std::string action = Body(SourceText("../SwitchTouch.cpp"), "void SwitchTouch::interruptOccurred(");
    ASSERT_FALSE(action.empty());
    for (const char *wait : {"IOSleep", "IODelay", "while (", "for (unsigned round"})
        EXPECT_THAT(action, Not(HasSubstr(wait)));
}

TEST(SwitchTouchSource, LeaveCoordinatesUnused)
{
    const std::string handle = Body(SourceText("../SwitchTouch.cpp"), "void SwitchTouch::handle(");
    const std::string leave = Between(handle, "if (type == EvLeave)", "return;\n    }");
    ASSERT_FALSE(leave.empty());
    EXPECT_THAT(leave, Not(HasSubstr("position("))) << "a leave's coordinates are noise (hekate)";
}

TEST(SwitchTouchSource, PalmThresholdIsLinuxPressureMax)
{
    // Linux ftm4 PRESSURE_MAX, hekate's palm rest.
    EXPECT_THAT(SourceText("../SwitchTouch.cpp"), HasSubstr("kPalmStrength = 500"));
}

TEST(SwitchTouchSource, TimedPostsKeepEdgeGestures)
{
    const std::string source = SourceText("../SwitchHIDDevice.cpp");
    EXPECT_THAT(Body(source, "    IOReturn postAt("), HasSubstr("installEdge();"));
    EXPECT_THAT(Body(source, "    IOReturn post(const void *report, size_t length)"), HasSubstr("installEdge();"));
}

TEST(SwitchTouchSource, TouchAxesCarryThePanelsMillimetres)
{
    const std::string finger = Between(SourceText("../SwitchHIDDevice.cpp"), "#define FINGER", "0xc0\n");
    ASSERT_FALSE(finger.empty());
    // Physical Maximum, Unit cm (SI length), Unit Exponent -2 on X; Y's own maximum; no unit after the axes.
    EXPECT_THAT(finger, HasSubstr("0x46, TOUCH_X_SIZE, 0x65, 0x11, 0x55, 0x0e, 0x81, 0x02"));
    EXPECT_THAT(finger, HasSubstr("0x46, TOUCH_Y_SIZE, 0x81, 0x02"));
    EXPECT_THAT(finger, HasSubstr("0x45, 0x00, 0x65, 0x00, 0x55, 0x00"));
    EXPECT_THAT(SourceText("../SwitchHIDDevice.cpp"), HasSubstr("TOUCH_X_SIZE 0x66, 0x03 /* 870: 87.0 mm */"));
}

// A report's own time on both iOS builds: postAt calls handleReportWithTime, whose slot each kind's table names
// (kits/SwitchABI/kinds/<kind>/IOHIDDevice.json, read from that kernelcache), not code words read from iOS 17's
// handleReport, which made every iOS 27 report carry the post's time instead (touch "stamps post") and turned motion
// batching off.
TEST(SwitchTouchSource, TimedPostsTakeTheTablesCallOnEveryBuild)
{
    const std::string source = SourceText("../SwitchHIDDevice.cpp");
    for (const char *word : {"0x3942c268", "0xf94007e1", "0xf9431508", "timestamps()", "kHandleReportWithTime"})
        EXPECT_THAT(source, Not(HasSubstr(word))) << word;
    // The method alone: it ends at the class body's indentation.
    const std::string post =
        Between(source, "    IOReturn postAt(const void *report, size_t length, uint64_t timestamp)", "\n    }\n");
    ASSERT_FALSE(post.empty());
    EXPECT_THAT(post, HasSubstr("self->handleReportWithTime("));
    EXPECT_THAT(post, Not(HasSubstr("kIOReturnUnsupported")));
    for (const char *kind : {"21H461", "24B119"}) {
        const std::string table =
            SourceText(std::string("../../../kits/SwitchABI/kinds/") + kind + "/IOHIDDevice.json");
        EXPECT_THAT(table, HasSubstr("\"handleReportWithTime\"")) << kind;
        EXPECT_THAT(table, Not(HasSubstr("\"handleReportWithTime\": \"0x0\""))) << kind;
    }
}
