// kernel/switch/diag/stall.h, the decisions of XNU's UI stall reports: which samples of SpringBoard's or backboardd's
// main thread are a stall, that a stall is reported once and its end once, how waits and user addresses are named.
#include <gtest/gtest.h>

#include <cstdint>
#include <regex>
#include <string>
#include <utility>
#include <vector>

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

// block_hint.h's thread_snapshot_wait_flags as (name, value) pairs, in the order the header lists them.
std::vector<std::pair<std::string, unsigned>> BlockHints(const char *tree)
{
    const std::string hints = switch_ios::test::SourceText(std::string("../") + tree + "/osfmk/kern/block_hint.h");
    const std::regex entry("(kThreadWait[A-Za-z]+) += 0x([0-9a-f]+),");
    std::vector<std::pair<std::string, unsigned>> out;
    for (std::sregex_iterator it(hints.begin(), hints.end(), entry), end; it != end; ++it)
        out.emplace_back((*it)[1].str(), (unsigned)std::stoul((*it)[2].str(), nullptr, 16));
    return out;
}

// The names follow xnu-12377's thread_snapshot_wait_flags by value; xnu-10063's are the same values, a prefix of them.
TEST(XnuStall, WaitNamesFollowBlockHint)
{
    const auto newer = BlockHints("xnu-12377.121.6"), older = BlockHints("xnu-10063.141.1");
    for (unsigned i = 0; i < newer.size(); ++i)
        EXPECT_EQ(newer[i].second, i) << newer[i].first << ": the table is indexed by value";
    EXPECT_EQ(newer.size(), xnu_stall_wait_names());
    ASSERT_LE(older.size(), newer.size());
    for (unsigned i = 0; i < older.size(); ++i)
        EXPECT_EQ(older[i], newer[i]);
    EXPECT_STREQ(xnu_stall_wait_name(0x02), "mach_msg receive");
    EXPECT_STREQ(xnu_stall_wait_name(0x03), "mach_msg receive on a port set");
    EXPECT_STREQ(xnu_stall_wait_name(0x04), "mach_msg send");
    EXPECT_STREQ(xnu_stall_wait_name(0x0a), "ulock");
    EXPECT_STREQ(xnu_stall_wait_name(0x10), "workloop sync wait");
    EXPECT_STREQ(xnu_stall_wait_name(0x14), "compressor");
    EXPECT_STREQ(xnu_stall_wait_name(0x16), "page busy");
    EXPECT_STREQ(xnu_stall_wait_name(0x1d), "page-in throttle");
    EXPECT_STREQ(xnu_stall_wait_name(0x22), "VM entry unwire");
    EXPECT_STREQ(xnu_stall_wait_name(0x23), "unknown wait");
}

// Exactly block_hint.h's kernel rwlock waits are read as one.
TEST(XnuStall, KernelRWLockWaitsByBlockHint)
{
    for (const auto &[name, value] : BlockHints("xnu-12377.121.6"))
        EXPECT_EQ(xnu_stall_rwlock_wait(value), name.find("KernelRWLock") != std::string::npos) << name;
}

// Both lck_rw_old_t (all of xnu-10063 and default lck_rw_t such as APFS on xnu-12377) and explicit lck_rw_new_t on
// xnu-12377 must decode to the exact lock address when allow_typed is true.
TEST(XnuStall, KernelRWLockFromItsWaitEvent)
{
    const uint64_t old = 0xfffffff012345674ull;
    // xnu-10063: always untyped.
    EXPECT_EQ(xnu_stall_rwlock_of(old + 2, 0x07, false), old);
    EXPECT_EQ(xnu_stall_rwlock_of(old + 1, 0x08, false), old);
    EXPECT_EQ(xnu_stall_rwlock_of(old + 1, 0x09, false), old);
    // xnu-12377: lck_rw_old_t (APFS/BSD/IOKit) uses untyped events even though allow_typed is true.
    EXPECT_EQ(xnu_stall_rwlock_of(old + 2, 0x07, true), old);
    EXPECT_EQ(xnu_stall_rwlock_of(old + 1, 0x08, true), old);
    EXPECT_EQ(xnu_stall_rwlock_of(old + 1, 0x09, true), old);
    // xnu-12377: explicit lck_rw_new_t uses WAITQ_TYPED_EVENT64(hint, lock) + (hint + 1 - 0x07).
    const uint64_t lock = 0xfffffff012345670ull;
    for (const unsigned hint : {0x07u, 0x08u, 0x09u}) {
        const uint64_t typed = (lock ^ ((uint64_t)hint << 48)) + (hint + 1 - 0x07);
        EXPECT_EQ(xnu_stall_rwlock_of(typed, hint, true), lock) << hint;
    }
}

// A turnstile inheritor belonging to kernel_task (pid 0, tid != 0) is a thread owner and must be kept and followed on
// hops 1-2 rather than falling through to port lookup.
TEST(XnuStall, KernelTaskOwnerHopIsFollowed)
{
    EXPECT_TRUE(xnu_stall_owner_is_thread(0, 0x1c6));
    EXPECT_TRUE(xnu_stall_owner_known(0, 0x1c6));
    EXPECT_TRUE(xnu_stall_hop_present(0, 0x1c6, false));

    EXPECT_TRUE(xnu_stall_owner_is_thread(69, 0x508));
    EXPECT_TRUE(xnu_stall_owner_known(69, 0x508));
    EXPECT_TRUE(xnu_stall_hop_present(69, 0x508, false));

    // A workqueue inheritor has a pid without a tid: known, but not yet resolved to a thread.
    EXPECT_FALSE(xnu_stall_owner_is_thread(69, 0));
    EXPECT_TRUE(xnu_stall_owner_known(69, 0));
    EXPECT_TRUE(xnu_stall_hop_present(69, 0, false));

    // Empty owner stops unless a lock on the way was held.
    EXPECT_FALSE(xnu_stall_owner_is_thread(0, 0));
    EXPECT_FALSE(xnu_stall_owner_known(0, 0));
    EXPECT_FALSE(xnu_stall_hop_present(0, 0, false));
    EXPECT_TRUE(xnu_stall_hop_present(0, 0, true));
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

// The end of a stall names the threads that ran the most since its first line: their gains over the snapshot, most
// first, a thread born since counting whole; a cut-short snapshot counts nothing for a thread it may have missed.
TEST(XnuStall, MostRunSinceTheFirstLine)
{
    const uint64_t tids[] = {0x47c, 0x17cd, 0x25a, 0x17d8, 0x2b3};
    const uint64_t cpus[] = {9000, 100, 50000, 200, 7000};
    const uint64_t nowTids[] = {0x25a, 0x17d8, 0x2b3, 0x47c, 0x17cd, 0x2b2};
    const uint64_t nowCpus[] = {50010, 2400, 9500, 9700, 3100, 1800};
    uint64_t top[4] = {}, gains[4] = {};
    EXPECT_EQ(xnu_stall_most(tids, cpus, 5, true, nowTids, nowCpus, 6, 4, top, gains), 4u);
    EXPECT_EQ(top[0], 0x17cdu);
    EXPECT_EQ(gains[0], 3000u);
    EXPECT_EQ(top[1], 0x2b3u);
    EXPECT_EQ(gains[1], 2500u);
    EXPECT_EQ(top[2], 0x17d8u);
    EXPECT_EQ(gains[2], 2200u);
    EXPECT_EQ(top[3], 0x2b2u);
    EXPECT_EQ(gains[3], 1800u);

    EXPECT_EQ(xnu_stall_most(tids, cpus, 5, false, nowTids, nowCpus, 6, 4, top, gains), 4u);
    EXPECT_EQ(top[3], 0x47cu);
    EXPECT_EQ(gains[3], 700u);

    const uint64_t idle[] = {50000, 200, 7000, 9000, 100};
    const uint64_t idleTids[] = {0x25a, 0x17d8, 0x2b3, 0x47c, 0x17cd};
    EXPECT_EQ(xnu_stall_most(tids, cpus, 5, true, idleTids, idle, 5, 4, top, gains), 0u);
    EXPECT_EQ(top[0], 0u);
}
