// drivers/SwitchJoyCon/SwitchJoyConUse.h: which rail reports are the user playing, and when the pair goes idle.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "../SwitchJoyConUse.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::Report;
using switch_joycon_fixture::report;

struct Use : ::testing::Test {
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
    EXPECT_EQ(at.front(), 1u);  // 1841 + 157 is still in, 1841 + 314 is out
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
        const unsigned x = (r[6] | (r[7] & 0x0f) << 8) + 260;
        worn.push_back(report(true, x, r[7] >> 4 | r[8] << 4));
    }
    EXPECT_EQ(uses(worn), 0u);
    EXPECT_TRUE(take(report(true, 1841 + 260 + 300, 2088)));
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

TEST(SwitchJoyConUse, IdleAfterGameControllersOwnTimeout)
{
    EXPECT_EQ(SWITCH_JOYCON_USE_IDLE_MS, 900000u);
    EXPECT_FALSE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS - 1));
    EXPECT_TRUE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS));
}

}  // namespace
