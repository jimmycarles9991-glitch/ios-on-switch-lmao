// Guards on SwitchJoyConHID.cpp's game-controller personalities, per kernel: the iOS 17 build (xnu-10063) carries
// AppleGameControllerPersonality's 21H461 Nintendo entries itself (IOHIDEventDummyService on the interface,
// HIDServiceSupport on the device), and the iOS 27 build (xnu-12377) adds none, so 24B119's own AppleGCHIDUserEventDriver
// (score 1000) claims the IOHIDInterface and JoyConHIDServicePlugin finds GameControllerSupport on it. A dummy service
// at score 10000 there would take the interface and leave the Joy-Cons without a GCController.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Configured;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchJoyConPersonalities, Ios17CarriesTheNintendoEntries)
{
    const std::string build = Configured(SourceText("../SwitchJoyConHID.cpp"), "XNU_12377_121_6", false);
    EXPECT_THAT(build, HasSubstr("        addNintendoPersonalities();"));
    EXPECT_THAT(build, HasSubstr("OSString::withCString(\"IOHIDEventDummyService\")"));
    EXPECT_THAT(build, HasSubstr("setProperty(\"HIDServiceSupport\", true);"));
    EXPECT_THAT(build, HasSubstr("        addButtonsPersonality();"));
}

TEST(SwitchJoyConPersonalities, Ios27LeavesTheInterfaceToApplesDriver)
{
    const std::string build = Configured(SourceText("../SwitchJoyConHID.cpp"), "XNU_12377_121_6", true);
    EXPECT_THAT(build, Not(HasSubstr("addNintendoPersonalities")));
    EXPECT_THAT(build, Not(HasSubstr("OSString::withCString(\"IOHIDEventDummyService\")")));
    EXPECT_THAT(build, Not(HasSubstr("IOHIDProviderPropertyMerger\")")));
    EXPECT_THAT(build, Not(HasSubstr("setProperty(\"HIDServiceSupport\"")));
    EXPECT_THAT(build, HasSubstr("        addButtonsPersonality();")) << "Home and Capture stay the console's buttons";
}

// 24B119's JoyConHIDServicePlugin reads VendorID and ProductID from AppleGCHIDUserEventDriver, which copies them from
// the IOHIDInterface; IOHIDDevice::publishProperties fills the interface only from the device's getters.
TEST(SwitchJoyConPersonalities, Ios27PublishesItsIdentityThroughTheGetters)
{
    const char *getters[] = {
        "OSString *newManufacturerString() const override", "OSNumber *newVendorIDNumber() const override",
        "OSNumber *newProductIDNumber() const override", "OSString *newSerialNumberString() const override"};
    const std::string ios27 = Configured(SourceText("../SwitchJoyConHID.cpp"), "XNU_12377_121_6", true);
    const std::string ios17 = Configured(SourceText("../SwitchJoyConHID.cpp"), "XNU_12377_121_6", false);
    for (const char *getter : getters) {
        EXPECT_THAT(ios27, HasSubstr(getter));
        EXPECT_THAT(ios17, Not(HasSubstr(getter))) << "iOS 17's plugin reads the device's own properties";
    }
}

}  // namespace
