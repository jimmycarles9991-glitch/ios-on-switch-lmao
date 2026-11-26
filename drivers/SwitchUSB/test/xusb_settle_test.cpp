// XusbSettle.h: a step with no interrupt is looked at twice at most (after the first sleep and at Linux's bound), with
// only sleeps between, and the host's waits use it instead of a loop.
#include "drivers/SwitchUSB/XusbSettle.h"
#include "xusb_register_model.h"

#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;

TEST(XusbSettle, NeverReadyIsLookedAtTwice)
{
    xusbModelReset();
    unsigned looks = 0;
    EXPECT_FALSE(xusb::settle("test", 2, 200, [&] {
        ++looks;
        return false;
    }));
    EXPECT_EQ(looks, 2u);
    EXPECT_EQ(xusbModel().log, (std::vector<XusbEvent>{xusbSleep(2), xusbSleep(198)}));
}

TEST(XusbSettle, ReadyAtTheFirstLookStopsThere)
{
    xusbModelReset();
    unsigned looks = 0;
    EXPECT_TRUE(xusb::settle("test", 1, 250, [&] {
        ++looks;
        return true;
    }));
    EXPECT_EQ(looks, 1u);
    EXPECT_EQ(xusbModel().log, (std::vector<XusbEvent>{xusbSleep(1)}));
}

TEST(XusbSettle, ABoundAtTheFirstLookIsOneLook)
{
    xusbModelReset();
    unsigned looks = 0;
    EXPECT_FALSE(xusb::settle("test", 1, 1, [&] {
        ++looks;
        return false;
    }));
    EXPECT_EQ(looks, 1u);
}

// The falcon's IMEM load, CNR after its start and PORTSC.PP raise no interrupt: each is a settle, none a loop. Sending a
// mailbox command waits on nothing here: SwitchXHCI::sendCommand sleeps on its answer and looks at the owner twice.
TEST(XusbSettle, HostWaitsAreSettlesNotLoops)
{
    const std::string host = SourceText("../XusbHost.cpp");
    const std::string load = Body(host, "bool Controller::loadFirmware(");
    ASSERT_FALSE(load.empty());
    EXPECT_EQ(Count(load, "for ("), 0u);
    EXPECT_EQ(Count(load, "while ("), 0u);
    EXPECT_EQ(Count(load, "xusb::settle("), 2u);
    const std::string send = Body(host, "bool Controller::sendMailbox(");
    ASSERT_FALSE(send.empty());
    EXPECT_EQ(Count(send, "xusb::settle("), 0u);
    EXPECT_EQ(Count(send, "for ("), 0u);
    EXPECT_EQ(Count(send, "while ("), 0u);
    const std::string reset = Body(SourceText("../SwitchXHCIEvents.cpp"), "bool SwitchXHCI::resetSuperSpeed(");
    EXPECT_EQ(Count(reset, "for ("), 0u);
    EXPECT_EQ(Count(reset, "while ("), 0u);
    EXPECT_EQ(Count(reset, "xusb::settle("), 1u);
}
}  // namespace
