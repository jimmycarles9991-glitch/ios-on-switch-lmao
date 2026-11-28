// What the wake sources rely on outside SwitchWakeSources.h, which no host model runs:
//   * the console buttons' HID descriptor (drivers/SwitchTouch/SwitchHIDDevice.cpp, the bytes iPadOS parses) carries
//     Menu and Snapshot as its fourth and fifth bits, kSwitchButtonHome and kSwitchButtonSnapshot in SwitchHID.h, in
//     the one-byte report;
//   * SwitchBluetooth looks at every inbound ACL packet's start (any panel state), drops a connection's buttons at its
//     Disconnection Complete and all of them with the client; SwitchWake no longer pulses Home for the wake event (the
//     controller's own click does it), and SwitchButtons clicks Menu and Snapshot.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

#include "../../SwitchTouch/SwitchHID.h"
#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

static_assert(kSwitchButtonHome == 1u << 3);
static_assert(kSwitchButtonSnapshot == 1u << 4);

TEST(SwitchWakeSource, ButtonsDescriptorHasHome)
{
    const std::string source = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    std::string body = Between(source, "kButtonsDescriptor[] = {", "};");
    body = std::regex_replace(body, std::regex("//[^\\n]*"), "");
    std::vector<unsigned> data;
    const std::regex byte("0x[0-9a-fA-F]{2}");
    for (std::sregex_iterator it(body.begin(), body.end(), byte), end; it != end; ++it)
        data.push_back((unsigned)std::stoul(it->str(), nullptr, 16));
    // HID short items: a prefix (tag | type | size), then size bytes of little-endian data.
    std::vector<unsigned> usages;
    unsigned bits = 0, size = 0, count = 0;
    for (size_t i = 0; i < data.size();) {
        const unsigned prefix = data[i];
        const unsigned n = prefix & 3;
        unsigned value = 0;
        for (unsigned b = 0; b < n && i + 1 + b < data.size(); ++b)
            value |= data[i + 1 + b] << (8 * b);
        const unsigned tag = prefix & 0xfc;
        if (tag == 0xa0)
            usages.clear();  // Collection: the usages before it name the collection, not a field
        else if (tag == 0x08)
            usages.push_back(value);
        else if (tag == 0x74)
            size = value;
        else if (tag == 0x94)
            count = value;
        else if (tag == 0x80)
            bits += size * count;
        i += 1 + n;
    }
    // Power, Volume +, Volume -, Menu (Home), Snapshot
    EXPECT_EQ(usages, (std::vector<unsigned>{0x30, 0xe9, 0xea, 0x40, 0x65}));
    EXPECT_EQ(bits, 8u);  // the report's one byte after its ID (SwitchButtonsReport)
}

TEST(SwitchWakeSource, BluetoothPeeksEveryPacket)
{
    const std::string bt = SourceText("../../SwitchBluetooth/SwitchBluetooth.cpp");
    const std::string acl = Between(bt, "case H4_ACL: {", "case H4_SCO:");
    EXPECT_THAT(acl, HasSubstr("switch_wake_bt_acl(acl, peek - 1);"));
    EXPECT_THAT(acl, Not(HasSubstr("dark")));
    EXPECT_THAT(bt, HasSubstr("switch_wake_bt_gone("));
    EXPECT_EQ(Count(bt, "switch_wake_bt_reset();"), 2u);
    const std::string wake = SourceText("../SwitchWake.cpp");
    EXPECT_THAT(wake, Not(HasSubstr("switch_buttons_home_pulse")));
    EXPECT_THAT(wake, HasSubstr("uint8_t click = step.click;"));
    EXPECT_THAT(wake, HasSubstr("switch_buttons_bt_click(click)"));
    EXPECT_THAT(wake, HasSubstr("switch_bt_reconnect_take("));  // the reconnect after a Bluetooth wake presses Home
    const std::string buttons = SourceText("../../SwitchButtons/SwitchButtons.cpp");
    EXPECT_THAT(buttons, HasSubstr("{SWITCH_BT_CAPTURE, kSwitchButtonSnapshot}"));
    EXPECT_THAT(buttons, Not(HasSubstr("IOSleep")));
    EXPECT_THAT(buttons, HasSubstr("btTimer->wakeAtTime("));
    EXPECT_THAT(wake, HasSubstr("__atomic_load_n(&gButtonsStale, __ATOMIC_RELAXED)"));
    const std::string gpio = SourceText("../../TegraGPIO/TegraGPIO.cpp");
    EXPECT_THAT(gpio, Not(HasSubstr("IOSleep")));
    EXPECT_THAT(gpio, HasSubstr("deliveryTimer->wakeAtTime("));
}
