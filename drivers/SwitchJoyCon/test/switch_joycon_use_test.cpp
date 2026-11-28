// drivers/SwitchJoyCon/SwitchJoyConUse.h: deliberate brush-resistant publish, active use tracking,
// touch preemption (only when idle without IMU/rumble), and 900 s pure-inactivity fallback.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "../SwitchJoyConUse.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::Report;
using switch_joycon_fixture::report;

class Use : public ::testing::Test {
protected:
    switch_joycon_use u{};
    switch_joycon_stick stick = switch_joycon_fixture::capturedStick();

    void SetUp() override
    {
        switch_joycon_use_reset(&u);
    }

    bool take(const Report &r)
    {
        return switch_joycon_use_report(&u, &stick, r.data(), r.size());
    }

    bool wake(const Report &r)
    {
        return switch_joycon_use_observe(&u, &stick, r.data(), r.size(), false);
    }

    unsigned uses(const std::vector<Report> &reports)
    {
        unsigned n = 0;
        for (const Report &r : reports)
            n += take(r) ? 1 : 0;
        return n;
    }
};

TEST_F(Use, TheCapturedRestingJoyConIsNotUse)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(328)), 0u);
}

TEST_F(Use, AQuarterHourOfNoiseAndDriftInsideTheDeadZoneIsNotUse)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(60000, 6, 90, 40)), 0u);
}

TEST_F(Use, APushIsUseFromItsFirstReportOutAndHoldingItIsNot)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(20)), 0u);
    const std::vector<Report> pushed = switch_joycon_fixture::push(0x75c + 900, 6, 300);
    std::vector<unsigned> at;
    for (size_t i = 0; i < pushed.size(); ++i) {
        if (take(pushed[i]))
            at.push_back(static_cast<unsigned>(i));
    }
    ASSERT_FALSE(at.empty());
    EXPECT_EQ(at.front(), 1u);
    EXPECT_LT(at.back(), 6u);
}

TEST_F(Use, BackToRestThenAnotherPushIsUseAgain)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(5)), 0u);
    EXPECT_GE(uses(switch_joycon_fixture::push(0x75c + 900, 6, 10)), 1u);
    EXPECT_EQ(uses(switch_joycon_fixture::resting(10)), 0u);
    EXPECT_GE(uses(switch_joycon_fixture::push(0x75c + 900, 6, 10)), 1u);
}

TEST_F(Use, MovingADeadZoneWhileOutIsUse)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(5)), 0u);
    EXPECT_GE(uses(switch_joycon_fixture::push(0x75c + 600, 3, 2)), 1u);
    EXPECT_FALSE(take(report(true, 0x75c + 600 + 0xae, 2088)));
    EXPECT_TRUE(take(report(true, 0x75c + 600 + 0xae + 20, 2088 + 20)));
}

TEST_F(Use, NoiseAtTheDeadZonesEdgeCountsOnce)
{
    EXPECT_EQ(uses(switch_joycon_fixture::resting(5)), 0u);
    unsigned n = 0;
    for (unsigned i = 0; i < 400; ++i)
        n += take(report(true, 0x75c + 0xae - 3 + (i * 7) % 8, 0x83b)) ? 1 : 0;
    EXPECT_EQ(n, 1u);
}

TEST_F(Use, AStickThatRestsOutOfItsDeadZoneIsNotUseUntilItMoves)
{
    std::vector<Report> worn;
    for (const Report &r : switch_joycon_fixture::resting(2000, 2)) {
        const unsigned x = (static_cast<unsigned>(r[6]) | ((static_cast<unsigned>(r[7]) & 0x0fu) << 8)) + 260u;
        const unsigned y = (static_cast<unsigned>(r[7]) >> 4) | (static_cast<unsigned>(r[8]) << 4);
        worn.push_back(report(true, x, y));
    }
    EXPECT_EQ(uses(worn), 0u);
    EXPECT_TRUE(take(report(true, 1841 + 260 + 300, 2088)));
}

TEST_F(Use, BrushResistantPublishRejectsTransientGlitchesAndAcceptsSustainedInput)
{
    // Arm stick at rest.
    EXPECT_FALSE(wake(report(true, 1841, 2088)));

    // Single-tick accidental button bump does not publish.
    Report btn = report(true, 1841, 2088);
    btn[3] = 0x08;
    EXPECT_FALSE(wake(btn));
    EXPECT_FALSE(wake(report(true, 1841, 2088)));

    // Sustained 2-tick button press publishes.
    EXPECT_FALSE(wake(btn));
    EXPECT_TRUE(wake(btn));

    // Reset and test light stick brush (just past dead_zone, <= 2x dead_zone) for 2 ticks: rejected.
    switch_joycon_use_reset(&u);
    EXPECT_FALSE(wake(report(true, 1841, 2088)));
    const Report brush = report(true, 0x75c + 0xae + 30, 0x83b);
    EXPECT_FALSE(wake(brush));
    EXPECT_FALSE(wake(brush));
    EXPECT_FALSE(wake(report(true, 1841, 2088)));

    // Sustained moderate stick deflection (3 ticks) publishes.
    EXPECT_FALSE(wake(brush));
    EXPECT_FALSE(wake(brush));
    EXPECT_TRUE(wake(brush));

    // Strong stick deflection (> 2x dead_zone): 1 tick rejected, 2 ticks publishes.
    switch_joycon_use_reset(&u);
    EXPECT_FALSE(wake(report(true, 1841, 2088)));
    const Report strong = report(true, 0x75c + 0xae * 2 + 50, 0x83b);
    EXPECT_FALSE(wake(strong));
    EXPECT_FALSE(wake(report(true, 1841, 2088)));
    EXPECT_FALSE(wake(strong));
    EXPECT_TRUE(wake(strong));
}

TEST_F(Use, BrushResistantPublishIgnoresWornStickRestingOutsideDeadZone)
{
    const Report worn = report(true, 0x75c + 260, 0x83b);
    for (unsigned i = 0; i < 100; ++i)
        EXPECT_FALSE(wake(worn));
    // Deliberate push past dead_zone from its worn rest position publishes after 2 ticks.
    const Report pushed = report(true, 0x75c + 260 + 300, 0x83b);
    EXPECT_FALSE(wake(pushed));
    EXPECT_TRUE(wake(pushed));
}

TEST_F(Use, EveryGamepadButtonIsUse)
{
    const struct {
        unsigned byte;
        uint8_t bit;
    } buttons[] = {{3, 0x01}, {3, 0x02}, {3, 0x04}, {3, 0x08}, {3, 0x40}, {3, 0x80}, {4, 0x01}, {4, 0x02},
                   {4, 0x04}, {4, 0x08}, {5, 0x01}, {5, 0x02}, {5, 0x04}, {5, 0x08}, {5, 0x40}, {5, 0x80}};

    for (const auto &button : buttons) {
        Report r = report(true, 1841, 2088);
        r[button.byte] = button.bit;
        EXPECT_TRUE(take(r)) << "byte " << button.byte << " bit " << unsigned{button.bit};
    }
}

TEST_F(Use, HomeCaptureGripAndRailButtonsAreNotUse)
{
    for (const auto &[byte, bit] :
         {std::pair<unsigned, uint8_t>{4, 0x10}, {4, 0x20}, {4, 0x80}, {3, 0x10}, {3, 0x20}, {5, 0x10}, {5, 0x20}}) {
        Report r = report(true, 1841, 2088);
        r[byte] = bit;
        EXPECT_FALSE(take(r)) << "byte " << byte << " bit " << unsigned{bit};
        EXPECT_FALSE(wake(r)) << "byte " << byte << " bit " << unsigned{bit};
    }
}

TEST_F(Use, SubcommandRepliesAndNfcReportsCarryTheSameButtons)
{
    Report reply = report(true, 1841, 2088);
    reply[0] = 0x21;
    reply[3] = 0x08;
    EXPECT_TRUE(take(reply));
    Report nfc = report(true, 1841, 2088);
    nfc[0] = 0x31;
    nfc[5] = 0x02;
    EXPECT_TRUE(take(nfc));
}

TEST_F(Use, SimpleReportsAndShortPacketsAreIgnored)
{
    Report simple = report(true, 1841, 2088);
    simple[0] = 0x3f;
    simple[3] = 0xff;
    EXPECT_FALSE(take(simple));
    Report r = report(true, 1841, 2088);
    r[3] = 0x08;
    EXPECT_FALSE(switch_joycon_use_report(&u, &stick, r.data(), 11));
}

TEST(SwitchJoyConUse, AJoyConRsOtherStickBytesAreNotItsStick)
{
    switch_joycon_stick stick{};
    switch_joycon_stick_defaults(&stick, false);
    switch_joycon_use u{};
    switch_joycon_use_reset(&u);
    Report r = report(false, 2048, 2048);
    EXPECT_FALSE(switch_joycon_use_report(&u, &stick, r.data(), r.size()));
    switch_joycon_fixture::putStick(r, true, 4000, 4000);
    EXPECT_FALSE(switch_joycon_use_report(&u, &stick, r.data(), r.size()));
    switch_joycon_fixture::putStick(r, false, 2048 + 600, 2048);
    EXPECT_TRUE(switch_joycon_use_report(&u, &stick, r.data(), r.size()));
}

TEST(SwitchJoyConUse, TouchPreemptsIdleRailGamepadAfterGraceUnlessImuOrRumbleActive)
{
    const uint64_t used = 10000;
    // No touch yet, or touch before last gamepad use: never preempts.
    EXPECT_FALSE(switch_joycon_use_touch_preempt(used, 0, false, used + 5000));
    EXPECT_FALSE(switch_joycon_use_touch_preempt(used, used, false, used + 5000));
    EXPECT_FALSE(switch_joycon_use_touch_preempt(used, used - 10, false, used + 5000));

    // Touch after last use, before grace window expires: does not preempt yet.
    const uint64_t touched = used + 500;
    EXPECT_FALSE(switch_joycon_use_touch_preempt(used, touched, false, used + SWITCH_JOYCON_USE_TOUCH_GRACE_MS - 1));

    // Grace window elapsed and not busy: preempts cleanly.
    EXPECT_TRUE(switch_joycon_use_touch_preempt(used, touched, false, used + SWITCH_JOYCON_USE_TOUCH_GRACE_MS));

    // Active IMU or rumble holds the controller alive even when touched.
    EXPECT_FALSE(switch_joycon_use_touch_preempt(used, touched, true, used + 60000));
}

TEST(SwitchJoyConUse, IdleAfterGameControllersOwnTimeout)
{
    EXPECT_EQ(SWITCH_JOYCON_USE_IDLE_MS, 900000u);
    EXPECT_FALSE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS - 1));
    EXPECT_TRUE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS));
}

}  // namespace
