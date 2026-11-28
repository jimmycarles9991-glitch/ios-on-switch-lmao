// XusbHost.cpp mailbox handling against xusb_register_model: DISABLE_SS_LFPS_DETECTION disables SS port 0 LFPS
// immediately and defers the 1 ms ACK reply instead of calling IOSleep(1) inside the mailbox IRQ handler.
#include "drivers/SwitchUSB/XusbHost.h"
#include "drivers/SwitchUSB/XusbHostRegisters.h"
#include "drivers/SwitchUSB/XusbLanes.h"
#include "drivers/SwitchUSB/XusbMailbox.h"
#include "drivers/SwitchUSB/XudcHardware.h"
#include "xusb_register_model.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<bool> gLfpsCalls;

}  // namespace

namespace xudc {
bool DmaBuffer::allocate(uint32_t, bool)
{
    return false;
}

void DmaBuffer::release()
{}
}  // namespace xudc

extern "C" {
void tegra_mc_hot_reset(uint32_t, uint32_t, bool)
{}

bool switch_dma_pa_managed(uint64_t)
{
    return false;
}

uint64_t phystokv_range(uint64_t pa, size_t *)
{
    return pa;
}

void switch_usb_ss_lfps(bool enable)
{
    gLfpsCalls.push_back(enable);
}
}  // extern "C"

namespace {

class XusbHostMailboxTest : public ::testing::Test {
protected:
    xhost::Controller host;

    void SetUp() override
    {
        xusbModelReset();
        gLfpsCalls.clear();
        ASSERT_TRUE(host.map());
    }

    static bool hasAnySleep()
    {
        for (const XusbEvent &e : xusbModel().log) {
            if (e.kind == XusbEvent::SleepMs)
                return true;
        }
        return false;
    }

    static void injectFirmwareMessage(uint32_t cmd, uint32_t data)
    {
        auto &r = xusbModel().registers;
        r[xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_OWNER] = 1u;  // MBOX_OWNER_FW
        r[xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_DATA_OUT] = (cmd << 24) | (data & 0xffffffu);
        r[xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_CMD] = xhost::MBOX_DEST_SMI;
    }
};

TEST_F(XusbHostMailboxTest, DisableLfpsPort0DefersReplyWithoutSleeping)
{
    const uint32_t data = 1u << 1;  // SS port 0
    injectFirmwareMessage(xhost::MBOX_DISABLE_SS_LFPS_DETECTION, data);

    uint32_t answer = 0;
    uint32_t answerData = 0;
    xusb_mbox_deferred deferred = {};
    ASSERT_TRUE(host.serviceMailbox(answer, answerData, deferred));

    EXPECT_FALSE(hasAnySleep()) << "serviceMailbox must never IOSleep on the work loop";
    EXPECT_EQ(answer, 0u);
    EXPECT_EQ(gLfpsCalls, (std::vector<bool>{false}));
    EXPECT_TRUE(deferred.pending);
    EXPECT_EQ(deferred.request_cmd, xhost::MBOX_DISABLE_SS_LFPS_DETECTION);
    EXPECT_EQ(deferred.reply_cmd, xhost::MBOX_ACK);
    EXPECT_EQ(deferred.data, data);
    EXPECT_EQ(deferred.delay_ms, XUSB_MBOX_LFPS_SETTLE_MS);
    EXPECT_TRUE(xusbWritesTo(xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_DATA_IN).empty())
        << "reply is deferred until the 1 ms timer fires";

    host.finishMailboxReply(deferred.request_cmd, deferred.reply_cmd, deferred.data);
    EXPECT_FALSE(hasAnySleep());
    const auto replies = xusbWritesTo(xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_DATA_IN);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0], (xhost::MBOX_ACK << 24) | data);
    EXPECT_NE(xusbModel().registers[xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_CMD] &
                  (xhost::MBOX_INT_EN | xhost::MBOX_DEST_FALC),
              0u);
}

TEST_F(XusbHostMailboxTest, EnableLfpsPort0RepliesImmediatelyWithoutDeferring)
{
    const uint32_t data = 1u << 1;
    injectFirmwareMessage(xhost::MBOX_ENABLE_SS_LFPS_DETECTION, data);

    uint32_t answer = 0;
    uint32_t answerData = 0;
    xusb_mbox_deferred deferred = {};
    ASSERT_TRUE(host.serviceMailbox(answer, answerData, deferred));

    EXPECT_FALSE(hasAnySleep());
    EXPECT_FALSE(deferred.pending);
    EXPECT_EQ(gLfpsCalls, (std::vector<bool>{true}));
    const auto replies = xusbWritesTo(xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_DATA_IN);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0], (xhost::MBOX_ACK << 24) | data);
}

TEST_F(XusbHostMailboxTest, DisableLfpsOtherPortRepliesImmediatelyWithoutLfpsWrite)
{
    const uint32_t data = 1u << 2;  // SS port 1: not on this board
    injectFirmwareMessage(xhost::MBOX_DISABLE_SS_LFPS_DETECTION, data);

    uint32_t answer = 0;
    uint32_t answerData = 0;
    xusb_mbox_deferred deferred = {};
    ASSERT_TRUE(host.serviceMailbox(answer, answerData, deferred));

    EXPECT_FALSE(hasAnySleep());
    EXPECT_FALSE(deferred.pending);
    EXPECT_TRUE(gLfpsCalls.empty());
    const auto replies = xusbWritesTo(xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_DATA_IN);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0], (xhost::MBOX_ACK << 24) | data);
}

TEST_F(XusbHostMailboxTest, AckMessageReleasesMailboxAndReturnsAnswer)
{
    injectFirmwareMessage(xhost::MBOX_ACK, 0x1234u);

    uint32_t answer = 0;
    uint32_t answerData = 0;
    xusb_mbox_deferred deferred = {};
    ASSERT_TRUE(host.serviceMailbox(answer, answerData, deferred));

    EXPECT_FALSE(hasAnySleep());
    EXPECT_FALSE(deferred.pending);
    EXPECT_EQ(answer, xhost::MBOX_ACK);
    EXPECT_EQ(answerData, 0x1234u);
    EXPECT_EQ(xusbModel().registers[xhost::kFpciPA + xhost::XUSB_CFG_ARU_MBOX_OWNER], xhost::MBOX_OWNER_NONE);
}

}  // namespace
