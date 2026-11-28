// kits/SwitchTime's unit conversions over a 19.2 MHz model clock.
#include "kits/SwitchTime/SwitchTime.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

uint64_t gSwitchTimeNow;

namespace {
uint64_t refDiv(uint64_t x, uint64_t num, uint64_t den)
{
    return static_cast<uint64_t>((static_cast<__uint128_t>(x) * num) / den);
}

void verifyFrequency(uint64_t freq)
{
    const switch_time_scale scale = switch_time_scale_init(freq);
    const uint64_t samples[] = {
        0,
        1,
        2,
        3,
        7,
        11,
        12,
        13,
        19,
        24,
        25,
        95,
        96,
        97,
        624,
        625,
        626,
        1000,
        19199,
        19200,
        19201,
        24000,
        62500,
        1000000,
        freq - 1,
        freq,
        freq + 1,
        freq * 60,
        freq * 86400,
        freq * 86400 * 365,
        freq * 86400 * 365 * 5,
    };
    for (const uint64_t x : samples) {
        EXPECT_EQ(switch_time_ticks_to_ns(&scale, x), refDiv(x, 1'000'000'000ULL, freq)) << freq << " " << x;
        EXPECT_EQ(switch_time_ticks_to_us(&scale, x), refDiv(x, 1'000'000ULL, freq)) << freq << " " << x;
        EXPECT_EQ(switch_time_ticks_to_ms(&scale, x), refDiv(x, 1'000ULL, freq)) << freq << " " << x;
        EXPECT_EQ(switch_time_ns_to_ticks(&scale, x), refDiv(x, freq, 1'000'000'000ULL)) << freq << " " << x;
        EXPECT_EQ(switch_time_us_to_ticks(&scale, x), refDiv(x, freq, 1'000'000ULL)) << freq << " " << x;
        EXPECT_EQ(switch_time_ms_to_ticks(&scale, x), refDiv(x, freq, 1'000ULL)) << freq << " " << x;
    }
    std::mt19937_64 rng(freq);
    for (int i = 0; i < 2000; ++i) {
        const uint64_t ticks = rng() % (freq * 86400ULL * 365ULL);
        EXPECT_EQ(switch_time_ticks_to_ns(&scale, ticks), refDiv(ticks, 1'000'000'000ULL, freq)) << freq;
        EXPECT_EQ(switch_time_ticks_to_us(&scale, ticks), refDiv(ticks, 1'000'000ULL, freq)) << freq;
        EXPECT_EQ(switch_time_ticks_to_ms(&scale, ticks), refDiv(ticks, 1'000ULL, freq)) << freq;
        const uint64_t ns = rng() % (1'000'000'000ULL * 86400ULL * 365ULL);
        EXPECT_EQ(switch_time_ns_to_ticks(&scale, ns), refDiv(ns, freq, 1'000'000'000ULL)) << freq;
    }
}

TEST(SwitchTime, Conversions)
{
    EXPECT_EQ(switch_abs_from_ms(1), 19200u);
    EXPECT_EQ(switch_abs_from_us(10), 192u);
    EXPECT_EQ(switch_abs_to_us(19200), 1000u);
    EXPECT_EQ(switch_abs_to_ms(19200 * 250), 250u);
}

TEST(SwitchTime, FixedPointReciprocalExactAcrossFrequencies)
{
    verifyFrequency(19'200'000ULL);
    verifyFrequency(24'000'000ULL);
    verifyFrequency(62'500'000ULL);
}

TEST(SwitchTime, ElapsedAndDeadlines)
{
    gSwitchTimeNow = 1'000'000;
    const uint64_t start = gSwitchTimeNow;
    gSwitchTimeNow += 19200 * 3;
    EXPECT_EQ(switch_ms_since(start), 3u);
    EXPECT_EQ(switch_us_since(start), 3000u);
    EXPECT_EQ(switch_deadline_us(100), gSwitchTimeNow + 1920);
    EXPECT_EQ(switch_deadline_ms(2), gSwitchTimeNow + 38400);
    EXPECT_EQ(switch_now_us(), gSwitchTimeNow * 625 / 12 / 1000);
    EXPECT_EQ(switch_now_ns(), gSwitchTimeNow * 625 / 12);
    EXPECT_EQ(switch_now_ms(), gSwitchTimeNow * 625 / 12 / 1000000);
    EXPECT_EQ(switch_ns_since(start), 3000000u);
}
}  // namespace
