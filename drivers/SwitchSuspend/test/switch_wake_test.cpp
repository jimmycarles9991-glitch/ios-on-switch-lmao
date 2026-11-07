// Waking the Switch (drivers/SwitchSuspend/SwitchWakeSources.h) on the host, through switch_wake_harness.c.
//
// A Bluetooth Joy-Con's or Pro Controller's Home and Capture in the HID input reports it sends to bluetoothd (reports
// 0x30, 0x21, 0x31 and 0x3F behind the HIDP header on an L2CAP dynamic channel, each at its exact length; anything
// else, another pad's report with the same ID, a continuation, a fragmented frame or a signalling channel is not one),
// their clicks of the console's Home and screenshot key (at release, none for the combine chord or a long hold, Home at
// once with the panel off, per connection and gone with it), the wake guarantee's state machine (armed by a wake source
// with the panel off, disarmed by a display-on request, firing at its deadline), the power journal's ring, and the PMC
// wake events SwitchSuspend arms for SC7 (the same registers as its constants before r15, and the wake reason decoded
// from the status registers). The console buttons' descriptor and the drivers' wiring: switch_wake_source_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "switch_wake_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kHandle = 0x000b;
constexpr uint8_t kHome = 0x10;             // the shared button byte's Home
constexpr uint8_t kCapture = 0x20;          // and Capture
constexpr int kBtHome = 1, kBtCapture = 2;  // SWITCH_BT_HOME, SWITCH_BT_CAPTURE
constexpr uint64_t kMs = 1000000;

void le16(Bytes &out, unsigned value)
{
    out.push_back((uint8_t)value);
    out.push_back((uint8_t)(value >> 8));
}

Bytes operator+(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct Acl {
    uint16_t handle = kHandle;
    uint16_t cid = 0x0041;
    unsigned boundary = 0x2;
    std::optional<unsigned> l2capLength, aclLength;
};

// An inbound HCI ACL packet (from its handle/flags header): L2CAP basic frame on `cid` carrying `payload`.
Bytes acl(const Bytes &payload, const Acl &a = {})
{
    Bytes frame;
    le16(frame, a.l2capLength.value_or((unsigned)payload.size()));
    le16(frame, a.cid);
    frame = frame + payload;
    Bytes out;
    le16(out, a.handle | a.boundary << 12);
    le16(out, a.aclLength.value_or((unsigned)frame.size()));
    return out + frame;
}

// HIDP DATA|INPUT + a standard full report: id, timer, battery/connection, right, shared, left, sticks, ... (49 bytes with
// the ID, as a Joy-Con sends 0x30 and 0x21; 0x31 carries NFC/IR data to 362).
Bytes fullReport(uint8_t report = 0x30, uint8_t shared = 0, uint8_t right = 0, uint8_t left = 0,
                 std::optional<size_t> size = std::nullopt)
{
    const size_t n = size.value_or(report == 0x31 ? 362 : 49);
    Bytes body = {report, 0x5a, 0x8e, right, shared, left};
    body.resize(n, 0);
    return Bytes{0xa1} + body;
}

// HIDP DATA|INPUT + 0x3F (simple HID): id, two button bytes, hat, four 16-bit axes (12 bytes).
Bytes simpleReport(uint8_t buttons1 = 0, uint8_t buttons2 = 0)
{
    Bytes out = {0xa1, 0x3f, buttons1, buttons2, 0x08};
    for (int i = 0; i < 4; ++i)
        out = out + Bytes{0x00, 0x80};
    return out;
}

Bytes prefix(const Bytes &packet, size_t n)
{
    return Bytes(packet.begin(), packet.begin() + (long)std::min(n, packet.size()));
}

class Wake : public testing::Test {
protected:
    void SetUp() override
    {
        bt_buttons_reset();
        wake_guard_reset();
        ring_reset();
    }

    // (result, handle) for the first `length` bytes of the packet (default: SWITCH_WAKE_BT_PEEK, as the driver).
    static std::pair<int, unsigned> buttons(const Bytes &packet, std::optional<size_t> length = std::nullopt)
    {
        uint16_t handle = 0xffff;
        uint8_t report = 0;
        const size_t n = length ? *length : std::min(packet.size(), (size_t)bt_peek());
        const Bytes head = prefix(packet, n);
        return {wake_bt_buttons(head.data(), n, &handle, &report), handle};
    }

    // (click, pressed, fresh) for one report.
    static std::tuple<unsigned, unsigned, unsigned> step(uint16_t handle, unsigned held, uint64_t now,
                                                         bool dark = false)
    {
        const unsigned packed = bt_report(handle, held, now, dark ? 1 : 0);
        return {packed & 0xf, packed >> 4 & 0xf, packed >> 8 & 1};
    }

    static std::pair<std::string, size_t> render()
    {
        std::vector<char> buffer(64 * 1024);
        const size_t length = ring_render(buffer.data(), buffer.size());
        return {std::string(buffer.data()), length};
    }

    static std::vector<uint32_t> registers(uint64_t events)
    {
        std::vector<uint32_t> out(6);
        pmc_registers(events, out.data());
        return out;
    }
};

using Step = std::tuple<unsigned, unsigned, unsigned>;
using Result = std::pair<int, unsigned>;

std::vector<std::string> lines(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}

// --- Bluetooth controllers' Home and Capture -------------------------------------------------------------------------

TEST_F(Wake, FullReportButtons)
{
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome))), Result(kBtHome, kHandle));
    EXPECT_EQ(buttons(acl(fullReport(0x21, kHome | 0x02))), Result(kBtHome, kHandle));  // Plus too
    EXPECT_EQ(buttons(acl(fullReport(0x31, kHome))), Result(kBtHome, kHandle));
    // A Joy-Con (L) reports Capture (0x20) in the shared byte, a Pro Controller both.
    EXPECT_EQ(buttons(acl(fullReport(0x30, kCapture, 0, 0x01))), Result(kBtCapture, kHandle));
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome | kCapture))), Result(kBtHome | kBtCapture, kHandle));
}

TEST_F(Wake, FullReportWithoutSystemButtons)
{
    EXPECT_EQ(buttons(acl(fullReport(0x30, 0x02 | 0x01 | 0x0c, 0xff))), Result(0, kHandle));
}

TEST_F(Wake, SimpleReport)
{
    EXPECT_EQ(buttons(acl(simpleReport(0, kHome))), Result(kBtHome, kHandle));
    EXPECT_EQ(buttons(acl(simpleReport(0, kCapture))), Result(kBtCapture, kHandle));
    EXPECT_EQ(buttons(acl(simpleReport(0xff, 0xcf))), Result(0, kHandle));
    // 0x3F has a fixed size: another length is not this report.
    EXPECT_EQ(buttons(acl(simpleReport(0, kHome) + Bytes{0x00})).first, -1);
}

TEST_F(Wake, OtherPadsWithTheSameIds)
{
    // A DualSense's Bluetooth report 0x31 is 78 bytes, its right stick at offset 4: never Home or Capture.
    EXPECT_EQ(buttons(acl(fullReport(0x31, kHome | kCapture, 0, 0, 78))).first, -1);
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome, 0, 0, 64))).first, -1);
    EXPECT_EQ(buttons(acl(fullReport(0x21, kHome, 0, 0, 48))).first, -1);
}

TEST_F(Wake, HandleWithoutFlags)
{
    const Bytes packet = acl(fullReport(0x30, kHome), {.handle = 0x0abc, .boundary = 0x0});  // first non-flushable
    EXPECT_EQ(buttons(packet), Result(kBtHome, 0x0abc));
}

TEST_F(Wake, NotAControllerReport)
{
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome), {.boundary = 0x1})).first, -1);  // continuation
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome), {.cid = 0x0001})).first, -1);    // signalling channel
    Bytes output = fullReport(0x30, kHome);
    output[0] = 0xa2;  // HIDP DATA|OUTPUT
    EXPECT_EQ(buttons(acl(output)).first, -1);
    const Bytes keyboard = {0xa1, 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0};  // a keyboard's boot report
    EXPECT_EQ(buttons(acl(keyboard)).first, -1);
    // The first of several ACL packets of one L2CAP frame (its length says more is coming).
    const Bytes fragmented = acl(fullReport(0x30, kHome), {.l2capLength = 100});
    EXPECT_EQ(buttons(fragmented).first, -1);
    EXPECT_EQ(buttons(prefix(acl(fullReport(0x30, kHome)), 13), 13).first, -1);  // not in yet
    EXPECT_EQ(buttons(Bytes{}, 0).first, -1);
}

TEST_F(Wake, PeekIsEnough)
{
    // The driver passes only SWITCH_WAKE_BT_PEEK bytes; every report kind is decided within them.
    EXPECT_LE(4u + 4 + 1 + 1 + 4 + 1, bt_peek());
    EXPECT_EQ(buttons(acl(fullReport(0x30, kHome)), 14), Result(kBtHome, kHandle));
    EXPECT_EQ(buttons(acl(simpleReport(0, kHome)), 12), Result(kBtHome, kHandle));
}

TEST_F(Wake, ClickAtRelease)
{
    EXPECT_EQ(step(kHandle, kBtHome, 0), Step(0, kBtHome, 1));    // down: nothing yet, a fresh connection
    EXPECT_EQ(step(kHandle, kBtHome, 15 * kMs), Step(0, 0, 0));   // held: 0x30 repeats every 15 ms
    EXPECT_EQ(step(kHandle, 0, 120 * kMs), Step(kBtHome, 0, 0));  // released: one Home click
    EXPECT_EQ(step(kHandle, 0, 135 * kMs), Step(0, 0, 0));
    EXPECT_EQ(step(kHandle, kBtCapture, 200 * kMs), Step(0, kBtCapture, 0));
    EXPECT_EQ(step(kHandle, 0, 300 * kMs), Step(kBtCapture, 0, 0));  // a screenshot
}

TEST_F(Wake, LongHoldClicksNothing)
{
    const uint64_t limit = bt_click_limit();
    EXPECT_EQ(limit, 1000 * kMs);
    step(kHandle, kBtHome, 0);
    EXPECT_EQ(step(kHandle, 0, limit), Step(0, 0, 0));
    step(kHandle, kBtHome, limit + kMs);
    EXPECT_EQ(step(kHandle, 0, 2 * limit), Step(kBtHome, 0, 0));  // just under the limit
}

TEST_F(Wake, CombineChordClicksNothing)
{
    // Capture on Joy-Con (L), handle 0x0c, and Home on Joy-Con (R): GameController's gesture that pairs them.
    step(0x0c, kBtCapture, 0);
    EXPECT_EQ(step(kHandle, kBtHome, 50 * kMs), Step(0, kBtHome, 1));
    EXPECT_EQ(step(0x0c, 0, 300 * kMs), Step(0, 0, 0));
    EXPECT_EQ(step(kHandle, 0, 320 * kMs), Step(0, 0, 0));
    // The next press is a press again.
    step(kHandle, kBtHome, 400 * kMs);
    EXPECT_EQ(step(kHandle, 0, 450 * kMs), Step(kBtHome, 0, 0));
}

TEST_F(Wake, TwoControllersOneButton)
{
    // Two Joy-Con (R)s: the system button is down while either holds it, one click when the last lets go.
    step(kHandle, kBtHome, 0);
    EXPECT_EQ(step(0x0c, kBtHome, 10 * kMs), Step(0, 0, 1));
    EXPECT_EQ(step(kHandle, 0, 50 * kMs), Step(0, 0, 0));
    EXPECT_EQ(step(0x0c, 0, 80 * kMs), Step(kBtHome, 0, 0));
}

TEST_F(Wake, DarkHomeClicksAtOnce)
{
    EXPECT_EQ(step(kHandle, kBtHome, 0, true), Step(kBtHome, kBtHome, 1));  // wakes the console
    EXPECT_EQ(step(kHandle, 0, 100 * kMs, false), Step(0, 0, 0));           // not again at the release
    // Capture with the panel off still clicks at its release (SpringBoard takes no screenshot while dark).
    step(kHandle, kBtCapture, 200 * kMs, true);
    EXPECT_EQ(step(kHandle, 0, 300 * kMs, true), Step(kBtCapture, 0, 0));
}

TEST_F(Wake, DisconnectDropsWithoutAClick)
{
    step(kHandle, kBtHome, 0);
    bt_gone(kHandle);
    EXPECT_EQ(bt_down(), 0u);
    EXPECT_EQ(step(kHandle, 0, 50 * kMs), Step(0, 0, 1));  // a new connection on the handle: no click
    step(0x0c, kBtHome, 60 * kMs);
    bt_gone(kHandle);  // another controller's disconnect leaves this one's press
    EXPECT_EQ(step(0x0c, 0, 100 * kMs), Step(kBtHome, 0, 0));
}

TEST_F(Wake, TableFull)
{
    for (uint16_t handle = 1; handle < 9; ++handle)
        EXPECT_EQ(std::get<2>(step(handle, handle == 1 ? kBtHome : 0, handle * kMs)), 1u) << handle;
    EXPECT_EQ(bt_down(), (unsigned)kBtHome);
    EXPECT_EQ(step(9, 0, 20 * kMs), Step(0, 0, 1));  // takes the oldest slot (handle 1): no click
    EXPECT_EQ(bt_down(), 0u);
    EXPECT_EQ(step(1, 0, 30 * kMs), Step(0, 0, 1));  // handle 1 was forgotten
}

// --- The wake guarantee ----------------------------------------------------------------------------------------------

TEST_F(Wake, GuardFiresWhenNothingAnswers)
{
    const uint64_t timeout = 2500;
    EXPECT_EQ(wake_guard_arm(0, 1000, timeout, 0), 1);
    EXPECT_EQ(wake_guard_arm(2, 2000, timeout, 0), 0);  // keeps the first deadline and source
    EXPECT_EQ(wake_guard_deadline(), 3500u);
    EXPECT_EQ(wake_guard_source(), 0u);
    EXPECT_EQ(wake_guard_check(3499, 0, 0), -1);
    EXPECT_EQ(wake_guard_check(3500, 0, 0), 1);
    EXPECT_EQ(wake_guard_armed(), 0);
    EXPECT_EQ(wake_guard_check(9000, 0, 0), 0);
}

TEST_F(Wake, GuardDisarmedByDisplayOn)
{
    EXPECT_EQ(wake_guard_arm(1, 0, 2500, 0), 1);
    wake_guard_display_on();
    EXPECT_EQ(wake_guard_check(5000, 0, 0), 0);
    // The on request still being played (panel not lit yet) or the panel already lit: nothing to do either.
    EXPECT_EQ(wake_guard_arm(1, 0, 2500, 0), 1);
    EXPECT_EQ(wake_guard_check(5000, 0, 1), 0);
    EXPECT_EQ(wake_guard_arm(1, 0, 2500, 0), 1);
    EXPECT_EQ(wake_guard_check(5000, 1, 0), 0);
}

TEST_F(Wake, GuardNotArmedWithPanelOn)
{
    EXPECT_EQ(wake_guard_arm(0, 0, 2500, 1), 0);
    EXPECT_EQ(wake_guard_check(5000, 0, 0), 0);
}

TEST_F(Wake, GuardTimeoutAndNames)
{
    EXPECT_EQ(guard_ms(), 2500u);
    std::vector<std::string> names;
    for (unsigned i = 0; i < 5; ++i)
        names.emplace_back(source_name(i));
    EXPECT_EQ(names,
              (std::vector<std::string>{"power key", "rail Joy-Con Home", "Bluetooth Home", "power in", "unknown"}));
}

// --- The journal's ring ----------------------------------------------------------------------------------------------

TEST_F(Wake, RingKeepsTheLastLinesOldestFirst)
{
    const unsigned count = ring_lines();
    for (unsigned i = 0; i < count + 6; ++i)
        ring_append(("line " + std::to_string(i)).c_str());
    const auto [text, length] = render();
    std::vector<std::string> want;
    for (unsigned i = 6; i < count + 6; ++i)
        want.push_back("line " + std::to_string(i));
    EXPECT_EQ(lines(text), want);
    EXPECT_EQ(length, text.size());
}

TEST_F(Wake, RingCutsLongLinesAndNewlines)
{
    ring_append(std::string(500, 'x').c_str());
    ring_append("first\nsecond");
    const auto [text, length] = render();
    EXPECT_EQ(lines(text), (std::vector<std::string>{std::string(ring_line_bytes() - 1, 'x'), "first"}));
}

TEST_F(Wake, RingRenderCapacity)
{
    ring_append("abcdef");
    char small[4] = {};
    EXPECT_EQ(ring_render(small, 4), 3u);
    EXPECT_STREQ(small, "abc");
    ring_reset();
    EXPECT_EQ(render(), (std::pair<std::string, size_t>("", 0)));
}

// --- PMC wake events -------------------------------------------------------------------------------------------------

TEST_F(Wake, PmcArmedEvents)
{
    EXPECT_EQ(pmc_armed(1, 0), (1ull << 51) | (1ull << 14));
    EXPECT_EQ(pmc_armed(0, 0), 1ull << 51);  // a USB-PD ALERT stuck low is not armed
    // The gauge's ALRT (r15-charging: charge-zone edges only during sleep), armed only while it reads high.
    EXPECT_EQ(pmc_armed(1, 1), (1ull << 51) | (1ull << 14) | (1ull << 27));
    EXPECT_EQ(pmc_armed(0, 1), (1ull << 51) | (1ull << 27));
}

TEST_F(Wake, PmcRegistersMatchTheOldConstants)
{
    // SwitchSuspend before r15: WAKE_MASK = 1 << 14 (when the ALERT was high), WAKE2_MASK = 1 << (51 - 32), both active
    // low (LVL bits cleared), nothing active high. Order: mask0, mask1, high0, high1, low0, low1.
    EXPECT_EQ(registers(pmc_armed(1, 0)), (std::vector<uint32_t>{1u << 14, 1u << 19, 0, 0, 1u << 14, 1u << 19}));
    // soc-rtc: active high
    EXPECT_EQ(registers(1ull << 16), (std::vector<uint32_t>{1u << 16, 0, 1u << 16, 0, 0, 0}));
}

TEST_F(Wake, PmcReason)
{
    const uint64_t armed = pmc_armed(1, 1);
    EXPECT_STREQ(pmc_reason(0, 1u << 19, armed), "pwrbtn");
    EXPECT_STREQ(pmc_reason(1u << 27, 0, armed), "battery");
    const std::vector<uint32_t> gauge = registers(1ull << 27);  // gauge ALRT: active low
    EXPECT_EQ(gauge[0], 1u << 27);                              // mask0
    EXPECT_EQ(gauge[4], 1u << 27);                              // low0
    EXPECT_EQ(gauge[2], 0u);                                    // high0
    EXPECT_STREQ(pmc_reason(1u << 14, 0, armed), "usb-c");
    EXPECT_STREQ(pmc_reason(1u << 14, 1u << 19, armed), "pwrbtn");      // table order
    EXPECT_STREQ(pmc_reason(1u << 14, 0, pmc_armed(0, 0)), "unknown");  // not armed
    EXPECT_STREQ(pmc_reason(1u << 8, 1u << 20, armed), "unknown");      // Wi-Fi/BT wake: not armed
}

TEST_F(Wake, PmcRadioWakeEvents)
{
    // Bluetooth host wake (52, PH5) active low, Wi-Fi host wake (8, PH2) active high; each only when its driver set the
    // chip up for this sleep.
    EXPECT_EQ(pmc_armed_radios(1, 0, 0, 0), pmc_armed(1, 0));
    EXPECT_EQ(pmc_armed_radios(1, 0, 1, 0), pmc_armed(1, 0) | (1ull << 52));
    EXPECT_EQ(pmc_armed_radios(0, 0, 1, 1), (1ull << 51) | (1ull << 52) | (1ull << 8));
    const std::vector<uint32_t> bt = registers(1ull << 52);
    EXPECT_EQ(bt, (std::vector<uint32_t>{0, 1u << 20, 0, 0, 0, 1u << 20}));
    const std::vector<uint32_t> wifi = registers(1ull << 8);
    EXPECT_EQ(wifi, (std::vector<uint32_t>{1u << 8, 0, 1u << 8, 0, 0, 0}));
    const uint64_t armed = pmc_armed_radios(1, 1, 1, 1);
    EXPECT_STREQ(pmc_reason(0, 1u << 20, armed), "bt");
    EXPECT_STREQ(pmc_reason(1u << 8, 0, armed), "wifi");
    EXPECT_STREQ(pmc_reason(1u << 8, 1u << 19, armed), "pwrbtn");  // the power key first
}

TEST_F(Wake, PmcUsbDeviceWakeEvent)
{
    // UTMIP port 0's sleepwalk (39, active high), only with a USB device on the host port for this sleep.
    EXPECT_EQ(pmc_armed_usb_device(1, 0), pmc_armed(1, 0));
    EXPECT_EQ(pmc_armed_usb_device(1, 1), pmc_armed(1, 0) | (1ull << 39));
    EXPECT_EQ(registers(1ull << 39), (std::vector<uint32_t>{0, 1u << 7, 0, 1u << 7, 0, 0}));
    EXPECT_STREQ(pmc_reason(0, 1u << 7, pmc_armed_usb_device(1, 1)), "usb");
    EXPECT_STREQ(pmc_reason(0, 1u << 7, pmc_armed_usb_device(1, 0)), "unknown");  // not armed
}

TEST_F(Wake, BluetoothSleepModeParameters)
{
    uint8_t on[12], off[12];
    bt_sleep_params(on, 1);
    bt_sleep_params(off, 0);
    // UART sleep, 600 ms idle thresholds, BT_WAKE and HOST_WAKE active low, host sleep allowed in SCO, sleep combined
    // with LPM, no tristate, no USB, no break, level (not pulsed) host wake.
    EXPECT_EQ(std::vector<uint8_t>(on, on + 12), (std::vector<uint8_t>{1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0, 0}));
    EXPECT_EQ(std::vector<uint8_t>(off, off + 12), std::vector<uint8_t>(12, 0));
}

TEST_F(Wake, BluetoothReconnectAfterSleep)
{
    const uint64_t s = 1000000000ull;
    EXPECT_FALSE(bt_reconnect_take(0, 1, 1));  // closed: nothing
    bt_reconnect_open(10 * s);
    EXPECT_FALSE(bt_reconnect_take(11 * s, 0, 1));  // a report of a controller that stayed connected
    EXPECT_FALSE(bt_reconnect_take(11 * s, 1, 0));  // the panel already lit
    EXPECT_TRUE(bt_reconnect_open_now());
    EXPECT_TRUE(bt_reconnect_take(12 * s, 1, 1));  // the reconnect: Home, once
    EXPECT_FALSE(bt_reconnect_open_now());
    EXPECT_FALSE(bt_reconnect_take(12 * s, 1, 1));
    bt_reconnect_open(20 * s);
    EXPECT_FALSE(bt_reconnect_take(36 * s, 1, 1));  // after 15 s it closed unused
    EXPECT_FALSE(bt_reconnect_open_now());
}

TEST_F(Wake, SleepTimeFromTheTegraRtc)
{
    EXPECT_EQ(sleep_rtc_ms(100, 250), 100250u);
    EXPECT_EQ(sleep_rtc_ms(0, 1999), 999u);  // MILLI_SECONDS counts 0-999
    EXPECT_EQ(sleep_elapsed_ms(sleep_rtc_ms(100, 900), sleep_rtc_ms(161, 100)), 60200u);
    EXPECT_EQ(sleep_elapsed_ms(sleep_rtc_ms(200, 0), sleep_rtc_ms(100, 0)), 0u);  // set backwards in between
}

TEST_F(Wake, SleepAverageCurrentFromTheCoulombCounter)
{
    // 0.5 mAh counts (10 mOhm effective sense): 2 counts down in an hour is -1 mA; 120 down in 6 h is -10 mA.
    EXPECT_EQ(sleep_average_ma(1000, 998, 500, 3600000), -1);
    EXPECT_EQ(sleep_average_ma(5000, 4880, 500, 6 * 3600000ull), -10);
    EXPECT_EQ(sleep_average_ma(2, 0xfffe, 500, 3600000), -2);  // the 16-bit counter wraps
    EXPECT_EQ(sleep_average_ma(100, 160, 500, 3600000), 30);   // charging while asleep
    EXPECT_EQ(sleep_average_ma(100, 90, 500, 0), 0);
}

}  // namespace
