// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchDisplayComposition.h"

namespace {
using namespace switch_display;

Composition frame(int first, int second, uint64_t sequence)
{
    Composition value;
    value.window[0].on = first >= 0;
    value.window[0].slot = first;
    value.window[1].on = second >= 0;
    value.window[1].slot = second;
    value.seq = sequence;
    return value;
}

TEST(Composition, OneWindowLatchDoesNotReleaseEitherPreviousAllocation)
{
    Composition storage[5];
    Compositions<> commits;
    commits.values = storage;
    const int old = commits.write(frame(1, 2, 1));
    ASSERT_EQ(commits.observe(0, old), Compositions<>::Latch::Taken);
    const int next = commits.write(frame(3, 4, 2));
    EXPECT_EQ(commits.observe(1u << 3, Compositions<>::kNone), Compositions<>::Latch::Waiting);
    for (int slot = 1; slot <= 4; ++slot)
        EXPECT_TRUE(commits.uses(slot));
    EXPECT_EQ(commits.observe(0, next), Compositions<>::Latch::Taken);
    EXPECT_FALSE(commits.uses(1));
    EXPECT_FALSE(commits.uses(2));
    EXPECT_TRUE(commits.uses(3));
    EXPECT_TRUE(commits.uses(4));
}

TEST(Composition, ReplacedCommitKeepsBothSetsUntilBoundaryIdentityIsKnown)
{
    Composition storage[5];
    Compositions<> commits;
    commits.values = storage;
    const int first = commits.write(frame(1, 2, 1));
    const int second = commits.write(frame(3, 4, 2));
    EXPECT_EQ(commits.write(frame(5, 6, 3)), Compositions<>::kNone);
    EXPECT_EQ(commits.observe(1u << 2, first), Compositions<>::Latch::Replaced);
    EXPECT_EQ(commits.active, first);
    EXPECT_EQ(commits.armed, second);
    ASSERT_NE(commits.write(frame(5, 6, 3)), Compositions<>::kNone);
    for (int slot = 1; slot <= 6; ++slot)
        EXPECT_TRUE(commits.uses(slot));
}

// A staged entry (a producer's completion writes its ACT_REQ) is never taken before request(), even when the scanned
// registers match it; a superseded one still is; withdraw() and settle() leave the latch as the requests made it.
TEST(Composition, AStagedEntryIsTakenOnlyAfterItsRequest)
{
    Composition storage[5];
    Compositions<> commits;
    commits.values = storage;
    const int first = commits.write(frame(1, 2, 1));
    ASSERT_EQ(commits.observe(0, first), Compositions<>::Latch::Taken);
    const int staged = commits.write(frame(3, -1, 2));
    commits.deferRequest();
    EXPECT_FALSE(commits.requested());
    EXPECT_TRUE(commits.busy(staged));
    EXPECT_EQ(commits.observe(0, staged), Compositions<>::Latch::Waiting);
    EXPECT_EQ(commits.active, first);
    commits.withdraw();
    EXPECT_EQ(commits.armed, Compositions<>::kNone);
    EXPECT_EQ(commits.active, first);
    const int out = commits.write(frame(4, -1, 3));
    const int next = commits.write(frame(5, -1, 4));
    commits.deferRequest();
    EXPECT_TRUE(commits.requested());
    EXPECT_EQ(commits.observe(0, out), Compositions<>::Latch::Replaced);
    EXPECT_EQ(commits.active, out);
    EXPECT_FALSE(commits.requested());
    commits.settle();
    EXPECT_EQ(commits.active, out);
    EXPECT_EQ(commits.armed, next);
    commits.request();
    EXPECT_TRUE(commits.requested());
    EXPECT_EQ(commits.observe(0, next), Compositions<>::Latch::Taken);
    EXPECT_EQ(commits.active, next);
    const int last = commits.write(frame(6, -1, 5));
    commits.deferRequest();
    commits.withdraw();
    EXPECT_EQ(commits.armed, Compositions<>::kNone);
    EXPECT_FALSE(commits.busy(last));
}

TEST(Composition, SameSurfaceGeometryChangeGetsAnIndependentCommitIdentity)
{
    Composition storage[5];
    Compositions<> commits;
    commits.values = storage;
    auto original = frame(1, 2, 10);
    const int first = commits.write(original);
    ASSERT_EQ(commits.observe(0, first), Compositions<>::Latch::Taken);
    original.seq = 11;
    original.window[1].image.position = 64;
    const int second = commits.write(original);
    EXPECT_NE(first, second);
    EXPECT_EQ(commits.values[first].window[1].image.position, 0u);
    EXPECT_EQ(commits.values[second].window[1].image.position, 64u);
    EXPECT_EQ(commits.observe(0, first), Compositions<>::Latch::Inconsistent);
    EXPECT_TRUE(commits.uses(1));
    EXPECT_TRUE(commits.uses(2));
}

TEST(Composition, StaticImageNeedsNoNewCommitAndOffRetiresBothWindows)
{
    Composition storage[5];
    Compositions<> commits;
    commits.values = storage;
    const int first = commits.write(frame(1, 2, 1));
    ASSERT_EQ(commits.observe(0, first), Compositions<>::Latch::Taken);
    for (unsigned n = 0; n < 120; ++n)
        EXPECT_EQ(commits.observe(0, first), Compositions<>::Latch::Waiting);
    const int off = commits.write(frame(-1, -1, 2));
    ASSERT_EQ(commits.observe(0, off), Compositions<>::Latch::Taken);
    EXPECT_FALSE(commits.uses(1));
    EXPECT_FALSE(commits.uses(2));
}

TEST(Composition, MatrixChangeUsesIdleWindowWithoutRecoloringVisibleVideo)
{
    auto desired = frame(3, -1, 1);
    desired.window[0].image.yuv = true;
    desired.window[0].image.matrix = native_planes::kBt709;
    WindowState windows[2] = {{true, 1, 0, 0}, {false, -1, -1, -1}};
    const auto result = assign(desired.window, windows);
    EXPECT_EQ(result.window[0], 1u);
    EXPECT_EQ(result.convert, 0u);
}

TEST(Composition, MatchingLiveMatricesAreReusedAndConflictsRequireVicConversion)
{
    auto desired = frame(2, 1, 1);
    for (auto &p : desired.window)
        p.image.yuv = true;
    desired.window[0].image.matrix = native_planes::kBt709;
    desired.window[1].image.matrix = native_planes::kBt601;
    WindowState windows[2] = {{true, 1, 0, 0}, {true, 2, 1, 0}};
    auto result = assign(desired.window, windows);
    EXPECT_EQ(result.window[0], 1u);
    EXPECT_EQ(result.convert, 0u);
    desired.window[1].image.matrix = native_planes::kBt2020;
    result = assign(desired.window, windows);
    EXPECT_EQ(result.convert, 2u);
}

TEST(Composition, SitingTheWindowCannotSampleGoesThroughVicOnAnyWindow)
{
    auto desired = frame(2, -1, 1);
    desired.window[0].image.yuv = true;
    desired.window[0].image.convert = true;
    WindowState windows[2] = {{false, -1, -1, -1}, {false, -1, -1, -1}};
    EXPECT_EQ(assign(desired.window, windows).convert, 1u);
    desired.window[0].image.convert = false;
    EXPECT_EQ(assign(desired.window, windows).convert, 0u);
}

TEST(Composition, MixedTransitionBandwidthCoversBothIndependentOldAndNewWindows)
{
    const Demand ui = {0, 0, 720, 1280, 10};
    const Demand first[2] = {{0, 0, 720, 1280, 100}, {0, 0, 720, 1280, 1}};
    const Demand second[2] = {{0, 0, 720, 1280, 1}, {0, 0, 720, 1280, 100}};
    EXPECT_EQ(transitionPeak(ui, first, 2, second, 2), 210u);
    const Demand initial[] = {ui, first[0], second[0]};
    EXPECT_EQ(peak(initial, 3), 111u);
}

TEST(Composition, DisjointOverlayPeaksDoNotAddAndOverflowRefusesAdmission)
{
    const Demand ui = {0, 0, 720, 1280, 10};
    const Demand first[1] = {{0, 0, 100, 1280, 100}}, second[1] = {{100, 0, 620, 1280, 200}};
    EXPECT_EQ(transitionPeak(ui, first, 1, second, 1), 210u);
    EXPECT_EQ(transitionPeak(ui, nullptr, 1, second, 1), UINT64_MAX);
}
}
