// SPDX-License-Identifier: MIT
// TegraGM20BSchedPolicy.h (what a compositor kick preempts, the interleaved runlist, the deadline's holds) and
// TegraGM20BRecover.h's
// tgm_stuck (when a context-switch timeout tick or a late wait may kill a channel) and tgm_hang_blame (which channel a
// late wait kills), over the registers and channels of deploy-2045's hang: GR stuck switching from backboardd's channel 3
// to Maps' 26, PBDMA0 on 26; and tgm_fault_va (the VA a killed channel's GetStatus reports) over nvkm's MMU fault lines.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint32_t kHangEng0 = 0x801ae003;  // busy, ch 3 -> ch 26, ctx_status 7 (switch)
constexpr uint32_t kCtxValid = 1u << 13;

uint32_t resident(uint32_t chid)
{
	return PFIFO_ENG_BUSY | kCtxValid | chid;
}

TEST(KickVictim, GrMidSwitchIsLeftToTheHost)
{
	EXPECT_EQ(tgm_kick_victim(kHangEng0, 0, 1, 26, 0), TGM_VICTIM_SWITCHING);
}

// deploy-2045's switch from backboardd's channel 3 to Maps' 26: the host picked an app while no high channel had work
TEST(KickLoading, TheAppGrIsLoadingIsPreempted)
{
	EXPECT_EQ(tgm_kick_loading(kHangEng0, 0), 26);
	EXPECT_EQ(tgm_kick_loading(PFIFO_ENG_BUSY | 5u << 13 | 26u << 16, 0), 26);
}

TEST(KickLoading, ASaveAloneASwitchToAHighChannelOrATsgIsLeftToTheHost)
{
	EXPECT_EQ(tgm_kick_loading(kHangEng0, 1), TGM_VICTIM_SWITCHING);
	EXPECT_EQ(tgm_kick_loading(PFIFO_ENG_BUSY | 6u << 13 | 26u << 16 | 3u, 0),
		  TGM_VICTIM_SWITCHING);
	EXPECT_EQ(tgm_kick_loading(kHangEng0 | PFIFO_ENG_NEXT_ID_TSG, 0), TGM_VICTIM_SWITCHING);
	EXPECT_EQ(tgm_kick_loading(kCtxValid | 26, 0), TGM_VICTIM_SWITCHING);
}

// The learned preemption latencies of F-v1's widget visit (deadline lines, "latency by channel"): the compositor's kick
// alone serves a channel within a millisecond; the poster, widget and icon renderers are held; a channel at the 100 ms
// cap is not.
TEST(HoldWorth, OnlyChannelsBetweenTheSlackAndAFrameAreHeld)
{
	EXPECT_FALSE(tgm_hold_worth(TGM_PREEMPT_MIN_NS));
	EXPECT_FALSE(tgm_hold_worth(TGM_HOLD_SLACK_NS));
	EXPECT_TRUE(tgm_hold_worth(TGM_HOLD_SLACK_NS + 1));
	for (const uint64_t us : {7836u, 7035u, 4480u})
		EXPECT_TRUE(tgm_hold_worth(us * 1000)) << us;
	EXPECT_TRUE(tgm_hold_worth(TGM_HOLD_CEILING_NS));
	EXPECT_FALSE(tgm_hold_worth(TGM_HOLD_CEILING_NS + 1));
	EXPECT_FALSE(tgm_hold_worth(TGM_INTERVAL_MAX_NS));
}

TEST(HoldWorth, AChannelDrawingInBandsIsLeftToTheCompositorsKick)
{
	EXPECT_FALSE(tgm_hold_worth(TGM_APP_BAND_NS));
	EXPECT_LE(TGM_APP_BAND_NS, 2000ull * 1000);
}

TEST(KickVictim, AppOnGrIsPreempted)
{
	EXPECT_EQ(tgm_kick_victim(resident(26), 0, 1, 26, 0), 26);
}

TEST(KickVictim, AppHoldingThePbdmaWhileGrRunsTheCompositor)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 26, 0), 26);
}

TEST(KickVictim, NothingWhenOnlyHighChannelsRun)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 5, 1), TGM_VICTIM_NONE);
	EXPECT_EQ(tgm_kick_victim(0, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, IdleGrWithAContextLoadedIsNoVictim)
{
	EXPECT_EQ(tgm_kick_victim(kCtxValid | 26, 0, 0, 26, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, ATsgIdNamesNoChannel)
{
	EXPECT_EQ(tgm_kick_victim(resident(7) | PFIFO_ENG_ID_TSG, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

std::vector<uint16_t> plan(const std::vector<uint8_t> &level, const std::vector<uint16_t> &entries,
			   unsigned max)
{
	std::vector<uint16_t> order(4096);
	order.resize(tgm_interleave(level.data(), entries.data(), unsigned(level.size()), max,
				    order.data(), unsigned(order.size())));
	return order;
}

unsigned entriesOf(const std::vector<uint16_t> &order, const std::vector<uint16_t> &entries)
{
	unsigned n = 0;
	for (uint16_t u : order)
		n += entries[u];
	return n;
}

TEST(Interleave, EveryHighUnitFollowsEachLowOne)
{
	const std::vector<uint8_t> high = {0, 1, 0, 0, 1};
	const std::vector<uint16_t> entries(5, 1);
	EXPECT_EQ(plan(high, entries, 64), (std::vector<uint16_t>{0, 1, 4, 2, 1, 4, 3, 1, 4}));
}

TEST(Interleave, OnlyHighOrOnlyLowListsEachOnce)
{
	EXPECT_EQ(plan({1, 1}, {1, 1}, 64), (std::vector<uint16_t>{0, 1}));
	EXPECT_EQ(plan({0, 0, 0}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2}));
	EXPECT_TRUE(plan({}, {}, 64).empty());
}

TEST(Interleave, AFullRunlistSpacesTheHighUnitsOutAndKeepsEveryUnit)
{
	std::vector<uint8_t> high(100, 0);
	std::vector<uint16_t> entries(100, 1);
	high[0] = high[50] = 1;
	entries[50] = 3;  // a group: header and two channels
	const unsigned max = 140;
	const auto order = plan(high, entries, max);
	EXPECT_LE(entriesOf(order, entries), max);
	for (uint16_t u = 0; u < 100; u++)
		EXPECT_NE(std::find(order.begin(), order.end(), u), order.end()) << "unit " << u;
	EXPECT_GT(std::count(order.begin(), order.end(), 0), 1);
}

TEST(Interleave, RoomForOneRepeatListsTheHighOnesOnceAfterTheLowOnes)
{
	const std::vector<uint8_t> high = {0, 1, 0};
	const std::vector<uint16_t> entries = {1, 1, 1};
	EXPECT_EQ(plan(high, entries, 3), (std::vector<uint16_t>{0, 2, 1}));
}

TEST(Interleave, NoRoomForEvenThatListsEachOnce)
{
	const std::vector<uint8_t> high = {0, 1, 0};
	const std::vector<uint16_t> entries = {1, 1, 1};
	EXPECT_EQ(plan(high, entries, 2), (std::vector<uint16_t>{0, 1, 2}));
}

constexpr uint8_t kBg = TGM_LEVEL_BACKGROUND, kFg = TGM_LEVEL_FOREGROUND, kHigh = TGM_LEVEL_HIGH;

// nvgpu's three levels: the compositor after every other unit, every foreground app between two background ones
TEST(Interleave, ThreeLevelsTheNvgpuWay)
{
	const std::vector<uint8_t> level = {kBg, kFg, kHigh, kFg, kBg};
	const std::vector<uint16_t> entries(5, 1);
	EXPECT_EQ(plan(level, entries, 64),
		  (std::vector<uint16_t>{0, 2, 1, 2, 3, 2, 4, 2, 1, 2, 3, 2}));
}

TEST(Interleave, WhicheverUnitTheHostLeavesAHigherLevelIsNext)
{
	const std::vector<uint8_t> level = {kFg, kBg, kHigh, kBg, kFg, kHigh, kBg};
	const std::vector<uint16_t> entries(level.size(), 1);
	const auto order = plan(level, entries, 256);
	for (size_t i = 0; i < order.size(); i++) {
		const uint8_t here = level[order[i]], next = level[order[(i + 1) % order.size()]];
		if (here != kHigh)
			EXPECT_EQ(next, kHigh) << "after position " << i;
	}
	for (uint16_t u = 0; u < level.size(); u++)
		EXPECT_NE(std::find(order.begin(), order.end(), u), order.end()) << "unit " << u;
}

TEST(Interleave, TwoLevelsOfAnyKindInterleaveAsHighAndLow)
{
	EXPECT_EQ(plan({kBg, kHigh, kBg}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2, 1}));
	EXPECT_EQ(plan({kBg, kFg, kBg}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2, 1}));
}

TEST(Interleave, ThreeLevelsTooBigForTheRunlistKeepTheCompositorAfterEveryApp)
{
	// 3 background, 3 foreground, 1 high: the full interleave needs 3 + 3 x 7 = 24 entries, more than 13
	const std::vector<uint8_t> level = {kBg, kFg, kBg, kFg, kBg, kFg, kHigh};
	const std::vector<uint16_t> entries(level.size(), 1);
	const auto order = plan(level, entries, 13);
	EXPECT_LE(order.size(), 13u);
	EXPECT_EQ(order, (std::vector<uint16_t>{0, 6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6}));
}

constexpr uint64_t kInterval = 16666667;  // 60 Hz

// Metal's GPU priorities as Core Animation sets them (CA::OGL::new_metal_context): its window server renderer 0, the
// asynchronous dispatcher 1, other contexts 2 and 4, background contexts' background priority 2
TEST(Priority, OnlyTheTopMetalPriorityIsTheCompositors)
{
	EXPECT_EQ(tgm_priority_level(0, 4, 0), TGM_LEVEL_HIGH);
	EXPECT_EQ(tgm_priority_level(1, 4, 0), TGM_LEVEL_FOREGROUND);
	EXPECT_EQ(tgm_priority_level(2, 4, 0), TGM_LEVEL_FOREGROUND);
	EXPECT_EQ(tgm_priority_level(4, 4, 0), TGM_LEVEL_BACKGROUND);
}

TEST(Priority, ABackgroundRoleRunsAtTheBackgroundPriority)
{
	EXPECT_EQ(tgm_priority_level(2, 4, 1), TGM_LEVEL_BACKGROUND);
	EXPECT_EQ(tgm_priority_level(2, 2, 1), TGM_LEVEL_FOREGROUND);
	// the compositor's level is never a background process's
	EXPECT_EQ(tgm_priority_level(0, 0, 1), TGM_LEVEL_FOREGROUND);
}

TEST(Deadline, AChannelIsHeldItsPreemptionLatencyBeforeTheCompositorsLatestStart)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 20 * kMs, expected, latency, 1),
		  vblank - 8 * kMs);
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 8 * kMs, expected, latency, 1),
		  vblank - 8 * kMs);
	// a channel with a longer latency is held earlier for the same frame
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 30 * kMs, expected, 18 * kMs, 1),
		  vblank - 24 * kMs);
}

TEST(Deadline, AHopelessHoldWaitsForTheNextVblankOnlyWithSlackToIt)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	// 3 ms before the vblank: cannot make it; 19.7 ms to the next, more than the 8 needed: held 8 ms before that one
	uint64_t now = vblank - 3 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
	// the 18 ms hog: 18 + 6 = 24 ms needed, 19.7 to the next vblank: no slack, held now (never slower for the compositor)
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, 18 * kMs, 1), now);
	// past the vblank already
	now = vblank + 2 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
}

TEST(Deadline, AChannelNotOnTheGpuOnlyNeedsDisablingWhenItsHoldIsLate)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	// 7 ms before the vblank, past its hold time (8 ms before): off the GPU it is disabled now and the frame makes it
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 7 * kMs, expected, latency, 0),
		  vblank - 7 * kMs);
	// on the GPU it needs its 2 ms to leave: hopeless for this vblank, held 8 ms before the next
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 7 * kMs, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
}

TEST(Deadline, TheCompositorsFrameIsItsGpuTimeWithoutOverlap)
{
	// two submissions queued back to back: the second's start stamp is before the first's end, so it counts from there
	const uint64_t first = tgm_gpu_span(1008 * kMs, 1000 * kMs, 0);
	const uint64_t second = tgm_gpu_span(1012 * kMs, 1003 * kMs, 1008 * kMs);
	const uint64_t present = tgm_gpu_span(1013 * kMs, 1009 * kMs, 1012 * kMs);
	EXPECT_EQ(first + second + present, 13 * kMs);
	// an app ran between them: the gap is not the compositor's
	EXPECT_EQ(tgm_gpu_span(1030 * kMs, 1025 * kMs, 1012 * kMs), 5 * kMs);
	EXPECT_EQ(tgm_gpu_span(1000 * kMs, 1005 * kMs, 0), 0u);
}

// deploy-next3's stats line: PTIMER x1.627 (ns per unit)
TEST(Deadline, PtimerSpansInNanoseconds)
{
	const uint64_t rate = tgm_pt_rate(1627 * kMs, 1000 * kMs);
	EXPECT_NEAR(double(tgm_pt_ns(10 * kMs, rate)), 16270 * 1000.0,
		    100.0);                              // 16.16: within 0.001%
	EXPECT_EQ(tgm_pt_rate(50 * kMs, 30 * kMs), 0u);  // too short a span to tell
	EXPECT_EQ(tgm_pt_ns(10 * kMs, 0), 0u);
}

TEST(Deadline, TheHogGetsATurnEveryOtherWindow)
{
	const uint64_t latency = 18 * kMs;
	// held in the last window, which closed 3 ms ago: its turn
	EXPECT_TRUE(tgm_owed_turn(1, 3 * kMs, latency));
	// the turn taken, it is held again in the window after
	EXPECT_FALSE(tgm_owed_turn(0, 3 * kMs, latency));
	// an app that leaves the GPU in 300 us is held every window
	EXPECT_FALSE(tgm_owed_turn(1, 3 * kMs, 300 * 1000));
}

TEST(Deadline, ErroredIsolatedOrRecoveringChannelsAreNeverHeldOrReleased)
{
	EXPECT_TRUE(tgm_may_release(0, 0, 0));
	EXPECT_FALSE(tgm_may_release(1, 0, 0));
	EXPECT_FALSE(tgm_may_release(0, 1, 0));
	EXPECT_FALSE(tgm_may_release(0, 0, 1));
	EXPECT_FALSE(tgm_may_release(0, 0, 2));
	EXPECT_TRUE(tgm_may_hold(kFg, 1, 0, 0, 0, 0, 0));
	EXPECT_TRUE(tgm_may_hold(kBg, 1, 0, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kHigh, 1, 0, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 0, 0, 0, 0, 0, 0));  // not registered by gpu_chan.c
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 1, 0, 0, 0, 0));  // the present in flight waits for it
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 1, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 1, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 0, 1, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 0, 0,
				  1));  // nvkm blocks it itself (a teardown, a context bind)
}

// r30-fences' implicit fences: a compositor submission that acquires on an app's pending surface write needs that app
// on the GPU (tgm_hold_need), as the present does for the channels it waits for
TEST(Deadline, AnAppTheCompositorWaitsOnIsNeverHeld)
{
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 1, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kBg, 1, 1, 0, 0, 0, 0));
}

TEST(Deadline, AnAppTheCompositorWaitsOnIsNeverPreemptedByItsKick)
{
	EXPECT_TRUE(tgm_kick_may_preempt(26, 0));
	EXPECT_FALSE(tgm_kick_may_preempt(26, 1));
	EXPECT_FALSE(tgm_kick_may_preempt(TGM_VICTIM_NONE, 0));
	EXPECT_FALSE(tgm_kick_may_preempt(TGM_VICTIM_SWITCHING, 0));
}

TEST(Deadline, CompositorTimeRisesAtOnceAndFallsSlowly)
{
	uint64_t e = tgm_expect(0, 16 * kMs);
	EXPECT_EQ(e, 8 * kMs);
	e = tgm_expect(e, 16 * kMs);
	EXPECT_EQ(e, 12 * kMs);
	for (int i = 0; i < 30; i++)
		e = tgm_expect(e, 16 * kMs);
	EXPECT_EQ(e, 16 * kMs);
	e = tgm_expect(e, 8 * kMs);
	EXPECT_EQ(e, 15 * kMs);
	for (int i = 0; i < 300; i++)
		e = tgm_expect(e, 8 * kMs);
	EXPECT_LE(e, 8 * kMs + 8);
}

TEST(Deadline, PreemptionLatencyTakesALongerSampleAtOnce)
{
	const uint64_t max = 100 * kMs;
	uint64_t p = tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 18 * kMs, max);
	EXPECT_EQ(p, 18 * kMs);
	p = tgm_preempt_sampled(p, 2 * kMs, max);
	EXPECT_EQ(p, 16 * kMs);
	EXPECT_EQ(tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 0, max), TGM_PREEMPT_MIN_NS);
	EXPECT_EQ(tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 200 * kMs, max), max);
	EXPECT_EQ(tgm_preempt_left_early(16 * kMs), 14 * kMs);
	EXPECT_EQ(tgm_preempt_left_early(TGM_PREEMPT_MIN_NS), TGM_PREEMPT_MIN_NS);
}

TEST(Deadline, WindowsOnlyWhileTheCompositorDraws)
{
	const uint64_t now = 1000 * kMs;
	// CA turned vsync on just now: the first frame after an idle is held for
	EXPECT_TRUE(tgm_compositor_drawing(now, kInterval, now, now, 0));
	// vsync on for a while, the compositor kicked last frame
	EXPECT_TRUE(tgm_compositor_drawing(now, kInterval, now, now - 500 * kMs, now - 10 * kMs));
	// its idle countdown: vsync still on, nothing kicked for three frames
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, now, now - 500 * kMs, now - 50 * kMs));
	// vsync off (a detached app, the display asleep, static UI)
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, now - 30 * kMs, now - 500 * kMs,
					    now - 30 * kMs));
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, 0, 0, now));
}

TEST(Deadline, HeldTimeNeverCountsTowardAStall)
{
	// 2.5 s without progress, 1 s of it held by the windows: 1.5 s stalled
	EXPECT_EQ(tgm_unheld_ns(3500 * kMs, 1000 * kMs, 4000 * kMs, 3000 * kMs), 1500 * kMs);
	EXPECT_EQ(tgm_unheld_ns(1000 * kMs, 900 * kMs, 500 * kMs, 300 * kMs), 0u);
	EXPECT_EQ(tgm_unheld_ns(900 * kMs, 1000 * kMs, 0, 0), 0u);  // progress after now was read
}

// nvkm_chan_error's preempt runs in the interrupt pass with interrupts off: with another preempt in flight it never waits
// (up to the FIFO's 10 s channel timeout with interrupts masked), the channel's group goes to nvkm's recovery instead.
TEST(Preempt, NothingInFlightGoesOutAtOnce)
{
	EXPECT_EQ(tgm_preempt_how(0, 0), TGM_PREEMPT_NOW);
	EXPECT_EQ(tgm_preempt_how(1, 0), TGM_PREEMPT_NOW);
}

TEST(Preempt, OneInFlightIsWaitedForOnlyByAThread)
{
	EXPECT_EQ(tgm_preempt_how(0, 1), TGM_PREEMPT_AFTER_WAIT);
	EXPECT_EQ(tgm_preempt_how(1, 1), TGM_PREEMPT_RECOVER);
}

constexpr uint64_t kTick = 100 * kMs;  // recover.c's CTXSW_TIMEOUT_US

TEST(Stuck, ALongDrainOfTheOutgoingChannelIsWaitedOut)
{
	// backboardd's m2v pass of 144 ms, two a frame, while an app waits: its last completion 20 ms before the switch
	// request, a tick every 100 ms after it, the passes done at 144 and 288 ms
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 90, 92, request + kTick, lastDone));
	EXPECT_FALSE(tgm_stuck(0, 91, 92, request + 2 * kTick, request + 144 * kMs));
	EXPECT_FALSE(tgm_stuck(0, 92, 93, request + 3 * kTick, request + 288 * kMs));
	EXPECT_LT(2 * 144 * kMs, TGM_STUCK_NS);
}

TEST(Stuck, Deploy2045sSwitchThatNeverEndsIsKilled)
{
	// ENG0 801ae003 at every tick; backboardd's fence 90/93, nothing completed since 20 ms before the request
	EXPECT_EQ(tgm_kick_victim(kHangEng0, 1, 1, 26, 0), TGM_VICTIM_SWITCHING);
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 90, 93, request + 4 * kTick, lastDone));
	EXPECT_TRUE(tgm_stuck(0, 90, 93, request + 5 * kTick, lastDone));
}

TEST(Stuck, IdleOrDeadChannelsNeverAre)
{
	EXPECT_FALSE(tgm_stuck(0, 93, 93, 10000 * kMs, 0));
	EXPECT_FALSE(tgm_stuck(1, 90, 93, 10000 * kMs, 0));
	EXPECT_TRUE(
		tgm_stuck(0, 0xfffffffe, 2, 10000 * kMs, 0));  // a fence word wrapping past zero
}

struct Candidates {
	std::vector<tgm_hang_candidate> c;
	int late_dead = 0, late_kernel = 0, progress = 0;

	tgm_hang verdict(int &blame) const
	{
		return tgm_hang_blame(c.data(), int(c.size()), late_dead, late_kernel, progress,
				      &blame);
	}
};

// live, stuck, kernel, resident (an engine or PBDMA is on it), moving (fence or GET), waits (1 + the source's index),
// victim, dead (no live channel, or a killed one), loading (an engine stuck loading its context), saving (an engine
// switching away from its context)
constexpr tgm_hang_candidate kStuckClient = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
			     kBusyClient = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
			     kNone = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
			     kStuckResident = {1, 1, 0, 1, 0, 0, 0, 0, 0, 0},
			     kMovingResident = {1, 1, 0, 1, 1, 0, 0, 0, 0, 0},
			     kStuckKernel = {1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
			     kBusyKernel = {1, 0, 1, 0, 0, 0, 0, 0, 0, 0};

constexpr tgm_hang_candidate waiting(tgm_hang_candidate c, int source, int moving = 0)
{
	c.waits = uint8_t(source + 1);
	c.moving = uint8_t(moving);
	return c;
}

TEST(HangBlame, Deploy2045KillsTheContextGrCannotSwitch)
{
	// the present waited for backboardd; GR's switch blames Maps (restoring 26), PBDMA0 is on 26 too
	Candidates k{{kStuckResident, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

// T-v2 19:26:20: GR loading killed channel 20's context (a suspect that is no live channel), backboardd stuck behind it
TEST(HangBlame, AnEngineLeftOnAKilledChannelIsResetNotTheCompositorBehindIt)
{
	tgm_hang_candidate killed = kNone;
	killed.resident = killed.dead = 1;
	Candidates k{{killed, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RESIDUE);
	EXPECT_EQ(blame, 0);
	k.c[0].dead = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING) << "what T-v2 did: the compositor killed";
	EXPECT_EQ(blame, 1);
}

// T-v3e, the present's deadline checks 1 s apart behind ENG0 0004a022 (GR loading backboardd's channel 4, BUSY clear)
// and PBDMA0 on channel 4. nvkm's decode named killed SwitchHWTest 34 as GR's, so the suspects were 34 and 4: the residue
// reset found no engine holding 34, and the second check killed backboardd.
TEST(HangBlame, Tv3eKilledTheCompositorWhoseContextGrWasLoading)
{
	tgm_hang_candidate hwtest = kNone, present = kStuckKernel;
	hwtest.live = hwtest.resident = hwtest.dead = 1;
	present.resident = 0;
	Candidates k{{hwtest, kStuckResident, present}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RESIDUE);
	EXPECT_EQ(blame, 0);
	k.c[0].dead = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING) << "the old fallback";
	EXPECT_EQ(blame, 1) << "backboardd";
	k.c[1].loading = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RELOAD)
		<< "GR is loading 4: reset under it, kill no one";
	EXPECT_EQ(blame, 1);
}

// T-v3g: GR saving the bench's channel 42 (nothing pending), PBDMA0 on backboardd's channel 4 behind it. The old rule
// never found 42 stuck (no work pending) and blamed backboardd once two checks found it still. Now the channel an engine
// is saving is the only one blamed, once two checks confirm the stall; until then nothing is.
TEST(HangBlame, Tv3gBlamesTheChannelGrCannotSaveNeverTheCompositorBehindIt)
{
	tgm_hang_candidate bench = kBusyClient, present = kStuckKernel;
	bench.resident = 1;
	present.resident = 0;
	Candidates k{{bench, kStuckResident, present}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING) << "the old rule";
	EXPECT_EQ(blame, 1) << "backboardd";
	k.c[0].saving = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING) << "first look: no one blamed";
	EXPECT_EQ(blame, -1);
	k.c[0].stuck = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0) << "the bench's channel alone";
}

TEST(HangBlame, AKernelChannelAnEngineCannotSaveIsNoClientsToKill)
{
	tgm_hang_candidate present = kStuckKernel;
	present.saving = 1;
	Candidates k{{present, kStuckResident}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_KERNEL) << "the present's engines are reset";
	EXPECT_EQ(blame, 0);
}

// With NVIDIA's decode the suspects are channel 4 alone (GR loading it, PBDMA0 on it): no channel is killed.
TEST(HangBlame, AnEngineLoadingALiveChannelsContextIsResetUnderIt)
{
	tgm_hang_candidate loading = kStuckResident, present = kStuckKernel;
	loading.loading = 1;
	present.resident = 0;
	Candidates k{{loading, present}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RELOAD);
	EXPECT_EQ(blame, 0);
	k.c[0].loading = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING)
		<< "only a load finished meanwhile leaves the old rule";
}

// T-v3e's last checks: backboardd killed too, GR still loading its context, killed 34 first among the suspects. Each
// dead resident's reset is tried in turn; before, only the first was, and the engine on channel 4 never was.
TEST(HangBlame, EveryDeadChannelAnEngineIsOnGetsItsResetTried)
{
	tgm_hang_candidate first = kNone, second = kNone;
	first.live = first.resident = first.dead = 1;
	second.live = second.resident = second.dead = second.loading = 1;
	Candidates k{{first, second, kStuckKernel}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RESIDUE);
	EXPECT_EQ(blame, 0);
	k.c[0].dead = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RESIDUE);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, ADeadChannelNoEngineIsOnIsNoResidue)
{
	tgm_hang_candidate gone = kStuckClient;
	gone.dead = 1;
	Candidates k{{gone}};
	k.late_dead = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, TheCompositorIsRecoveredWhenItIsTheOneStuck)
{
	// GR stuck saving backboardd's context: suspects 3 (backboardd) then 26 (Maps), late is backboardd
	Candidates k{{kStuckResident, kStuckResident, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, AChannelOnAnEndlessAcquireBlamesItself)
{
	// GR idle, the PBDMA switched away from it (acquire switch): no suspect, the late channel is stuck
	Candidates k{{kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

constexpr tgm_hang_candidate victim(tgm_hang_candidate c)
{
	c.victim = 1;
	return c;
}

// An acquire timeout (gpu_acquire.c): the waiter is the late channel and its RAMFC names the producer, which sits on no
// engine and completes nothing: the producer is killed, never the waiter that waited for it.
TEST(HangBlame, AnAcquireTimeoutKillsTheProducerNotTheWaiter)
{
	Candidates k{{kNone, kStuckClient, waiting(kStuckClient, 1)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, AWaiterOnAProducerThatMovesIsLeftAlone)
{
	Candidates k{{kNone, {1, 1, 0, 0, 1, 0, 0, 0, 0, 0}, waiting(kStuckClient, 1)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

// A bind epoch the kernel's retirement releases, or a host1x syncpoint (a display that is off, a decoder): the channel
// waits on no work of its own and is never killed for it.
TEST(HangBlame, AChannelOnAKernelOrHost1xReleaseIsAVictim)
{
	Candidates k{{kNone, victim(kStuckClient)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
	Candidates chain{{kNone, victim(kStuckClient), waiting(kStuckClient, 1)}};
	EXPECT_EQ(chain.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

// A kernel channel on a display's syncpoint while the display is off: nothing else progresses, and nothing is wrong.
TEST(HangBlame, ALateKernelVictimIsNeverFatal)
{
	Candidates k{{kNone, victim(kBusyKernel)}};
	k.c[1].stuck = 1;
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
	k.c[1].victim = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_KERNEL);
	EXPECT_EQ(blame, 1) << "the late kernel channel's own engines";
}

// iconservicesagent's channel 20 on slice B's boot: GR and PBDMA0 on another process's long 3D work that keeps going,
// channel 20 not resident with work pending behind it, and the late sibling's order acquire on channel 20.
TEST(HangBlame, ChannelsWaitingBehindAnEngineThatMovesAreNotKilled)
{
	Candidates k{{kMovingResident, kStuckClient, waiting(kStuckClient, 1)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

TEST(HangBlame, AResidentChannelThatDoesNotMoveIsKilledNotItsWaiters)
{
	Candidates k{
		{kStuckResident, kStuckClient, waiting(kStuckClient, 1), waiting(kStuckClient, 0)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, WithNoEngineOnThemTheBlameFollowsTheAcquiresToTheEnd)
{
	Candidates k{{kNone, waiting(kStuckClient, 2), kStuckClient, waiting(kStuckClient, 1)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 2);
}

TEST(HangBlame, AnAcquireOnASourceThatMovesIsSlowWork)
{
	Candidates k{{kNone, {1, 1, 0, 0, 1, 0, 0, 0, 0, 0}, waiting(kStuckClient, 1)}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

TEST(HangBlame, SlowWorkIsNotKilled)
{
	Candidates k{{kBusyClient, kNone, kBusyClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

TEST(HangBlame, AStuckKernelChannelHasItsEnginesReset)
{
	Candidates k{{kStuckKernel, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_KERNEL);
	EXPECT_EQ(blame, 0);
}

// T-v5a 195.8 s: "scrub ch 0 ... fence 686/750 ... ENG0 001b201b (ch 27 -> ch 27, ctxsw valid), PBDMA0 00002000 ch 0
// mthd 80040300": the scrubber on PBDMA0 alone (GR idle, not a suspect), the scrubber also the late channel
TEST(HangBlame, Tv5asScrubberStuckOnThePbdmaWithEveryEngineIdleIsTheKernelsToRecover)
{
	tgm_hang_candidate late = kStuckKernel;
	late.resident = 0;
	Candidates k{{kStuckKernel, late}};
	k.late_kernel = 1;
	k.progress = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_KERNEL);
	EXPECT_EQ(blame, 0);
	k.c[0].stuck = k.c[1].stuck = 0;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING) << "the first look: one strike";
}

TEST(HangBlame, LateKernelWorkWithNothingProgressingHasItsEnginesReset)
{
	Candidates k{{kNone, kBusyKernel}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_KERNEL);
	EXPECT_EQ(blame, 1);
	k.progress = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
}

TEST(HangBlame, ALateChannelAlreadyKilledIsRecovering)
{
	Candidates k{{kNone, kBusyClient}};
	k.late_dead = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, -1);
}

// nvkm's MMU fault line as gf100_fifo_mmu_fault_recover formats it, behind the device name nvkm_error puts in front
std::string FaultLine(uint64_t va, int chid)
{
	char line[256];
	std::snprintf(line, sizeof line,
		      "tegra-gm20b 57000000.gpu: fifo: fault %02x [%s] at %016llx engine %02x [%s] "
		      "client %02x "
		      "[%s%s] reason %02x [%s] on channel %d [%010llx %s]",
		      1, "WRITE", static_cast<unsigned long long>(va), 0, "GR", 0x0f, "GPC0/",
		      "PROP_0", 2, "PTE", chid, 0x2004a000ULL, "backboardd");
	return line;
}

TEST(FaultVa, TheMmuFaultLineNamesIt)
{
	EXPECT_EQ(tgm_fault_va(FaultLine(0x6000090040ULL, 7).c_str(), 7), 0x6000090040ULL);
}

TEST(FaultVa, TheKilledChannelsOwnAmongAPassesReports)
{
	const std::string pass = FaultLine(0x1000, 3) + " | " + FaultLine(0x6002450500ULL, 9);
	EXPECT_EQ(tgm_fault_va(pass.c_str(), 9), 0x6002450500ULL);
	EXPECT_EQ(tgm_fault_va(pass.c_str(), 3), 0x1000u);
	EXPECT_EQ(tgm_fault_va(pass.c_str(), 5), 0u) << "another channel's fault is not its own";
	EXPECT_EQ(tgm_fault_va(FaultLine(0x2000, -1).c_str(), 1), 0x2000u)
		<< "one nvkm could not place";
}

TEST(FaultVa, NoneWithoutAFaultLine)
{
	EXPECT_EQ(
		tgm_fault_va("tegra-gm20b 57000000.gpu: gr: TRAP ch 7 [002004a000 backboardd]", 7),
		0u);
	EXPECT_EQ(tgm_fault_va("gr: DATA_ERROR 0000000c [] ch 7 | fifo: CHSW_ERROR 00000001", 7),
		  0u);
	EXPECT_EQ(tgm_fault_va("", 7), 0u);
}

}  // namespace

TEST(HangConfirm, AChannelIsBlamedOnlyAtTheSecondCheckThatFindsItStill)
{
	uint32_t strikes = 0, seen = 0;
	EXPECT_FALSE(tgm_hang_confirm(1, 40, &strikes, &seen));
	EXPECT_TRUE(tgm_hang_confirm(1, 40, &strikes, &seen));
}

TEST(HangConfirm, ALongJobThatCompletesBetweenChecksStartsOver)
{
	uint32_t strikes = 0, seen = 0;
	EXPECT_FALSE(tgm_hang_confirm(1, 40, &strikes, &seen));
	EXPECT_FALSE(tgm_hang_confirm(1, 41, &strikes, &seen));
	EXPECT_TRUE(tgm_hang_confirm(1, 41, &strikes, &seen));
	EXPECT_FALSE(tgm_hang_confirm(0, 41, &strikes, &seen));
	EXPECT_FALSE(tgm_hang_confirm(1, 41, &strikes, &seen));
}

// PBDMA0 at sliceV boot 2's three hangs (METHOD0, DATA0): backboardd's channel and the present's, each on a
// SEMAPHORED ACQ_GEQ with ACQUIRE_SWITCH.
TEST(HangSuspects, APbdmaParkedOnASemaphoreAcquireNamesNoSuspect)
{
	EXPECT_TRUE(tgm_pbdma_acquiring(0x8000001d, 0x00001004));
	EXPECT_TRUE(tgm_pbdma_acquiring(0x0000001d, 0x00001004));
	EXPECT_TRUE(tgm_pbdma_acquiring(0x8000001d, 0x00000001));
	EXPECT_TRUE(tgm_pbdma_acquiring(0x8000001d, 0x00001008));
	EXPECT_FALSE(tgm_pbdma_acquiring(0x8000001d, 0x01100002));  // a release
	EXPECT_FALSE(tgm_pbdma_acquiring(0x80000021, 0x00000000));  // NON_STALL_INTERRUPT
	EXPECT_FALSE(
		tgm_pbdma_acquiring(0x80010e01, 0x00001004));  // a 3D method with ACQ_GEQ-like data
}

// The panic: the present waited 2 s for backboardd's fence; GR idle, PBDMA0 between the two channels' acquires and on
// the present's at two consecutive checks. As a suspect the present is a stuck kernel channel (fatal); left out, the
// late channel is the one recovered.
TEST(HangBlame, SliceVBoot2ThePresentOnItsAcquireLeavesTheBlameToItsProducer)
{
	const bool presentResident = !tgm_pbdma_acquiring(0x0000001d, 0x00001004);
	Candidates k;
	if (presentResident)
		k.c.push_back(kStuckKernel);
	k.c.push_back(kStuckClient);
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
	Candidates before{{kStuckKernel, kStuckClient}};
	EXPECT_EQ(before.verdict(blame), TGM_HANG_KERNEL);
	EXPECT_EQ(blame, 0);
}
