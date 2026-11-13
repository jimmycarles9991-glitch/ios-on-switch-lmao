// drivers/SwitchJoyCon/SwitchJoyConStick.h: a Joy-Con's stick calibration and dead zone from its SPI flash.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchJoyConStick.h"
#include "switch_joycon_fixture.h"

namespace {

using switch_joycon_fixture::report;
using switch_joycon_fixture::spiReply;

TEST(SwitchJoyConStick, TheCapturedJoyConReadsAsDeku)
{
    const switch_joycon_stick s = switch_joycon_fixture::capturedStick();
    EXPECT_TRUE(s.left);
    EXPECT_FALSE(s.user);
    EXPECT_TRUE(s.params);
    EXPECT_EQ(s.centre[0], 0x75c);
    EXPECT_EQ(s.centre[1], 0x83b);
    EXPECT_EQ(s.dead_zone, 0xae);
    EXPECT_EQ(switch_joycon_stick_unpack(switch_joycon_capture::kParams, 3), 0xe14u);
}

TEST(SwitchJoyConStick, ReadsTheJoyConsOwnBlocks)
{
    const uint32_t left[] = {0x603d, 0x8010, 0x6086}, right[] = {0x6046, 0x801b, 0x6098};
    const uint8_t sizes[] = {9, 11, 18};
    for (unsigned read = 0; read < SWITCH_JOYCON_STICK_READS; ++read) {
        for (const bool isLeft : {true, false}) {
            switch_joycon_stick s{};
            switch_joycon_stick_defaults(&s, isLeft);
            uint8_t request[5];
            switch_joycon_stick_request(&s, read, request);
            const uint32_t address = isLeft ? left[read] : right[read];
            EXPECT_EQ(request[0] | request[1] << 8 | request[2] << 16 | request[3] << 24, address);
            EXPECT_EQ(request[4], sizes[read]);
        }
    }
}

TEST(SwitchJoyConStick, TheRightStickBlockStartsWithItsCentre)
{
    const uint8_t right[9] = {0x5c, 0xb7, 0x83, 0xec, 0x74, 0x48, 0xeb, 0x44, 0x46};
    switch_joycon_stick s{};
    switch_joycon_stick_defaults(&s, false);
    const std::vector<uint8_t> data = spiReply(false, SWITCH_JOYCON_STICK_FACTORY, right);
    ASSERT_TRUE(switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_FACTORY, data.data(), data.size()));
    EXPECT_EQ(s.centre[0], 0x75c);
    EXPECT_EQ(s.centre[1], 0x83b);
}

TEST(SwitchJoyConStick, UserCalibrationWithItsMagicReplacesTheFactorys)
{
    switch_joycon_stick s = switch_joycon_fixture::capturedStick();
    uint8_t user[11] = {0xb2, 0xa1};
    for (unsigned i = 0; i < 9; ++i)
        user[2 + i] = switch_joycon_capture::kFactory[i];
    user[5] = 0x00;  // centre x 0x700
    user[6] = 0x07;
    const std::vector<uint8_t> data = spiReply(true, SWITCH_JOYCON_STICK_USER, user);
    ASSERT_TRUE(switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_USER, data.data(), data.size()));
    EXPECT_TRUE(s.user);
    EXPECT_EQ(s.centre[0], 0x700);
    EXPECT_EQ(s.centre[1], 0x830);

    user[0] = 0xff;
    switch_joycon_stick other = switch_joycon_fixture::capturedStick();
    const std::vector<uint8_t> unmarked = spiReply(true, SWITCH_JOYCON_STICK_USER, user);
    ASSERT_TRUE(switch_joycon_stick_reply(&other, SWITCH_JOYCON_STICK_USER, unmarked.data(), unmarked.size()));
    EXPECT_FALSE(other.user);
    EXPECT_EQ(other.centre[0], 0x75c);
}

TEST(SwitchJoyConStick, ErasedOrNonsenseBlocksKeepTheDefaults)
{
    switch_joycon_stick s{};
    switch_joycon_stick_defaults(&s, true);
    uint8_t erased[18];
    for (uint8_t &b : erased)
        b = 0xff;
    for (unsigned read = 0; read < SWITCH_JOYCON_STICK_READS; ++read) {
        const std::vector<uint8_t> data = spiReply(true, read, erased);
        ASSERT_TRUE(switch_joycon_stick_reply(&s, read, data.data(), data.size()));
    }
    EXPECT_EQ(s.centre[0], SWITCH_JOYCON_STICK_CENTRE);
    EXPECT_EQ(s.dead_zone, SWITCH_JOYCON_STICK_DEAD_ZONE);
    EXPECT_FALSE(s.params);

    uint8_t zeros[18] = {};
    for (unsigned read : {SWITCH_JOYCON_STICK_FACTORY, SWITCH_JOYCON_STICK_PARAMS}) {
        const std::vector<uint8_t> data = spiReply(true, read, zeros);
        ASSERT_TRUE(switch_joycon_stick_reply(&s, read, data.data(), data.size()));
    }
    EXPECT_EQ(s.centre[0], SWITCH_JOYCON_STICK_CENTRE);
    EXPECT_EQ(s.dead_zone, SWITCH_JOYCON_STICK_DEAD_ZONE);
}

TEST(SwitchJoyConStick, AReplyToAnotherReadIsNotTaken)
{
    switch_joycon_stick s{};
    switch_joycon_stick_defaults(&s, true);
    const std::vector<uint8_t> params = spiReply(true, SWITCH_JOYCON_STICK_PARAMS, switch_joycon_capture::kParams);
    EXPECT_FALSE(switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_FACTORY, params.data(), params.size()));
    EXPECT_FALSE(switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_PARAMS, params.data(), params.size() - 1));
    EXPECT_FALSE(s.params);
}

TEST(SwitchJoyConStick, EachJoyConReadsItsOwnStickOnly)
{
    switch_joycon_stick left{}, right{};
    switch_joycon_stick_defaults(&left, true);
    switch_joycon_stick_defaults(&right, false);
    switch_joycon_fixture::Report r = report(true, 1841, 2088);
    EXPECT_EQ(switch_joycon_stick_axis(&left, r.data(), 0), 1841u);
    EXPECT_EQ(switch_joycon_stick_axis(&left, r.data(), 1), 2088u);
    EXPECT_EQ(switch_joycon_stick_axis(&right, r.data(), 0), 0u);
    r = report(false, 2200, 1900);
    EXPECT_EQ(switch_joycon_stick_axis(&right, r.data(), 0), 2200u);
    EXPECT_EQ(switch_joycon_stick_axis(&right, r.data(), 1), 1900u);
}

TEST(SwitchJoyConStick, TheCapturedRestIsInsideTheDeadZoneAndAPushIsNot)
{
    const switch_joycon_stick s = switch_joycon_fixture::capturedStick();
    for (const auto &sample : switch_joycon_capture::kResting) {
        const switch_joycon_fixture::Report r = report(true, sample.x, sample.y);
        EXPECT_TRUE(switch_joycon_stick_within(&s, r.data(), s.centre, s.dead_zone));
    }
    const switch_joycon_fixture::Report pushed = report(true, 0x75c + 0xaf, 0x83b);
    EXPECT_FALSE(switch_joycon_stick_within(&s, pushed.data(), s.centre, s.dead_zone));
}

}  // namespace
