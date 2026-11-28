// SPDX-License-Identifier: MIT
// device_open.h: a process's Metal device that did not open fails that ask alone, its class named, and
// the next ask opens afresh; the device carries the cause out (TegraGM20BMetal.mm) and a prepared raw device that did not open
// is prepared again.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../device_open.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(GM20BDeviceOpen, TheRawDeviceCarriesTheClassOfWhatItMetToThePluginsLine)
{
    const std::string facade = SourceText("../TegraGM20BMetal.mm");
    EXPECT_THAT(facade, HasSubstr("failure = dev->openFailure();"));
    EXPECT_THAT(Body(facade, "id<MTLDevice> metal_device_open("), HasSubstr("gOpenFailure = failure;"));
    const std::string gpu = SourceText("../gpu.cpp");
    const std::string open = Body(
        gpu, "bool Device::open(const char *packPath, uint64_t stack, std::string &err, Role role, bool asyncPackWindow)");
    EXPECT_LT(open.find("openFailure_ = OpenFailure::Structural;"), open.find("pack_.load("));
    EXPECT_LT(open.find("openFailure_ = OpenFailure::Transient;"), open.find("k_.open(err)"));
    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalPlugin.mm"),
                     "id openOnce(unsigned port, gm20b::OpenFailure &failure, NSString *__strong &why)"),
                HasSubstr("failure = metal_open_failure();"));
}

// A device that did not open fails that ask alone (nil, one error line naming its class); no count, no latch: the next
// ask opens afresh
TEST(GM20BDeviceOpen, EveryAskOfAProcessWithNoDeviceOpensAfresh)
{
    const std::string plugin = SourceText("../TegraGM20BMetalPlugin.mm");
    EXPECT_THAT(plugin, Not(HasSubstr("static bool tried")));
    EXPECT_THAT(plugin, Not(HasSubstr("DeviceOpens")));
    const std::string open = Body(plugin, "id openDevice(unsigned port)");
    EXPECT_THAT(open, HasSubstr("if (device)\n        return device;"));
    EXPECT_THAT(open, HasSubstr("device = openOnce(port, failure, why);"));
    EXPECT_THAT(open, HasSubstr("gm20b::openFailureText(failure), why);"));
    const std::string container = Body(plugin, "bool systemContainerCache(char *out, size_t size)");
    EXPECT_LT(container.find("entitledToSystemContainer(identifier)"), container.find("lookup(identifier, &error)"));
}

TEST(GM20BDeviceOpen, APreparedRawDeviceThatDidNotOpenIsPreparedAgain)
{
    const std::string facade = SourceText("../TegraGM20BMetal.mm");
    const std::string open = Body(facade, "id<MTLDevice> metal_device_open(");
    EXPECT_THAT(open, HasSubstr("gPrepared.started = dev != nullptr;"));
    EXPECT_THAT(open, HasSubstr("gOpenFailure = failure;"));
}

}  // namespace
