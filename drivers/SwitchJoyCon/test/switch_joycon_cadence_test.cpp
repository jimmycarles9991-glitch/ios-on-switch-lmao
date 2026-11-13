// drivers/SwitchJoyCon/SwitchJoyConCadence.h: how often the rail asks a running Joy-Con, and what a quiet one costs the
// first press.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchJoyConCadence.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::Report;
using switch_joycon_fixture::report;

// The rail's shared clock: a tick every `clock` ms; a request when the cadence says so, the Joy-Con's state as of the
// request coming back with the reply.
struct Rail {
    switch_joycon_cadence c{};
    switch_joycon_stick stick = switch_joycon_fixture::capturedStick();
    Report last = report(true, 1841, 2088);

    Rail()
    {
        switch_joycon_cadence_reset(&c, 0);
    }

    bool woke = false;  // the last request's reply woke a quiet cadence: the rail's clock comes back in 15 ms

    bool tick(uint64_t now, const Report &state, bool busy = false)
    {
        if (!switch_joycon_cadence_due(&c, busy, now))
            return false;
        woke = switch_joycon_cadence_input(&c, switch_joycon_cadence_engaged(&stick, last.data(), state.data()), now);
        last = state;
        return true;
    }
};

TEST(SwitchJoyConCadence, AnUntouchedJoyConGoesQuietAfterASecondAndIsAskedEvery30ms)
{
    Rail rail;
    const std::vector<Report> rest = switch_joycon_fixture::resting(2000, 3, 40, 0);
    std::vector<uint64_t> sent;
    for (uint64_t now = 15, i = 0; now < 30000; now += 15, ++i) {
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
    for (uint64_t at : sent)
        early += at <= 1000 ? 1 : 0;
    EXPECT_EQ(early, 1000u / 15u);
}

TEST(SwitchJoyConCadence, AFirstPressWaitsAtMost15msLongerThanWhenActive)
{
    // A press at every millisecond of a quiet stretch: the request that sees it comes at most 30 ms later, 15 ms more
    // than the active cadence's 15. Afterwards the Joy-Con is asked every 15 ms again.
    uint64_t worst = 0;
    for (uint64_t pressAt = 3000; pressAt < 3060; ++pressAt) {
        Rail rail;
        uint64_t seen = 0;
        std::vector<uint64_t> after;
        for (uint64_t now = 15; now < 3300; now += 15) {
            Report state = report(true, 1841, 2088);
            if (now >= pressAt)
                state[3] = 0x08;
            if (rail.tick(now, state) && now >= pressAt) {
                if (!seen)
                    seen = now;
                else
                    after.push_back(now);
            }
        }
        ASSERT_NE(seen, 0u);
        worst = seen - pressAt > worst ? seen - pressAt : worst;
        ASSERT_GE(after.size(), 2u);
        EXPECT_EQ(after[0] - seen, 15u);
    }
    EXPECT_LE(worst, uint64_t{SWITCH_JOYCON_CADENCE_QUIET_MS});
    EXPECT_LE(worst - SWITCH_JOYCON_CADENCE_ACTIVE_MS, 15u);
}

TEST(SwitchJoyConCadence, WithBothRailsQuietTheClockComesBack15msAfterAPress)
{
    // The rail's clock at 30 ms (both Joy-Cons quiet), re-armed at 15 ms by the reply that wakes one, as runningReport
    // does; the reply comes back 6 ms after its request (deku: 5.64 ms median).
    constexpr uint64_t kReply = 6;
    for (uint64_t pressAt = 3000; pressAt < 3060; ++pressAt) {
        Rail rail;
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
    Rail rail;
    Report held = report(true, 1841, 2088);
    held[5] = 0x40;
    for (uint64_t now = 15; now < 5000; now += 15)
        EXPECT_TRUE(rail.tick(now, held));
    Rail deflected;
    for (uint64_t now = 15; now < 5000; now += 15)
        EXPECT_TRUE(deflected.tick(now, report(true, 0x75c + 400, 0x83b)));
    Rail grip;
    Report gripped = report(true, 1841, 2088);
    gripped[4] = 0x80;
    for (uint64_t now = 15; now < 5000; now += 15)
        (void)grip.tick(now, gripped);
    EXPECT_TRUE(grip.c.quiet);
}

TEST(SwitchJoyConCadence, OutputMotionOrRumbleKeepTheActiveCadence)
{
    Rail rail;
    const Report rest = report(true, 1841, 2088);
    for (uint64_t now = 15; now < 3000; now += 15)
        (void)rail.tick(now, rest);
    ASSERT_TRUE(rail.c.quiet);
    EXPECT_TRUE(rail.tick(3000, rest, true));
    EXPECT_TRUE(rail.tick(3015, rest, true));
    EXPECT_FALSE(rail.c.quiet);
    EXPECT_FALSE(rail.tick(3030, rest));
    EXPECT_TRUE(rail.tick(3045, rest));
}

TEST(SwitchJoyConCadence, ARailClockAt30msAsksAQuietJoyConEveryTick)
{
    Rail rail;
    const Report rest = report(true, 1841, 2088);
    for (uint64_t now = 15; now < 3000; now += 15)
        (void)rail.tick(now, rest);
    ASSERT_EQ(switch_joycon_cadence_interval(&rail.c), SWITCH_JOYCON_CADENCE_QUIET_MS);
    for (uint64_t now = 3030; now < 4000; now += 30)
        EXPECT_TRUE(rail.tick(now, rest));
    EXPECT_TRUE(rail.tick(4029, rest));  // a clock that fires a millisecond late or early keeps the cadence
    EXPECT_TRUE(rail.tick(4058, rest));
}

// An untouched minute, two Joy-Cons' worth: the rail's clock at 15 ms (one rail active would keep it there) and at the
// 30 ms two quiet rails arm it at, the captured stick with twice its jitter, every reply judged for the HID device.
// Before: a request every 15 ms and every 0x30 report handed on in the plugin's 0x30 mode.
TEST(SwitchJoyConCadence, AnUntouchedMinuteCostsHalfTheRequestsAndNoReports)
{
    for (const uint64_t clock : {uint64_t{15}, uint64_t{30}}) {
        Rail rail;
        switch_joycon_forward forward{};
        switch_joycon_forward_reset(&forward);
        const std::vector<Report> rest = switch_joycon_fixture::resting(5000, 3, 20, 10);
        unsigned requests = 0, forwarded = 0, i = 0;
        for (uint64_t now = 15; now <= 61000; now += clock, ++i) {
            const Report &state = rest[i % rest.size()];
            if (!rail.tick(now, state))
                continue;
            if (now <= 1000)
                continue;  // the first second after the session starts is at 15 ms
            ++requests;
            forwarded += switch_joycon_forward_report(&forward, &rail.stick, state.data(), state.size()) ? 1 : 0;
        }
        const double perSecond = requests / 60.0;
        RecordProperty("clock_" + std::to_string(clock) + "_requests_per_s", std::to_string(perSecond));
        RecordProperty("clock_" + std::to_string(clock) + "_forwarded", std::to_string(forwarded));
        EXPECT_NEAR(perSecond, 1000.0 / SWITCH_JOYCON_CADENCE_QUIET_MS, 0.1) << "clock " << clock;
        EXPECT_LE(forwarded, 1u) << "clock " << clock;  // the first report a new device gets
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
