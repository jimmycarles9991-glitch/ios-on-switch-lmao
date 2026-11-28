// drivers/SwitchJoyCon/SwitchJoyConForward.h: lossless active stick forwarding, clean snap-back settle,
// and resting idle stick suppression.
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

class Forward : public ::testing::Test {
protected:
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
    // 15 minutes at 15 ms: twice the captured jitter, drifting 60 counts inside the dead zone.
    const unsigned n = forwarded(switch_joycon_fixture::resting(60000, 3, 60, -20));
    EXPECT_GE(n, 1u);
    EXPECT_LE(n, 8u);
}

TEST_F(Forward, OutsideDeadZoneEverySingleCountStepForwardsLosslessly)
{
    const auto &rest = switch_joycon_capture::kResting[0];
    EXPECT_TRUE(take(report(true, rest.x, rest.y)));
    EXPECT_TRUE(f.settled);

    // Move outside dead zone (centre 0x75c = 1884, dead_zone = 0xae = 174 -> outside at 2160).
    const unsigned out = 0x75c + 0xae + 100;
    EXPECT_TRUE(take(report(true, out, 0x83b)));
    EXPECT_FALSE(f.settled);

    // Fine 1-count and 2-count aiming steps must all forward (no 12-count staircasing).
    for (unsigned step = 1; step <= 20; ++step)
        EXPECT_TRUE(take(report(true, out + step, 0x83b))) << "step " << step;

    // Identical raw stick bytes with only timer advancing are still deduplicated.
    EXPECT_FALSE(take(report(true, out + 20, 0x83b, 1)));
    EXPECT_FALSE(take(report(true, out + 20, 0x83b, 2)));
}

TEST_F(Forward, SnapBackToCenterNeverFreezesNearDeadZoneEdge)
{
    const auto &rest = switch_joycon_capture::kResting[0];
    EXPECT_TRUE(take(report(true, rest.x, rest.y)));

    // Push outside dead zone.
    EXPECT_TRUE(take(report(true, 0x75c + 400, 0x83b)));
    EXPECT_FALSE(f.settled);

    // Release: first in-deadzone sample lands near the edge (radius 170 inside 174),
    // then moves inward in steps smaller than dead_zone/4 (43) all the way to center.
    // Every returning step must forward so the stick never freezes at radius 170.
    EXPECT_TRUE(take(report(true, 0x75c + 170, 0x83b)));
    EXPECT_TRUE(take(report(true, 0x75c + 135, 0x83b)));
    EXPECT_TRUE(take(report(true, 0x75c + 100, 0x83b)));
    EXPECT_TRUE(take(report(true, 0x75c + 60, 0x83b)));
    EXPECT_TRUE(take(report(true, 0x75c + 20, 0x83b)));
    EXPECT_TRUE(take(report(true, 0x75c, 0x83b)));
    EXPECT_FALSE(f.settled);

    // Once still at center for SETTLE_TICKS samples, resting suppression engages cleanly.
    for (unsigned i = 0; i < SWITCH_JOYCON_FORWARD_SETTLE_TICKS; ++i)
        (void)take(report(true, 0x75c, 0x83b, static_cast<uint8_t>(i + 1)));
    EXPECT_TRUE(f.settled);
    EXPECT_EQ(switch_joycon_stick_axis(&stick, f.last, 0), 0x75cu);
    EXPECT_FALSE(take(report(true, 0x75c + 5, 0x83b - 4)));
}

TEST_F(Forward, FineStickMotionForwardsWhileButtonsHeldEvenInsideDeadZone)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    Report held = report(true, 1841, 2088, 1);
    held[5] = 0x08;
    EXPECT_TRUE(take(held));
    EXPECT_FALSE(f.settled);

    Report nudged = held;
    switch_joycon_fixture::putStick(nudged, true, 1843, 2088);
    EXPECT_TRUE(take(nudged));
    EXPECT_FALSE(take(nudged));
}

TEST_F(Forward, RestingThresholdSuppressesSmallJitterAndBreaksOutOnQuarterDeadZone)
{
    const auto &rest = switch_joycon_capture::kResting[0];
    EXPECT_TRUE(take(report(true, rest.x, rest.y)));
    EXPECT_FALSE(take(report(true, rest.x + 0xae / 4, rest.y)));
    EXPECT_FALSE(take(report(true, rest.x + 25, rest.y + 33)));
    EXPECT_TRUE(take(report(true, rest.x + 0xae / 4 + 1, rest.y)));
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

TEST_F(Forward, ReprimePreservesImuAndForwardsLiveStateOnce)
{
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    sendSubcommand(f, 0x40, 0x01);
    switch_joycon_forward_reprime(&f);
    EXPECT_TRUE(f.imu);
    EXPECT_FALSE(f.primed);

    sendSubcommand(f, 0x40, 0x00);
    switch_joycon_forward_reprime(&f);
    EXPECT_TRUE(take(report(true, 1841, 2088)));
    EXPECT_FALSE(take(report(true, 1841, 2088, 1)));
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
