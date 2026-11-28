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

TEST(SwitchTouchSource, DirectTouchEventContainsBuildOffsetsAndNoHotPathLogs)
{
    const std::string source = SourceText("../SwitchHIDTouchEvent.cpp");
    EXPECT_THAT(source, HasSubstr("SWITCH_IPSW_KIND_21H461"));
    EXPECT_THAT(source, HasSubstr("SWITCH_IPSW_KIND_24B119"));
    EXPECT_THAT(source, HasSubstr("#error"));
    const std::string alloc = Body(source, "OSObject *allocDigitizerEvent(");
    ASSERT_FALSE(alloc.empty());
    EXPECT_THAT(alloc, Not(HasSubstr("os_log")));
    EXPECT_THAT(alloc, Not(HasSubstr("kprintf")));
    const std::string direct = Between(SourceText("../SwitchHIDDevice.cpp"), "    bool postTouchDirect(", "\n    }\n");
    ASSERT_FALSE(direct.empty());
    EXPECT_THAT(direct, Not(HasSubstr("runAction")));
    EXPECT_THAT(direct, Not(HasSubstr("IOLog")));
}

TEST(SwitchTouchSource, PostAtCopiesReportInsideWorkLoopAction)
{
    const std::string source = SourceText("../SwitchHIDDevice.cpp");
    const std::string post =
        Between(source, "    IOReturn postAt(const void *report, size_t length, uint64_t timestamp)", "\n    }\n");
    ASSERT_FALSE(post.empty());
    const size_t runAction = post.find("loop->runAction(");
    const size_t copy = post.find("memcpy(");
    ASSERT_NE(runAction, std::string::npos);
    ASSERT_NE(copy, std::string::npos);
    EXPECT_GT(copy, runAction);
    EXPECT_EQ(post.find("memcpy(", copy + 1), std::string::npos);
}

TEST(SwitchTouchSource, IdentityGettersAndLocationIDSlot192)
{
    const std::string source = SourceText("../SwitchHIDDevice.cpp");
    for (const char *method :
         {"OSString *newManufacturerString() const override", "OSNumber *newVendorIDNumber() const override",
          "OSNumber *newProductIDNumber() const override", "OSNumber *newLocationIDNumber() const override"}) {
        EXPECT_THAT(source, HasSubstr(method)) << method;
    }
    const std::string iface = SourceText("../../../kits/SwitchABI/interface/IOHIDDevice.json");
    EXPECT_THAT(iface, HasSubstr("\"newLocationIDNumber\""));
    EXPECT_THAT(iface, HasSubstr("\"__ZNK11IOHIDDevice19newLocationIDNumberEv\""));
    for (const char *kind : {"21H461", "24B119"}) {
        const std::string table =
            SourceText(std::string("../../../kits/SwitchABI/kinds/") + kind + "/IOHIDDevice.json");
        EXPECT_THAT(table, HasSubstr("\"192\": \"newLocationIDNumber\"")) << kind;
        EXPECT_THAT(table, HasSubstr("\"newLocationIDNumber\": \"0xfffffff0")) << kind;
        EXPECT_THAT(table, Not(HasSubstr("\"opaque192\""))) << kind;
    }
}

TEST(SwitchTouchSource, LightSensorPropertiesCopyEventAndSharedWorkLoop)
{
    const std::string source = SourceText("../SwitchHIDDevice.cpp");
    const std::string lightProps = Body(source, "void addLightProperties(");
    ASSERT_FALSE(lightProps.empty());
    EXPECT_THAT(lightProps, HasSubstr("{\"ALSSensorType\", 0}"));
    EXPECT_THAT(lightProps, HasSubstr("{\"ReportInterval\", kSwitchLightReportIntervalUs}"));

    const std::string start = Between(source, "    bool start(IOService *provider) override", "\n    }\n");
    ASSERT_FALSE(start.empty());
    EXPECT_THAT(start, HasSubstr("kSwitchLightReportIntervalUs"));
    EXPECT_THAT(start, HasSubstr("sharedLoop->retain();"));

    const std::string copyEvent = Body(source, "OSObject *copyEvent(");
    ASSERT_FALSE(copyEvent.empty());
    EXPECT_THAT(copyEvent, HasSubstr("kAmbientLightType"));
    EXPECT_THAT(copyEvent, HasSubstr("entries[i].eventType == type"));

    const std::string deliver = Between(source, "    IOReturn deliver(OSObject *event)", "\n    }\n");
    ASSERT_FALSE(deliver.empty());
    EXPECT_THAT(deliver, HasSubstr("cacheEvent(event);"));
    EXPECT_THAT(deliver, HasSubstr("temperature::ensureLightHooked(eventDriver, this);"));

    const std::string create = Body(source, "IOService *switch_hid_create(");
    ASSERT_FALSE(create.empty());
    EXPECT_THAT(create, HasSubstr("temperature::prepare();"));
    EXPECT_THAT(create, HasSubstr("initWithProfile(&profile, sharedLoop)"));
    EXPECT_THAT(source, HasSubstr("IOService *switch_hid_create(SwitchHIDKind kind)"));
    EXPECT_THAT(source, HasSubstr("void switch_hid_set_light_copy_handler("));
}

TEST(SwitchTouchSource, JoyConTouchTimestampHookOnLiveDispatchOnly)
{
    const std::string touch = SourceText("../SwitchTouch.cpp");
    EXPECT_THAT(touch, HasSubstr("#include \"../SwitchJoyCon/SwitchJoyConTouch.h\""));
    EXPECT_THAT(touch, HasSubstr("extern \"C\" __attribute__((weak)) void switch_joycon_note_touch_ms(uint64_t)"));
    const std::string flush = Body(touch, "void SwitchTouch::flush(");
    ASSERT_FALSE(flush.empty());
    EXPECT_THAT(flush, HasSubstr("if (live && activeCount)"));
    EXPECT_THAT(flush, HasSubstr("switch_joycon_note_touch_ms(stampUs / 1000);"));
    const std::string release = Body(touch, "void SwitchTouch::releaseAll(");
    ASSERT_FALSE(release.empty());
    EXPECT_THAT(release, HasSubstr("flush(true, 0, false);"));
}

TEST(SwitchTouchSource, SharesWorkLoopWithHidDevice)
{
    const std::string start = Body(SourceText("../SwitchTouch.cpp"), "bool SwitchTouch::start(");
    ASSERT_FALSE(start.empty());
    const size_t loopAt = start.find("loop = IOWorkLoop::workLoop();");
    const size_t createAt = start.find("switch_hid_create(kSwitchHIDTouch, loop)");
    const size_t armAt = start.find("timer->setTimeoutMS(1);");
    ASSERT_NE(loopAt, std::string::npos);
    ASSERT_NE(createAt, std::string::npos);
    ASSERT_NE(armAt, std::string::npos);
    EXPECT_EQ(switch_ios::test::Count(start, "switch_hid_create("), 1u);
    EXPECT_GT(createAt, loopAt);
    EXPECT_LT(createAt, armAt);
}
