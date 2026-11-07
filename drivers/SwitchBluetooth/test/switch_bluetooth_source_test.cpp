// Guards on SwitchBluetooth.cpp for what switch_bluetooth_rx_test and switch_bluetooth_rx_dma_test assume but cannot
// see (their UART models run no driver code): PIO mode's RX trigger ("more than 16") with hardware flow control on, and
// DMA mode programmed as modelled, through SwitchBluetoothRxDma.h's framing and transfer lengths. (The APB DMA word
// transfer itself is checked by //drivers/TegraAPBDMA/test.)
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchBluetoothSource, PioProgramsTheTrigger)
{
    const std::string source = SourceText("../SwitchBluetooth.cpp");
    EXPECT_THAT(source, Not(HasSubstr("FCR_RX_TRIG_8")));
    std::istringstream lines(source);
    unsigned writes = 0;
    for (std::string line; std::getline(lines, line);)
        if (line.find("FCR_RX_TRIG_16 | FCR_TX_TRIG_16") != std::string::npos)
            ++writes;
    EXPECT_EQ(writes, 3u) << "setupUart, resetRxFifoLocked and leaving RX DMA mode";
    EXPECT_THAT(source, HasSubstr("FCR_RX_TRIG_16 = 3u << 6"));
    EXPECT_THAT(source, HasSubstr("MCR_CTS_EN | MCR_RTS_EN | MCR_RTS"));  // flow control holds the chip at a full FIFO
}

TEST(SwitchBluetoothSource, DmaModeUsesTheHeaderAsModelled)
{
    const std::string source = SourceText("../SwitchBluetooth.cpp");
    EXPECT_THAT(source, HasSubstr("#include \"SwitchBluetoothRxDma.h\""));
    EXPECT_THAT(source, HasSubstr("switch_bt_h4_frame_byte(&rxFrame, byte)"));
    EXPECT_THAT(source, HasSubstr("rxDmaLength = switch_bt_rx_dma_length(&rxFrame);"));
    EXPECT_THAT(source, HasSubstr("FCR_ENABLE | FCR_DMA | FCR_RX_TRIG_1 | FCR_TX_TRIG_16"));
    EXPECT_THAT(source, HasSubstr("FCR_RX_TRIG_1 = 0u << 6"));
    EXPECT_THAT(source, HasSubstr("IER_RX_DMA = IER_RLSI | IER_RTOIE | IER_EORD"));
    EXPECT_THAT(source, HasSubstr("tegra_apbdma_start_rx_words(rxDma"));
}

// The rails learn the address bluetoothd reads before launchd starts it, so pairing a seated Joy-Con never replaces a
// HID device gamecontrollerd has already prepared.
TEST(SwitchBluetoothSource, RailsLearnTheAddressBeforeLaunchd)
{
    const std::string source = SourceText("../SwitchBluetooth.cpp");
    const size_t hook = source.find("extern \"C\" void switch_bluetooth_data_mounted(void)");
    ASSERT_NE(hook, std::string::npos);
    const std::string body = source.substr(hook, source.find("\n}\n", hook) - hook);
    const size_t staged = body.find("publishAddressBytes(staged);"), announce = body.find("announceAddressToRails();");
    ASSERT_NE(staged, std::string::npos);
    ASSERT_NE(announce, std::string::npos);
    EXPECT_LT(staged, announce);
    EXPECT_EQ(body.find("return;"), body.rfind("return;")) << "only the switch-no-bt return";
}

}  // namespace
