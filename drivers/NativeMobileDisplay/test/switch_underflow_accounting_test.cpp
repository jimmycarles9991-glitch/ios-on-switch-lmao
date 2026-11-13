// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../SwitchUnderflowAccounting.h"

namespace {
using Accounting = switch_display::UnderflowAccounting;
using Edge = Accounting::Edge;

std::vector<std::string> gLines;

void IOLog(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gLines.emplace_back(line);
}

constexpr uint32_t kWinAUnderflowInt = 1u << 8, kWinBUnderflowInt = 1u << 9, kWinCUnderflowInt = 1u << 10;

// The driver's frame-end order: underflowRead, checkRotation and planeFrameEnd (their latches), underflowFrameEnd.
struct Display {
    struct Window {
        bool on = false;
    };

    struct PlaneSet {
        Window window[2];
    };

    struct {
        int active = -1;
    } planeLatch;

    PlaneSet sets[2];
    std::map<uint32_t, uint32_t> dc;
    static constexpr bool stats = false;
    bool rotationLatched = false, scanoutStopped = false, planesAttached = true;
    uint64_t frameEnds = 0;

    const PlaneSet &planeSetOf(int set) const
    {
        return sets[set >= 0 ? 1 : 0];
    }

    void frameEnd(uint32_t a, uint32_t b, uint32_t c, uint32_t latched = 0)
    {
        dc[0xbca] = a;
        dc[0xdca] = b;
        dc[0xfca] = c;
        ++frameEnds;
        underflowRead();
        noteLatchedUnderflow(latched);
        underflowFrameEnd();
    }

#include "../SwitchUnderflowDisplay.h"
};

TEST(UnderflowDisplay, BootChainCountsBeforeTheLatchAreDroppedAndCleared)
{
    gLines.clear();
    Display d;
    d.frameEnd(6, 0x1234, 0x99);
    EXPECT_EQ(d.dc[0xbca], 0u);
    EXPECT_EQ(d.dc[0xdca], 0u);
    EXPECT_EQ(d.dc[0xfca], 0u);
    EXPECT_TRUE(gLines.empty());
    EXPECT_EQ(d.underflows.take(0).frames, 0u);
}

TEST(UnderflowDisplay, WindowACountsOnceLatchedAndLogsOncePerEpisode)
{
    gLines.clear();
    Display d;
    d.rotationLatched = true;
    d.frameEnd(3, 0, 0);
    d.frameEnd(2, 0, 0);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "SwitchDisplay: window A underflowed: UFLOW_STATUS 0x3, WIN_A_UF_INT clear, frame end 1\n");
    for (unsigned i = 0; i < Accounting::kQuietFrames; ++i)
        d.frameEnd(0, 0, 0);
    ASSERT_EQ(gLines.size(), 2u);
    EXPECT_EQ(gLines[1],
              "SwitchDisplay: window A underflow over: 2 frame ends underflowed, 5 counted, then 60 clean\n");
    EXPECT_EQ(d.underflows.take(0).counted, 5u);
}

TEST(UnderflowDisplay, StoppedScanoutCountsNothing)
{
    gLines.clear();
    Display d;
    d.rotationLatched = true;
    d.scanoutStopped = true;
    d.frameEnd(4, 0, 0);
    EXPECT_TRUE(gLines.empty());
    EXPECT_EQ(d.dc[0xbca], 0u);
}

TEST(UnderflowDisplay, OverlayWindowsCountOnlyWhileTheActiveSetShowsThem)
{
    gLines.clear();
    Display d;
    d.frameEnd(0, 5, 5);
    EXPECT_TRUE(gLines.empty());
    d.planeLatch.active = 0;
    d.sets[1].window[0].on = true;
    d.frameEnd(0, 7, 8);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "SwitchDisplay: window B underflowed: UFLOW_STATUS 0x7, WIN_B_UF_INT clear, frame end 2\n");
    EXPECT_EQ(d.underflows.take(1).counted, 7u);
    EXPECT_EQ(d.underflows.take(2).counted, 0u);
    d.planesAttached = false;
    d.frameEnd(0, 9, 0);
    EXPECT_EQ(d.underflows.take(1).counted, 0u);
}

TEST(UnderflowDisplay, LatchAloneOpensAnEpisode)
{
    gLines.clear();
    Display d;
    d.rotationLatched = true;
    d.frameEnd(0, 0, 0, kWinAUnderflowInt | kWinCUnderflowInt);
    ASSERT_EQ(gLines.size(), 2u);
    EXPECT_EQ(gLines[0], "SwitchDisplay: window A underflowed: UFLOW_STATUS 0, WIN_A_UF_INT latched, frame end 1\n");
    EXPECT_EQ(gLines[1], "SwitchDisplay: window C underflowed: UFLOW_STATUS 0, WIN_C_UF_INT latched, frame end 1\n");
    d.frameEnd(0, 0, 0);
    EXPECT_EQ(d.underflows.take(0).frames, 1u);
}

TEST(UnderflowDisplay, ClearZeroesEveryCount)
{
    Display d;
    d.dc[0xbca] = 1;
    d.dc[0xdca] = 2;
    d.dc[0xfca] = 3;
    d.clearUnderflowCounts();
    EXPECT_EQ(d.dc[0xbca] | d.dc[0xdca] | d.dc[0xfca], 0u);
}

TEST(UnderflowAccounting, CountWithTheLatchClearIsAnUnderflow)
{
    Accounting a;
    EXPECT_EQ(a.frameEnd(0, 6, false), Edge::kOpened);
    const Accounting::Totals totals = a.take(0);
    EXPECT_EQ(totals.counted, 6u);
    EXPECT_EQ(totals.frames, 1u);
    EXPECT_FALSE(totals.overflowed);
}

TEST(UnderflowAccounting, LatchWithNoCountIsAnUnderflowFrame)
{
    Accounting a;
    EXPECT_EQ(a.frameEnd(1, 0, true), Edge::kOpened);
    const Accounting::Totals totals = a.take(1);
    EXPECT_EQ(totals.counted, 0u);
    EXPECT_EQ(totals.frames, 1u);
}

TEST(UnderflowAccounting, CleanFramesCountNothing)
{
    Accounting a;
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(a.frameEnd(0, 0, false), Edge::kNone);
    const Accounting::Totals totals = a.take(0);
    EXPECT_EQ(totals.counted, 0u);
    EXPECT_EQ(totals.frames, 0u);
}

TEST(UnderflowAccounting, WindowsAreKeptApart)
{
    Accounting a;
    EXPECT_EQ(a.frameEnd(0, 3, false), Edge::kOpened);
    EXPECT_EQ(a.frameEnd(1, 0, false), Edge::kNone);
    EXPECT_EQ(a.frameEnd(2, 9, false), Edge::kOpened);
    EXPECT_EQ(a.take(0).counted, 3u);
    EXPECT_EQ(a.take(1).frames, 0u);
    EXPECT_EQ(a.take(2).counted, 9u);
}

TEST(UnderflowAccounting, OverflowedCountKeepsItsLowBitsAndSaysSo)
{
    Accounting a;
    a.frameEnd(0, Accounting::kCountOverflowed | 5u, false);
    const Accounting::Totals totals = a.take(0);
    EXPECT_EQ(totals.counted, 5u);
    EXPECT_TRUE(totals.overflowed);
    EXPECT_TRUE(a.episode(0).overflowed);
}

TEST(UnderflowAccounting, TakeStartsTheNextStatsWindowButNotTheEpisode)
{
    Accounting a;
    a.frameEnd(0, 2, false);
    EXPECT_EQ(a.take(0).counted, 2u);
    EXPECT_EQ(a.take(0).counted, 0u);
    EXPECT_EQ(a.frameEnd(0, 4, false), Edge::kNone);
    EXPECT_EQ(a.episode(0).counted, 6u);
    EXPECT_EQ(a.episode(0).frames, 2u);
}

TEST(UnderflowAccounting, EpisodeClosesAfterQuietFramesInARow)
{
    Accounting a;
    EXPECT_EQ(a.frameEnd(0, 1, false), Edge::kOpened);
    for (unsigned i = 1; i < Accounting::kQuietFrames; ++i)
        EXPECT_EQ(a.frameEnd(0, 0, false), Edge::kNone) << i;
    EXPECT_EQ(a.frameEnd(0, 0, false), Edge::kClosed);
    EXPECT_EQ(a.episode(0).frames, 1u);
    EXPECT_EQ(a.episode(0).counted, 1u);
    EXPECT_EQ(a.frameEnd(0, 0, false), Edge::kNone);
}

TEST(UnderflowAccounting, FlappingWindowLogsOneEpisode)
{
    Accounting a;
    unsigned opened = 0, closed = 0;
    for (unsigned i = 0; i < 6000; ++i) {
        const Edge edge = a.frameEnd(0, (i % 2u) == 0u ? 1u : 0u, false);
        opened += edge == Edge::kOpened ? 1u : 0u;
        closed += edge == Edge::kClosed ? 1u : 0u;
    }
    EXPECT_EQ(opened, 1u);
    EXPECT_EQ(closed, 0u);
    EXPECT_EQ(a.episode(0).frames, 3000u);
    EXPECT_EQ(a.episode(0).counted, 3000u);
}

TEST(UnderflowAccounting, UnderflowBeforeTheQuietRunEndsKeepsTheEpisodeOpen)
{
    Accounting a;
    a.frameEnd(2, 7, false);
    for (unsigned i = 1; i < Accounting::kQuietFrames; ++i)
        a.frameEnd(2, 0, false);
    EXPECT_EQ(a.frameEnd(2, 0, true), Edge::kNone);
    for (unsigned i = 1; i < Accounting::kQuietFrames; ++i)
        EXPECT_EQ(a.frameEnd(2, 0, false), Edge::kNone);
    EXPECT_EQ(a.frameEnd(2, 0, false), Edge::kClosed);
    EXPECT_EQ(a.episode(2).frames, 2u);
    EXPECT_EQ(a.episode(2).counted, 7u);
}

TEST(UnderflowAccounting, NextEpisodeStartsFromZero)
{
    Accounting a;
    a.frameEnd(0, 5, false);
    for (unsigned i = 0; i < Accounting::kQuietFrames; ++i)
        a.frameEnd(0, 0, false);
    EXPECT_EQ(a.frameEnd(0, 2, false), Edge::kOpened);
    EXPECT_EQ(a.episode(0).counted, 2u);
    EXPECT_EQ(a.episode(0).frames, 1u);
    EXPECT_FALSE(a.episode(0).overflowed);
}
}  // namespace
