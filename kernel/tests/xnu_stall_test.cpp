// kernel/switch/diag/stall.h, the decisions of XNU's UI stall reports: which samples of SpringBoard's or backboardd's
// main thread are a stall, that a stall is reported once and its end once, how waits and user addresses are named.
#include <gtest/gtest.h>

#include <cstdint>
#include <regex>
#include <string>

#include "bazel/testing/source_text.h"
#include "xnu_stall_harness.h"

namespace {
constexpr uint64_t kT = 1000;  // the threshold, in ticks

xnu_stall_sample Waiting(uint64_t lastRun, uint64_t now, bool runLoopIdle = false)
{
    xnu_stall_sample s{};
    s.waiting = true;
    s.run_loop_idle = runLoopIdle;
    s.last_run = lastRun;
    s.made_runnable = xnu_stall_not_runnable();
    s.now = now;
    return s;
}

xnu_stall_sample Runnable(uint64_t lastRun, uint64_t madeRunnable, uint64_t now)
{
    xnu_stall_sample s{};
    s.runnable = true;
    s.last_run = lastRun;
    s.made_runnable = madeRunnable;
    s.now = now;
    return s;
}

xnu_stall_sample Running(uint64_t lastRun, uint64_t now)
{
    xnu_stall_sample s{};
    s.running = true;
    s.last_run = lastRun;
    s.made_runnable = xnu_stall_not_runnable();
    s.now = now;
    return s;
}
}  // namespace

TEST(XnuStall, ThresholdIsTwoSeconds)
{
    EXPECT_EQ(xnu_stall_threshold_ns(), 2000000000ull);
}

TEST(XnuStall, WhatIsAStall)
{
    const xnu_stall_sample brief = Waiting(100, 100 + kT - 1);
    EXPECT_EQ(xnu_stall_kind(&brief, kT), 0);
    const xnu_stall_sample blocked = Waiting(100, 100 + kT);
    EXPECT_EQ(xnu_stall_kind(&blocked, kT), 1);
    // An idle run loop (a port set receive) is the main thread at rest, however long.
    const xnu_stall_sample idle = Waiting(100, 100 + 50 * kT, true);
    EXPECT_EQ(xnu_stall_kind(&idle, kT), 0);
    // Runnable counts from when it was made runnable, not from when it left the core.
    const xnu_stall_sample woken = Runnable(100, 100 + 40 * kT, 100 + 40 * kT + kT - 1);
    EXPECT_EQ(xnu_stall_kind(&woken, kT), 0);
    const xnu_stall_sample starved = Runnable(100, 100 + 40 * kT, 100 + 41 * kT);
    EXPECT_EQ(xnu_stall_kind(&starved, kT), 2);
    const xnu_stall_sample unset = Runnable(100, xnu_stall_not_runnable(), 100 + 41 * kT);
    EXPECT_EQ(xnu_stall_kind(&unset, kT), 0);
    const xnu_stall_sample running = Running(100, 100 + 41 * kT);
    EXPECT_EQ(xnu_stall_kind(&running, kT), 0);
    // A clock behind the sample's times (another CPU's stamp) is no stall.
    const xnu_stall_sample behind = Waiting(5000, 4000);
    EXPECT_EQ(xnu_stall_kind(&behind, kT), 0);
}

TEST(XnuStall, OneBeginAndOneEndPerEpisode)
{
    xnu_stall_reset();
    uint64_t now = 100;
    const xnu_stall_sample early = Waiting(100, now += kT / 2);
    EXPECT_EQ(xnu_stall_step(&early, kT), 0);
    const xnu_stall_sample begun = Waiting(100, now += kT);
    EXPECT_EQ(xnu_stall_step(&begun, kT), 1);
    EXPECT_EQ(xnu_stall_since(), 100u);
    EXPECT_EQ(xnu_stall_episode_kind(), 1);
    for (int i = 0; i < 100; ++i) {
        const xnu_stall_sample still = Waiting(100, now += kT);
        EXPECT_EQ(xnu_stall_step(&still, kT), 0) << "reported once however long it lasts";
    }
    // Woken but not run yet: the same episode.
    const xnu_stall_sample woken = Runnable(100, now, now + 1);
    EXPECT_EQ(xnu_stall_step(&woken, kT), 0);
    const xnu_stall_sample ran = Running(100, now += 2);
    EXPECT_EQ(xnu_stall_step(&ran, kT), 2);
    // Running again, or blocked again for less than the threshold: nothing.
    const xnu_stall_sample again = Running(100, now += 2);
    EXPECT_EQ(xnu_stall_step(&again, kT), 0);
    const uint64_t left = now += 5;
    const xnu_stall_sample brief = Waiting(left, left + kT / 2);
    EXPECT_EQ(xnu_stall_step(&brief, kT), 0);
    // A new stall is a new episode.
    const xnu_stall_sample second = Waiting(left, left + 3 * kT);
    EXPECT_EQ(xnu_stall_step(&second, kT), 1);
    EXPECT_EQ(xnu_stall_since(), left);
    // An end seen through a new last_run_time (it ran between two samples and blocked again).
    const xnu_stall_sample next = Waiting(left + 4 * kT, left + 4 * kT + 1);
    EXPECT_EQ(xnu_stall_step(&next, kT), 2);
}

TEST(XnuStall, StarvationIsReportedFromWhenItWasMadeRunnable)
{
    xnu_stall_reset();
    const xnu_stall_sample idle = Waiting(100, 100 + 30 * kT, true);
    EXPECT_EQ(xnu_stall_step(&idle, kT), 0);
    const xnu_stall_sample starved = Runnable(100, 100 + 30 * kT, 100 + 32 * kT);
    EXPECT_EQ(xnu_stall_step(&starved, kT), 1);
    EXPECT_EQ(xnu_stall_since(), 100 + 30 * kT);
    EXPECT_EQ(xnu_stall_episode_kind(), 2);
}

// The names follow block_hint.h's thread_snapshot_wait_flags by value.
TEST(XnuStall, WaitNamesFollowBlockHint)
{
    const std::string hints = switch_ios::test::SourceText("../xnu-10063.141.1/osfmk/kern/block_hint.h");
    const std::regex entry("(kThreadWait[A-Za-z]+) += 0x([0-9a-f]+),");
    unsigned count = 0;
    for (std::sregex_iterator it(hints.begin(), hints.end(), entry), end; it != end; ++it) {
        const unsigned value = (unsigned)std::stoul((*it)[2].str(), nullptr, 16);
        EXPECT_EQ(value, count) << (*it)[1].str() << ": the table is indexed by value";
        ++count;
    }
    EXPECT_EQ(count, xnu_stall_wait_names());
    EXPECT_STREQ(xnu_stall_wait_name(0x02), "mach_msg receive");
    EXPECT_STREQ(xnu_stall_wait_name(0x03), "mach_msg receive on a port set");
    EXPECT_STREQ(xnu_stall_wait_name(0x04), "mach_msg send");
    EXPECT_STREQ(xnu_stall_wait_name(0x0a), "ulock");
    EXPECT_STREQ(xnu_stall_wait_name(0x10), "workloop sync wait");
    EXPECT_STREQ(xnu_stall_wait_name(0x14), "compressor");
    EXPECT_STREQ(xnu_stall_wait_name(0x15), "unknown wait");
}

TEST(XnuStall, UserAddresses)
{
    uint64_t value = 0;
    // Shared cache: unslid.
    EXPECT_EQ(xnu_stall_user_where(0x1a0001234, 0x180000000, 0x40000000, 0x8000000, 0x100000000, &value), 1);
    EXPECT_EQ(value, 0x198001234u);
    // Main executable: offset from its Mach-O header.
    EXPECT_EQ(xnu_stall_user_where(0x100004321, 0x180000000, 0x40000000, 0x8000000, 0x100000000, &value), 2);
    EXPECT_EQ(value, 0x4321u);
    // Anything else as it is.
    EXPECT_EQ(xnu_stall_user_where(0x2800001000, 0x180000000, 0x40000000, 0x8000000, 0x100000000, &value), 0);
    EXPECT_EQ(value, 0x2800001000u);
    // No shared region.
    EXPECT_EQ(xnu_stall_user_where(0x1a0001234, 0, 0, 0, 0, &value), 0);
}
