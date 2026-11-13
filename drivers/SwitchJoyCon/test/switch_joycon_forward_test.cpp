// drivers/SwitchJoyCon/SwitchJoyConForward.h: which rail reports reach the HID device.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchJoyConForward.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::Report;
using switch_joycon_fixture::report;

void sendSubcommand(switch_joycon_forward &f, uint8_t subcommand, uint8_t argument)
{
    uint8_t out[49] = {0x01, 0x00};
    out[10] = subcommand;
    out[11] = argument;
    switch_joycon_forward_output(&f, out, sizeof(out));
}

struct Forward : ::testing::Test {
    switch_joycon_forward f{};
    switch_joycon_stick stick = switch_joycon_fixture::capturedStick();

    void SetUp() override
    {
        switch_joycon_forward_reset(&f);
    }

    bool take(const Report &r)
    {
        return switch_joycon_forward_report(&f, &stick, r.data(), r.size());
    }

    unsigned forwarded(const std::vector<Report> &reports)
    {
        unsigned n = 0;
        for (const Report &r : reports)
            n += take(r) ? 1 : 0;
        return n;
    }
};

TEST_F(Forward, TheCapturedRestingJoyConForwardsOnlyItsFirstReport)
{
    EXPECT_EQ(forwarded(switch_joycon_fixture::resting(328)), 1u);
}

TEST_F(Forward, AnUntouchedQuarterHourOfNoisyDriftingStickForwardsAlmostNothing)
{
    // 15 minutes at 15 ms: twice the captured jitter (3 more counts either way), drifting 60 counts inside the dead
    // zone. A report per quarter dead zone of drift.
    const unsigned n = forwarded(switch_joycon_fixture::resting(60000, 3, 60, -20));
    EXPECT_GE(n, 1u);
    EXPECT_LE(n, 3u);
}

TEST_F(Forward, AtRestAQuarterDeadZoneIsNoiseAndOutOfItASixteenth)
{
    const auto &rest = switch_joycon_capture::kResting[0];
    EXPECT_TRUE(take(report(true, rest.x, rest.y)));
    EXPECT_FALSE(take(report(true, rest.x + 0xae / 4, rest.y)));
    EXPECT_FALSE(take(report(true, rest.x + 25, rest.y + 33)));
    EXPECT_TRUE(take(report(true, rest.x + 0xae / 4 + 1, rest.y)));

    const unsigned out = 0x75c + 0xae + 100;
    EXPECT_TRUE(take(report(true, out, 0x83b)));
    EXPECT_FALSE(take(report(true, out + 0xae / 16, 0x83b)));
    EXPECT_TRUE(take(report(true, out + 0xae / 16 + 1, 0x83b)));
}

TEST_F(Forward, APushForwardsWhileTheStickMovesAndStopsWhenItIsHeld)
{
    EXPECT_TRUE(take(switch_joycon_fixture::resting(1)[0]));
    const std::vector<Report> pushed = switch_joycon_fixture::push(0x75c + 900, 8, 200);
    unsigned moving = 0, held = 0;
    for (size_t i = 0; i < pushed.size(); ++i)
        (i < 8 ? moving : held) += take(pushed[i]) ? 1 : 0;
    EXPECT_EQ(moving, 8u);
    EXPECT_EQ(held, 0u);
}

TEST_F(Forward, ButtonEdgesAlwaysForward)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    Report pressed = report(true, 1841, 2088, 1);
    pressed[5] = 0x08;
    EXPECT_TRUE(take(pressed));
    EXPECT_FALSE(take(pressed));
    EXPECT_TRUE(take(report(true, 1842, 2087, 3)));
    Report home = report(true, 1841, 2088, 4);
    home[4] = 0x10;
    EXPECT_TRUE(take(home));
}

TEST_F(Forward, BatteryChangesForwardButConnectionBitsAndTheTimerDoNot)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    Report connection = report(true, 1841, 2088, 9);
    connection[2] = 0x81;
    EXPECT_FALSE(take(connection));
    Report charging = report(true, 1841, 2088);
    charging[2] = 0x90;
    EXPECT_TRUE(take(charging));
    Report lower = report(true, 1841, 2088);
    lower[2] = 0x60;
    EXPECT_TRUE(take(lower));
}

TEST_F(Forward, MotionWantedForwardsEveryReportUntilItIsTurnedOff)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    EXPECT_FALSE(take(report(true, 1841, 2088, 1)));
    sendSubcommand(f, 0x40, 0x01);
    EXPECT_TRUE(take(report(true, 1841, 2088, 2)));
    EXPECT_TRUE(take(report(true, 1841, 2088, 3)));
    sendSubcommand(f, 0x40, 0x00);
    EXPECT_FALSE(take(report(true, 1841, 2088, 4)));
}

TEST_F(Forward, TheInputReportModeChangesNothing)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    sendSubcommand(f, 0x03, 0x3f);
    EXPECT_FALSE(take(report(true, 1841, 2088, 1)));
    sendSubcommand(f, 0x03, 0x30);
    EXPECT_FALSE(take(report(true, 1841, 2088, 2)));
}

TEST_F(Forward, RepliesAndMcuReportsAlwaysForward)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    Report reply = report(true, 1841, 2088);
    reply[0] = 0x21;
    EXPECT_TRUE(take(reply));
    EXPECT_TRUE(take(reply));
    Report mcu = report(true, 1841, 2088);
    mcu[0] = 0x31;
    EXPECT_TRUE(take(mcu));
}

TEST_F(Forward, ANewDeviceForwardsItsFirstReportAndForgetsMotion)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    sendSubcommand(f, 0x40, 0x01);
    switch_joycon_forward_reset(&f);
    EXPECT_FALSE(f.imu);
    EXPECT_TRUE(take(report(true, 1841, 2088, 1)));
    EXPECT_FALSE(take(report(true, 1841, 2088, 2)));
}

TEST(SwitchJoyConForward, AJoyConRWatchesItsOwnStick)
{
    switch_joycon_stick stick{};
    switch_joycon_stick_defaults(&stick, false);
    switch_joycon_forward f{};
    switch_joycon_forward_reset(&f);
    Report r = switch_joycon_fixture::report(false, 2048, 2048);
    EXPECT_TRUE(switch_joycon_forward_report(&f, &stick, r.data(), r.size()));
    switch_joycon_fixture::putStick(r, true, 3000, 3000);
    EXPECT_FALSE(switch_joycon_forward_report(&f, &stick, r.data(), r.size()));
    switch_joycon_fixture::putStick(r, false, 2600, 2048);
    EXPECT_TRUE(switch_joycon_forward_report(&f, &stick, r.data(), r.size()));
}

}  // namespace
