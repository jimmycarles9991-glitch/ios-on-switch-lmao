// Guards for what switch_motion_test's models cannot see: the sample and FIFO drain paths never wait or poll (the
// models' clock does not run while code spins), the motion services' HID descriptor (SwitchHIDDevice.cpp, a stand-in in
// the scenarios) carries the Sensor-page usages 21H461's IOHIDEventDriver knows, the orientation device serves the
// descriptor orientation_pipeline_test checks, SwitchBacklight follows the panel's power into the orientation
// detector, the device motion service's event driver takes CoreMotion's fast path (SwitchHIDFastPath.cpp), and the
// motion services carry J181's Motion & Fitness gate (SwitchHIDMotionAccess.cpp).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchMotionSource, SamplePathsNeverWait)
{
    const std::string source = SourceText("../SwitchMotion.cpp");
    for (const char *signature :
         {"void SwitchMotion::sample(", "void SwitchMotion::drainFifo(", "void SwitchMotion::fuse("}) {
        const std::string body = Body(source, signature);
        ASSERT_FALSE(body.empty()) << signature;
        for (const char *wait : {"IOSleep", "IODelay", "while (", "for (;;)"})
            EXPECT_THAT(body, Not(HasSubstr(wait))) << signature << " " << wait;
    }
}

// Report Interval 0x30e and Report Latency 0x31b; 0x316 is Reporting State, which no element takes (BatchInterval never
// reached the driver and FIFO batching never ran before r17).
TEST(SwitchMotionSource, SensorPropertyUsages)
{
    const std::string motion = Between(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "#define MOTION_DESCRIPTOR",
                                       "#undef MOTION_DESCRIPTOR");
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x0e, 0x03,"));
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x1b, 0x03,"));
    EXPECT_THAT(motion, Not(HasSubstr("0x0a, 0x16, 0x03")));
}

// CoreMotion's 6-axis device motion: AppleVendorMotion 0xff0c / 5, the payload as one element of the build's length
// inside an AppleVendor Message collection, then the same Report Interval and Report Latency features.
TEST(SwitchMotionSource, DeviceMotionDescriptor)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    const std::string descriptor = Between(hid, "const uint8_t kDeviceMotionDescriptor[] = {", "};");
    EXPECT_THAT(descriptor, HasSubstr("0x06, 0x0c, 0xff, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,"));
    EXPECT_THAT(descriptor, HasSubstr("0x06, 0x00, 0xff, 0x09, 0x23, 0xa1, 0x00,"));
    EXPECT_THAT(descriptor, HasSubstr("0x95, kDeviceMotionBytes, 0x81, 0x02, 0xc0,"));
    EXPECT_THAT(descriptor, HasSubstr("0x0a, 0x0e, 0x03,"));
    EXPECT_THAT(descriptor, HasSubstr("0x0a, 0x1b, 0x03,"));
    EXPECT_THAT(
        hid, HasSubstr("{kDeviceMotionDescriptor, sizeof(kDeviceMotionDescriptor), sizeof(SwitchDeviceMotionReport),"));
}

// orientation_pipeline_test checks kSwitchOrientationDescriptor against the report; the device must serve that one.
TEST(SwitchMotionSource, OrientationDeviceServesTheCheckedDescriptor)
{
    EXPECT_THAT(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"),
                HasSubstr("{kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor), "
                          "sizeof(SwitchOrientationReport),"));
}

TEST(SwitchMotionSource, BacklightFollowsPanelPower)
{
    // The weak default, off and on.
    EXPECT_EQ(Count(SourceText("../../SwitchBrightness/SwitchBacklight.cpp"), "switch_motion_display_power("), 3u);
}

// CoreMotion's orientation notifier opens the orientation device with AppleSPULib's AppleSPUHIDDevice plug-in and asks
// LastOrientation through its 'ashd' user client: a 10-byte reply of the echo, the orientation and the sample time.
TEST(SwitchMotionSource, OrientationDeviceAnswersTheAopPlugIn)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    EXPECT_THAT(hid, HasSubstr("types->setObject(\"7ACF5332-1A35-4893-87CB-BA64E1887FAE\", plugIn)"));
    EXPECT_THAT(hid, HasSubstr("return switch_motion_orientation_client(this, task, security, type, handler);"));
    EXPECT_THAT(hid, HasSubstr("return IOHIDDevice::newUserClient(task, security, type, properties, handler);"));
    const std::string client = SourceText("../SwitchOrientationClient.cpp");
    EXPECT_THAT(client, HasSubstr("kSelectorReset = 0, kSelectorOpen = 4, kSelectorClose = 5, kSelectorCommand = 6"));
    EXPECT_THAT(client, HasSubstr("constexpr uint32_t kLastSize = 10"));
    EXPECT_THAT(client, HasSubstr("switch_motion_last_orientation(&orientation, &stamp);"));
    const std::string motion = SourceText("../SwitchMotion.cpp");
    EXPECT_THAT(Body(motion, "void SwitchMotion::announce("), HasSubstr("__atomic_store_n(&gLastOrientation,"));
}

// The slots both builds' IOHIDEventServiceFastPathUserClient calls (SwitchHIDFastPath.cpp's file comment), only for the
// builds they were read from, the plug-in type IOHIDServiceClientFastPathInit asks for, and the hook installed from the
// first publish of the device motion device's driver, before it starts.
TEST(SwitchMotionSource, DeviceMotionTakesTheFastPath)
{
    const std::string fast = SourceText("../../SwitchTouch/SwitchHIDFastPath.cpp");
    for (const char *slot : {
             "kDispatch = 198,",
             "kPrimaryUsagePage = 199,",
             "kPrimaryUsage = 200,",
             "kCopyEventForClient = 205,",
             "kCopyPropertyForClient = 206,",
             "kOpenForClient = 208,",
             "kCloseForClient = 209,",
         }) {
        EXPECT_THAT(fast, HasSubstr(slot));
    }
    EXPECT_THAT(fast, HasSubstr("\"0B842ADD-C395-4352-95F6-6585EB515623\""));
    EXPECT_THAT(fast, HasSubstr("\"IOHIDFamily.kext/PlugIns/IOHIDLib.plugin\""));
    EXPECT_THAT(fast, HasSubstr("setProperty(\"NativeSandboxGrantProcess\", \"backboardd,SpringBoard,locationd\")"));
    EXPECT_THAT(fast, HasSubstr("#if SWITCH_IPSW_KIND_24B119 || SWITCH_IPSW_KIND_21H461"));
    EXPECT_THAT(fast, HasSubstr("gIOFirstPublishNotification"));
    EXPECT_THAT(fast, Not(HasSubstr("IOSimpleLock")));
    EXPECT_THAT(fast, Not(HasSubstr("__atomic_fetch_add")));
    const std::string create =
        Body(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "IOService *switch_hid_create(");
    EXPECT_LT(create.find("switch_hid_fastpath_watch(device)"), create.find("device->start(resources)"));
}

TEST(SwitchMotionSource, PrimaryIrqFilterAndPackedConfig)
{
    const std::string motion = SourceText("../SwitchMotion.cpp");
    EXPECT_THAT(motion, HasSubstr("IOFilterInterruptEventSource::filterInterruptEventSource("));
    EXPECT_THAT(motion, Not(HasSubstr("__atomic_fetch_add(&motionReports")));
    EXPECT_THAT(Body(motion, "void SwitchMotion::post("), Not(HasSubstr("__atomic_load_n")));
}

// The event system has the event before a copy can answer it; an open asking for the AOP's always-on history, which
// J181's provider gates on FastPathHasEntitlement, is refused, since nothing here keeps one; and a motion event driver
// published again is hooked again, answering nothing its predecessor dispatched.
TEST(SwitchMotionSource, TheFastPathAnswersOnlyWhatWasDispatched)
{
    const std::string fast = SourceText("../../SwitchTouch/SwitchHIDFastPath.cpp");
    const std::string dispatch = Body(fast, "void dispatch(");
    EXPECT_LT(dispatch.find("(originalDispatch)(driver, event, options)"), dispatch.find("keep(event)"));
    EXPECT_THAT(Body(fast, "bool openForClient("), HasSubstr("getObject(\"aonBuffer\")"));
    EXPECT_THAT(Body(fast, "bool published("), Not(HasSubstr("installed")));
    const std::string install = Body(fast, "bool install(");
    ASSERT_NE(install.find("keep(nullptr)"), std::string::npos);
    EXPECT_LT(install.find("keep(nullptr)"), install.find("= table;"));
}

// J181's Motion & Fitness gate (SwitchHIDMotionAccess.cpp), on 24B119 only: the accelerometer, gyroscope and device
// motion event drivers carry it from their first publish, so IOKit's event system reads it when it adds them.
TEST(SwitchMotionSource, TheMotionServicesCarryJ181sMotionAndFitnessGate)
{
    const std::string access = SourceText("../../SwitchTouch/SwitchHIDMotionAccess.cpp");
    EXPECT_THAT(access, HasSubstr("#if SWITCH_IPSW_KIND_24B119\nnamespace {"));
    EXPECT_THAT(access, HasSubstr("gIOFirstPublishNotification"));
    const std::string published = Body(access, "bool published(");
    EXPECT_THAT(published, HasSubstr("setProperty(\"TCCService\", \"kTCCServiceMotionSensors\")"));
    EXPECT_THAT(published, HasSubstr("setProperty(\"motionRestrictedService\", true)"));
    const std::string create =
        Body(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "IOService *switch_hid_create(");
    const std::string gated = Between(create, "#if SWITCH_IPSW_KIND_24B119", "#endif");
    for (const char *kind : {"kSwitchHIDAccelerometer", "kSwitchHIDGyro", "kSwitchHIDDeviceMotion"})
        EXPECT_THAT(gated, HasSubstr(kind));
    EXPECT_THAT(gated, Not(HasSubstr("kSwitchHIDOrientation")));
    EXPECT_LT(create.find("switch_hid_motion_restrict(device)"), create.find("device->start(resources)"));
}

// HW Test's sensor.device_motion reads these three sysctls (apps/hwtest/tests/SensorTests.swift, its entitlements'
// sysctl list): registered once the driver has started.
TEST(SwitchMotionSource, DebugTotalsAreTheSysctlsHWTestReads)
{
    const std::string totals = SourceText("../SwitchMotionTotals.cpp");
    for (const char *name :
         {"\"switch_motion_reports\"", "\"switch_motion_fastpath_copies\"", "\"switch_motion_gyro_us\""})
        EXPECT_THAT(totals, HasSubstr(name));
    const std::string start = Body(SourceText("../SwitchMotion.cpp"), "extern \"C\" int switch_motion_start(");
    EXPECT_LT(start.find("__atomic_store_n(&gMotion, candidate"), start.find("switch_motion_totals_register()"));
}

TEST(SwitchMotionSource, SharedWorkLoopAcrossMotionHidDevices)
{
    const std::string start = Body(SourceText("../SwitchMotion.cpp"), "bool SwitchMotion::start(");
    ASSERT_FALSE(start.empty());
    const size_t loopAt = start.find("loop = IOWorkLoop::workLoop();");
    ASSERT_NE(loopAt, std::string::npos);
    EXPECT_EQ(Count(start, "switch_hid_create("), 3u);
    for (const char *call :
         {"switch_hid_create(s == Accel ? kSwitchHIDAccelerometer : kSwitchHIDGyro, loop)",
          "switch_hid_create(kSwitchHIDDeviceMotion, loop)", "switch_hid_create(kSwitchHIDOrientation, loop)"}) {
        const size_t at = start.find(call);
        ASSERT_NE(at, std::string::npos) << call;
        EXPECT_GT(at, loopAt) << call;
    }
}
