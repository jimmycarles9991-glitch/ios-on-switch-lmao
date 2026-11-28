// drivers/SwitchJoyCon/SwitchJoyConCadence.h: 10 ms active / 30 ms quiet cadence, 5000 ms gamepad hold,
// and cumulative resting-anchor wake for slow stick pushes.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchJoyConCadence.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::Report;
using switch_joycon_fixture::report;

struct Rail {
    switch_joycon_cadence c{};
    switch_joycon_stick stick = switch_joycon_fixture::capturedStick();
    bool gamepad = false;
    bool woke = false;

    explicit Rail(bool gamepadActive = false) : gamepad(gamepadActive)
    {
        switch_joycon_cadence_reset(&c, 0);
    }

    bool tick(uint64_t now, const Report &state, bool busy = false)
    {
        if (!switch_joycon_cadence_due(&c, busy, now))
            return false;
        woke = switch_joycon_cadence_observe(&c, &stick, state.data(), state.size(), gamepad, now);
        return true;
    }
};

TEST(SwitchJoyConCadence, AnUntouchedJoyConGoesQuietAfterIdleHoldAndIsAskedEvery30ms)
{
    Rail rail(false);
    const std::vector<Report> rest = switch_joycon_fixture::resting(2000, 3, 40, 0);
    std::vector<uint64_t> sent;
    for (uint64_t now = 10, i = 0; now < 30000; now += 10, ++i) {
        if (rail.tick(now, rest[i % rest.size()]))
            sent.push_back(now);
    }
    EXPECT_TRUE(rail.c.quiet);
    unsigned quiet = 0;
    for (size_t i = 1; i < sent.size(); ++i) {
        if (sent[i] > 2000) {
            EXPECT_EQ(sent[i] - sent[i - 1], 30u) << "at " << sent[i];
            ++quiet;
        }
    }
    EXPECT_GT(quiet, 900u);
    unsigned early = 0;
    for (const uint64_t at : sent)
        early += at < SWITCH_JOYCON_CADENCE_IDLE_HOLD_MS ? 1 : 0;
    EXPECT_EQ(early, SWITCH_JOYCON_CADENCE_IDLE_HOLD_MS / SWITCH_JOYCON_CADENCE_ACTIVE_MS - 1u);
}

TEST(SwitchJoyConCadence, ActiveGamepadHolds10msCadenceFor5000ms)
{
    Rail rail(true);
    const Report rest = report(true, 1841, 2088);
    for (uint64_t now = 10; now < 2000; now += 10)
        (void)rail.tick(now, rest);
    ASSERT_TRUE(rail.c.quiet);

    // One button press wakes the rail and holds 10 ms cadence for a full 5000 ms.
    Report pressed = rest;
    pressed[3] = 0x08;
    ASSERT_TRUE(rail.tick(2010, pressed));
    EXPECT_TRUE(rail.woke);
    EXPECT_FALSE(rail.c.quiet);

    // Release on the next tick extends the 5000 ms hold from 2020 -> 7020.
    ASSERT_TRUE(rail.tick(2020, rest));
    EXPECT_FALSE(rail.c.quiet);

    for (uint64_t now = 2030; now < 7020; now += 10) {
        ASSERT_TRUE(rail.tick(now, rest)) << "at " << now;
        EXPECT_FALSE(rail.c.quiet) << "at " << now;
    }
    // At 7020 (5000 ms after release), quiet resumes (next quiet poll at 7040 = 30 ms after 7010).
    EXPECT_FALSE(rail.tick(7020, rest));
    EXPECT_TRUE(rail.c.quiet);
    EXPECT_TRUE(rail.tick(7040, rest));
}

TEST(SwitchJoyConCadence, SlowStickPushInsideDeadZoneWakesAgainstRestingAnchor)
{
    Rail rail(true);
    const Report rest = report(true, 1841, 2088);
    for (uint64_t now = 10; now < 2000; now += 10)
        (void)rail.tick(now, rest);
    ASSERT_TRUE(rail.c.quiet);

    // Push slowly at 15 counts per 30 ms quiet tick (well below 48/tick) while staying inside the 174-count dead zone.
    bool wokeInsideDeadZone = false;
    uint64_t now = 2010;
    for (unsigned step = 1; step <= 10; ++step, now += 30) {
        const unsigned x = 1841 + step * 15;
        const Report moving = report(true, x, 2088);
        ASSERT_TRUE(rail.tick(now, moving));
        if (rail.woke) {
            EXPECT_TRUE(switch_joycon_stick_within(&rail.stick, moving.data(), rail.stick.centre, rail.stick.dead_zone))
                << "must wake before reaching dead zone edge, step " << step;
            wokeInsideDeadZone = true;
            break;
        }
    }
    EXPECT_TRUE(wokeInsideDeadZone);
    EXPECT_FALSE(rail.c.quiet);
}

TEST(SwitchJoyConCadence, AFirstPressComesBack10msLater)
{
    uint64_t worst = 0;
    for (uint64_t pressAt = 3000; pressAt < 3060; ++pressAt) {
        Rail rail(true);
        uint64_t seen = 0;
        std::vector<uint64_t> after;
        for (uint64_t now = 10; now < 3300; now += 10) {
            Report state = report(true, 1841, 2088);
            if (now >= pressAt)
                state[3] = 0x08;
            if (rail.tick(now, state) && now >= pressAt) {
                if (!seen) {
                    seen = now;
                } else {
                    after.push_back(now);
                }
            }
        }
        ASSERT_NE(seen, 0u);
        worst = seen - pressAt > worst ? seen - pressAt : worst;
        ASSERT_GE(after.size(), 2u);
        EXPECT_EQ(after[0] - seen, uint64_t{SWITCH_JOYCON_CADENCE_ACTIVE_MS});
    }
    EXPECT_LE(worst, uint64_t{SWITCH_JOYCON_CADENCE_QUIET_MS});
}

TEST(SwitchJoyConCadence, QuietClockRearmsAt10msAfterAPress)
{
    constexpr uint64_t kReply = 5;
    for (uint64_t pressAt = 3000; pressAt < 3060; ++pressAt) {
        Rail rail(true);
        std::vector<uint64_t> sent;
        for (uint64_t now = 30; now < 3400;) {
            Report state = report(true, 1841, 2088);
            if (now >= pressAt)
                state[3] = 0x08;
            const bool asked = rail.tick(now, state);
            if (asked && now >= pressAt)
                sent.push_back(now);
            now +=
                asked && rail.woke ? kReply + SWITCH_JOYCON_CADENCE_ACTIVE_MS : switch_joycon_cadence_interval(&rail.c);
        }
        ASSERT_GE(sent.size(), 3u);
        EXPECT_LE(sent[0] - pressAt, uint64_t{SWITCH_JOYCON_CADENCE_QUIET_MS}) << pressAt;
        EXPECT_EQ(sent[1] - sent[0], kReply + SWITCH_JOYCON_CADENCE_ACTIVE_MS) << pressAt;
        EXPECT_EQ(sent[2] - sent[1], uint64_t{SWITCH_JOYCON_CADENCE_ACTIVE_MS}) << pressAt;
    }
}

TEST(SwitchJoyConCadence, HeldButtonsAndADeflectedStickKeepTheActiveCadence)
{
    Rail rail(true);
    Report held = report(true, 1841, 2088);
    held[5] = 0x40;
    for (uint64_t now = 10; now < 8000; now += 10)
        EXPECT_TRUE(rail.tick(now, held));
    Rail deflected(true);
    for (uint64_t now = 10; now < 8000; now += 10)
        EXPECT_TRUE(deflected.tick(now, report(true, 0x75c + 400, 0x83b)));
    Rail grip(false);
    Report gripped = report(true, 1841, 2088);
    gripped[4] = 0x80;
    for (uint64_t now = 10; now < 3000; now += 10)
        (void)grip.tick(now, gripped);
    EXPECT_TRUE(grip.c.quiet);
}

TEST(SwitchJoyConCadence, OutputMotionOrRumbleKeepTheActiveCadence)
{
    Rail rail(false);
    const Report rest = report(true, 1841, 2088);
    for (uint64_t now = 10; now < 3000; now += 10)
        (void)rail.tick(now, rest);
    ASSERT_TRUE(rail.c.quiet);
    EXPECT_TRUE(rail.tick(3000, rest, true));
    EXPECT_TRUE(rail.tick(3010, rest, true));
    EXPECT_FALSE(rail.c.quiet);
    EXPECT_FALSE(rail.tick(3020, rest));
    EXPECT_FALSE(rail.tick(3030, rest));
    EXPECT_TRUE(rail.tick(3040, rest));
}

TEST(SwitchJoyConCadence, AnUntouchedMinuteCosts33HzAndNoReports)
{
    for (const uint64_t clock : {uint64_t{10}, uint64_t{30}}) {
        Rail rail(false);
        switch_joycon_forward forward{};
        switch_joycon_forward_reset(&forward);
        const std::vector<Report> rest = switch_joycon_fixture::resting(5000, 3, 20, 10);
        unsigned requests = 0;
        unsigned forwarded = 0;
        unsigned i = 0;
        for (uint64_t now = 10; now <= 61000; now += clock, ++i) {
            const Report &state = rest[i % rest.size()];
            if (!rail.tick(now, state))
                continue;
            if (now <= 1000)
                continue;
            ++requests;
            forwarded += switch_joycon_forward_report(&forward, &rail.stick, state.data(), state.size()) ? 1 : 0;
        }
        const double perSecond = requests / 60.0;
        EXPECT_NEAR(perSecond, 1000.0 / SWITCH_JOYCON_CADENCE_QUIET_MS, 0.2) << "clock " << clock;
        EXPECT_LE(forwarded, 1u) << "clock " << clock;
    }
}

TEST(SwitchJoyConCadence, RumbleMovesOnlyItsOwnSide)
{
    const uint8_t neutral[10] = {0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    EXPECT_FALSE(switch_joycon_rumble_moving(true, neutral, sizeof(neutral)));
    EXPECT_FALSE(switch_joycon_rumble_moving(false, neutral, sizeof(neutral)));
    uint8_t left[10];
    for (unsigned i = 0; i < 10; ++i)
        left[i] = neutral[i];
    left[5] = 0x60;
    EXPECT_TRUE(switch_joycon_rumble_moving(true, left, sizeof(left)));
    EXPECT_FALSE(switch_joycon_rumble_moving(false, left, sizeof(left)));
    EXPECT_FALSE(switch_joycon_rumble_moving(true, left, 9));
}

}  // namespace
