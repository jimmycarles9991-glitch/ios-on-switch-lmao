extern "C" {
#include "hw_test_app/cpu/switch_ipi_probe.h"
}

#include <gtest/gtest.h>

#include <mach/mach_time.h>

#include <thread>
#include <vector>

namespace {
uint64_t ticks(uint64_t microseconds)
{
    mach_timebase_info_data_t base = {};
    mach_timebase_info(&base);
    return microseconds * 1000 * base.denom / base.numer;
}
}  // namespace

// Two threads of this Mac waking each other 2000 times: every wake arrives, each one later than its signal.
TEST(IpiProbe, EveryWakeArrives)
{
    constexpr unsigned kRounds = 2000;
    switch_ipi_pingpong *p = switch_ipi_pingpong_create(kRounds);
    ASSERT_NE(p, nullptr);
    std::vector<uint64_t> first(kRounds), second(kRounds);
    unsigned lostFirst = 0;
    unsigned lostSecond = 0;
    std::thread a([&] {
        lostFirst = switch_ipi_pingpong_side(p, true, ticks(20), ticks(1'000'000), first.data());
    });
    std::thread b([&] {
        lostSecond = switch_ipi_pingpong_side(p, false, ticks(20), ticks(1'000'000), second.data());
    });
    a.join();
    b.join();
    switch_ipi_pingpong_destroy(p);
    EXPECT_EQ(lostFirst, 0u);
    EXPECT_EQ(lostSecond, 0u);
    for (unsigned i = 0; i < kRounds; ++i) {
        ASSERT_GT(first[i], 0u) << i;
        ASSERT_GT(second[i], 0u) << i;
    }
}

// A side whose peer never answers stops at its first wait and counts every round left as lost.
TEST(IpiProbe, AWakeThatNeverComesIsLost)
{
    switch_ipi_pingpong *p = switch_ipi_pingpong_create(10);
    ASSERT_NE(p, nullptr);
    std::vector<uint64_t> latency(10);
    EXPECT_EQ(switch_ipi_pingpong_side(p, false, 0, ticks(20'000), latency.data()), 10u);
    switch_ipi_pingpong_destroy(p);
}
