// SPDX-License-Identifier: MIT
// TegraGM20BRecover.h's scheduler error names, the decision on an engine whose switch timed out, and which PBDMA
// interrupts nvkm's handler still gets, over device boot 7 (iOS 27): GR reset for iconservicesagent's trapped channel 10
// while PBDMA0 served backboardd's channel 4 (ENG_RESET), then GR stuck switching a killed channel's context, a
// CTXSW_TIMEOUT tick every 256 ms and SCHED_ERROR 0x0d.
#include <cstdint>
#include <cstdio>
#include <string>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BHangDecode.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint64_t kTick = 256 * kMs;

TEST(SchedError, Boot7sPanicCodeIsANewRunlist)
{
	EXPECT_EQ(std::string(tgm_sched_error_name(0x0d)), "NEW_RUNLIST");
}

TEST(SchedError, EveryCodeOfTheHostManualHasItsName)
{
	const struct {
		uint32_t code;
		const char *name;
	} codes[] = {
		{0x00, "NO_ERROR"},          {0x05, "ENGINE_RESET"},    {0x06, "RL_ACK_TIMEOUT"},
		{0x07, "RL_ACK_EXTRA"},      {0x08, "RL_RDAT_TIMEOUT"}, {0x09, "RL_RDAT_EXTRA"},
		{0x0a, "CTXSW_TIMEOUT"},     {0x0c, "RL_REQ_TIMEOUT"},  {0x0d, "NEW_RUNLIST"},
		{0x0e, "CONFIG_WHILE_BUSY"}, {0x20, "BAD_TSG"}};

	for (const auto &c : codes)
		EXPECT_EQ(std::string(tgm_sched_error_name(c.code)), c.name) << c.code;
	EXPECT_EQ(TGM_SCHED_CTXSW_TIMEOUT, 0x0au);
}

TEST(SchedError, AnUnlistedCodeIsUnknownNotAnError)
{
	EXPECT_EQ(std::string(tgm_sched_error_name(0x0b)), "unknown");
	EXPECT_EQ(std::string(tgm_sched_error_name(0xff)), "unknown");
}

TEST(BindError, Boot9sPanicCodeIsAnUnbindWhileRunning)
{
	EXPECT_EQ(std::string(tgm_bind_error_name(0x03)), "UNBIND_WHILE_RUNNING");
	EXPECT_EQ(std::string(tgm_bind_error_name(0x0b)), "UNBIND_WHILE_PARKED");
	EXPECT_EQ(std::string(tgm_bind_error_name(0x04)), "unknown");
}

constexpr uint32_t kBusy = 0x80000000u;

uint32_t engine(uint32_t ctx, uint32_t id, uint32_t next)
{
	return kBusy | next << 16 | ctx << 13 | id;
}

TEST(Unbind, AnEngineRunningOrSavingTheChannelHoldsIt)
{
	EXPECT_TRUE(tgm_engine_holds(engine(1, 17, 0), 17));   // resident
	EXPECT_TRUE(tgm_engine_holds(engine(6, 17, 4), 17));   // saving it out
	EXPECT_TRUE(tgm_engine_holds(engine(7, 17, 4), 17));   // switching away from it
	EXPECT_FALSE(tgm_engine_holds(engine(1, 17, 0), 11));  // another channel's
}

TEST(Unbind, AnEngineLoadingTheChannelHoldsIt)
{
	EXPECT_TRUE(tgm_engine_holds(engine(5, 4, 17), 17));
	EXPECT_TRUE(tgm_engine_holds(engine(7, 4, 17), 17));
	EXPECT_FALSE(tgm_engine_holds(engine(6, 4, 17), 17));  // NEXT_ID means nothing while saving
}

// T-v2 19:26:20: after channel 20's kill GR sat 3 s in 0x0014a014, a context load of channel 20 with BUSY clear
TEST(Unbind, AnEngineWaitingOnALoadHoldsTheChannelThoughNotBusy)
{
	EXPECT_TRUE(tgm_engine_holds(0x0014a014u, 20));
	EXPECT_FALSE(tgm_engine_holds(0x0014a014u, 1));
	EXPECT_TRUE(tgm_engine_holds(engine(6, 17, 4) & ~kBusy, 17)) << "saving it out";
}

// gk104_engn_status's choice of channel (nvkm/engine/fifo/gk104.c), the decode nvkm used before engn_cxid replaced it
int nouveauCxid(uint32_t stat, bool chswLoad)
{
	const bool busy = stat & 0x80000000u, chsw = stat & 0x00008000u, save = stat & 0x00004000u,
		   load = stat & 0x00002000u;
	const int prev = int(stat & 0xfffu), next = int((stat >> 16) & 0xfffu);

	if (busy && chsw) {
		if (load && save)
			return chswLoad ? next : prev;
		return load ? next : prev;
	}
	return load ? prev : -1;
}

// T-v3e 165.8 s: ENG0 0004a022 "ch 34 -> ch 4, ctxsw load", BUSY clear: GR loads backboardd's channel 4, switched away from
// SwitchHWTest's killed 34. nvkm named 34, so the hang rule's residue reset found no engine on 34 and killed channel 4.
TEST(EngineContext, ALoadWithBusyClearIsOnTheChannelItLoads)
{
	constexpr uint32_t tv3e = 0x0004a022u, tv2 = 0x0014a014u;
	int tsg = -1;

	EXPECT_EQ(PFIFO_ENG_CTX_STATUS(tv3e), uint32_t(PFIFO_ENG_CTX_LOAD));
	EXPECT_EQ(tgm_engine_chid(tv3e, 0, &tsg), 4);
	EXPECT_EQ(tsg, 0);
	EXPECT_EQ(nouveauCxid(tv3e, false), 34)
		<< "what nvkm's recovery and the hang rule were told";
	EXPECT_TRUE(tgm_engine_holds(tv3e, 4));
	EXPECT_FALSE(tgm_engine_holds(tv3e, 34));
	EXPECT_EQ(tgm_engine_chid(tv2, 0, &tsg), 20);
}

TEST(EngineContext, EveryStateNamesTheChannelTheTeardownSequenceReads)
{
	int tsg = -1;

	EXPECT_EQ(tgm_engine_chid(engine(1, 17, 4), 0, &tsg), 17);  // VALID: ID
	EXPECT_EQ(tgm_engine_chid(engine(6, 17, 4), 0, &tsg), 17);  // SAVE: ID
	EXPECT_EQ(tgm_engine_chid(engine(5, 17, 4), 0, &tsg), 4);   // LOAD: NEXT_ID
	EXPECT_EQ(tgm_engine_chid(engine(7, 17, 4), 0, &tsg), 17);  // SWITCH before FECS loads
	EXPECT_EQ(tgm_engine_chid(engine(7, 17, 4), 1, &tsg), 4);   // and once it does
	EXPECT_EQ(tgm_engine_chid(engine(0, 17, 4), 0, &tsg), -1);  // INVALID
	EXPECT_EQ(tgm_engine_chid(engine(1, 17, 4) | 0x00001000u, 0, &tsg), 17);
	EXPECT_EQ(tsg, 1);
	EXPECT_EQ(tgm_engine_chid(engine(5, 17, 4) | 0x10000000u, 0, &tsg), 4);
	EXPECT_EQ(tsg, 1);
	for (uint32_t ctx : {1u, 6u, 7u})
		EXPECT_EQ(tgm_engine_chid(engine(ctx, 17, 4), 0, &tsg),
			  nouveauCxid(engine(ctx, 17, 4), false))
			<< "a BUSY engine's channel is nvkm's in every other state: " << ctx;
}

// Both device stalls a GR reset left (T-v2 0014a014, T-v3e 0004a022) are a switch pending with BUSY clear in LOAD, which
// gk104_engn_chsw (BUSY and CTXSW) never counted: every CTXSW_TIMEOUT tick for them reached no decision.
TEST(EngineContext, AStuckLoadIsASwitchTheTicksDecideOn)
{
	for (uint32_t stall : {0x0004a022u, 0x0014a014u}) {
		EXPECT_TRUE(tgm_engine_load_stuck(stall)) << std::hex << stall;
		EXPECT_TRUE(tgm_engine_switch_pending(stall)) << std::hex << stall;
		EXPECT_EQ(nouveauCxid(stall, false), int(stall & 0xfffu));
	}
	EXPECT_FALSE(tgm_engine_load_stuck(engine(5, 34, 4))) << "a BUSY load is nvkm's switch";
	EXPECT_TRUE(tgm_engine_switch_pending(engine(5, 34, 4)));
	EXPECT_FALSE(tgm_engine_load_stuck(engine(6, 34, 4) & ~kBusy)) << "a save";
	EXPECT_FALSE(tgm_engine_load_stuck(engine(1, 34, 0) & ~kBusy));
	EXPECT_FALSE(tgm_engine_switch_pending(engine(1, 34, 0)));
	EXPECT_FALSE(tgm_engine_switch_pending(0x00000022u));
}

TEST(EngineContext, AStuckLoadResetsTheEngineAndKillsNoOne)
{
	EXPECT_EQ(tgm_load_verdict(1, 0, tgm_load_reloaded(0, 0, 3636)), TGM_CTXSW_RELOAD);
	EXPECT_EQ(tgm_load_verdict(1, 1, 0), TGM_CTXSW_RESET)
		<< "a killed channel's load is residue";
	EXPECT_EQ(tgm_load_verdict(0, 0, 0), TGM_CTXSW_RESET) << "no channel's";
}

TEST(EngineContext, TheSameLoadStuckAgainWithNothingCompletedIsFatal)
{
	EXPECT_EQ(tgm_load_verdict(1, 0, tgm_load_reloaded(1, 3632, 3632)), TGM_CTXSW_FATAL);
	EXPECT_EQ(tgm_load_verdict(1, 0, tgm_load_reloaded(1, 3632, 3633)), TGM_CTXSW_RELOAD)
		<< "it completed work after the reset: a new stall";
}

// T-v3e's PBDMA0 00042004 "ch 4 mthd 80001428 data 00000000": channel 4 loaded (CHAN_STATUS VALID) on a 3D method
// (subchannel 0, 0x1428 INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI), not parked on an acquire
TEST(EngineReset, TheCompositorOnThePbdmaIsPreemptedAtTheHold)
{
	constexpr uint32_t pbdma = 0x00042004u;
	EXPECT_NE(pbdma & PBDMA_BUSY, 0u);
	EXPECT_EQ(PBDMA_CHID(pbdma), 4u);
	EXPECT_EQ(PBDMA_METHOD_ADDR(0x80001428u), 0x1428u);
	EXPECT_FALSE(tgm_pbdma_acquiring(0x80001428u, 0));
	EXPECT_EQ(tgm_reset_pbdma(pbdma, 1), TGM_RESET_PBDMA_LIVE);
	EXPECT_EQ(tgm_reset_pbdma(pbdma, 0), TGM_RESET_PBDMA_DEAD);
	EXPECT_EQ(tgm_reset_pbdma(0, 1), TGM_RESET_PBDMA_IDLE);
}

TEST(EngineReset, OnlyTheOutermostOfNestedResetsHoldsAndReleases)
{
	uint32_t holds = 0;
	EXPECT_TRUE(tgm_reset_hold_enter(&holds));   // tgm_engine_reset_locked
	EXPECT_FALSE(tgm_reset_hold_enter(&holds));  // GR's own reset inside it
	EXPECT_FALSE(tgm_reset_hold_leave(&holds));
	EXPECT_TRUE(tgm_reset_hold_leave(&holds));
	EXPECT_FALSE(tgm_reset_hold_leave(&holds)) << "a release with no hold is no release";
	EXPECT_EQ(holds, 0u);
}

// T-v3g 4471 s: "GR 802ac02a current{SwitchHWTest ch 42 fence 3/3 err 0} ... PGRAPH 01000081 ENG0 802ac02a (ch 42 -> ch 42,
// ctxsw save), PBDMA0 00042004 ch 4 mthd 80001424": GR busy saving channel 42, whose jobs had all completed (a 2D solid
// fill with a float colour format into an 8-bit target left GR unable to idle), backboardd's channel 4 on the PBDMA
// behind it. Every tick said WAIT: the stuck rule needs work pending, and 42 had none.
TEST(StuckSave, Tv3gsSaveOfAChannelWithNothingPendingIsDecidedAtTheFirstTick)
{
	constexpr uint32_t eng0 = 0x802ac02au, pgraph = 0x01000081u, pbdma = 0x00042004u;
	EXPECT_TRUE(eng0 & PFIFO_ENG_BUSY);
	EXPECT_EQ(PFIFO_ENG_CTX_STATUS(eng0), uint32_t(PFIFO_ENG_CTX_SAVE));
	EXPECT_EQ(tgm_engine_saving(eng0, 0), 42);
	EXPECT_TRUE(tgm_engine_switch_pending(eng0));
	EXPECT_FALSE(tgm_engine_load_stuck(eng0));
	EXPECT_TRUE(tgm_engine_holds(eng0, 42));
	EXPECT_NE(pgraph & 1u, 0u) << "GR busy";
	EXPECT_EQ(PBDMA_CHID(pbdma), 4u);
	EXPECT_EQ(PBDMA_METHOD_ADDR(0x80001424u), 0x1424u);
	const uint64_t tick = 250 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 3, 3, tick, 0)) << "what the rule saw at every tick";
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, tgm_stuck(0, 3, 3, tick, 0), 0), TGM_CTXSW_WAIT);
	EXPECT_TRUE(tgm_switch_unowned(0, 3, 3));
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, tgm_switch_unowned(0, 3, 3), 0), TGM_CTXSW_KILL);
	EXPECT_FALSE(tgm_switch_unowned(1, 3, 3)) << "a killed channel's context is residue";
	EXPECT_FALSE(tgm_switch_unowned(0, 2, 3)) << "work pending: the stuck rule decides";
}

// The present's or the scrubber's channel with nothing pending, an engine stuck switching off it. T-v4's first tick redid
// nothing and set the redo mark, and any later stall of the still idle channel panicked: the present is idle for minutes
// under direct scanout. Nothing of an idle channel is lost to a reset, so every such stall is a reset that kills no one.
TEST(KernelStall, AnIdleKernelChannelIsResetAtEveryStallAndKillsNoOne)
{
	const uint32_t done = 9;
	const int idle = tgm_switch_unowned(0, done, done);
	ASSERT_TRUE(idle);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, idle, tgm_ctxsw_redo_pending(1, done, done)),
		  TGM_CTXSW_FATAL)
		<< "what T-v4 did with the second stall";
	for (const int again : {0, 1}) {
		EXPECT_EQ(tgm_kernel_stall_verdict(idle, 0, 0, 0), TGM_CTXSW_RESET);
		EXPECT_EQ(tgm_kernel_stall_verdict(idle, 0, 0, 2), TGM_CTXSW_RESET);
		EXPECT_FALSE(tgm_kernel_stall_kills(idle, again, 0, 2)) << again;
	}
	const uint64_t reset = 1000 * kMs;
	EXPECT_FALSE(tgm_engine_not_back(reset, done, reset + 60 * 1000 * kMs, done))
		<< "a stall a minute after a reset that worked is a new one";
}

// The present's copy cut by an engine reset is redone; stuck again before the redo landed (T-v4: fatal), the client
// contexts the GPU holds are killed and the engine reset again.
TEST(KernelStall, AKernelChannelStuckAgainWithItsWorkKillsTheResidentClientsAndRedoes)
{
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 0, 0, 1), TGM_CTXSW_REDO);
	EXPECT_FALSE(tgm_kernel_stall_kills(0, 0, 0, 1)) << "the first stall kills no one";
	EXPECT_TRUE(tgm_kernel_stall_kills(0, 1, 0, 1));
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 0, 0, 0), TGM_CTXSW_REDO)
		<< "again, nothing resident: reset and redo";
	EXPECT_FALSE(tgm_kernel_stall_kills(0, 1, 0, 0));
}

// The one fatal row: the engine did not come back after a reset with no client context resident.
TEST(KernelStall, OnlyAnEngineThatDidNotComeBackAfterACleanResetIsFatal)
{
	const uint64_t reset = 5000 * kMs;
	const uint32_t done = 12;
	const uint64_t secondTick = reset + 200 * kMs;
	ASSERT_TRUE(tgm_engine_not_back(reset, done, secondTick, done));
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 0, 1, 0), TGM_CTXSW_FATAL);
	EXPECT_EQ(tgm_kernel_stall_verdict(1, 0, 1, 0), TGM_CTXSW_FATAL)
		<< "the engine, not the idle channel";
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 0, 1, 1), TGM_CTXSW_REDO)
		<< "a client is resident: it goes first";
	EXPECT_TRUE(tgm_kernel_stall_kills(1, 0, 1, 1));
	EXPECT_FALSE(tgm_engine_not_back(0, done, secondTick, done))
		<< "the last reset had a client resident";
	EXPECT_FALSE(tgm_engine_not_back(reset, done, secondTick, done + 1))
		<< "T-v3 251 s: the interrupted job completed after the reset (12 -> 13)";
	EXPECT_FALSE(tgm_engine_not_back(reset, done, reset + TGM_ENGINE_BACK_NS, done));
}

// A kernel channel's context the engine cannot load again after a reset under it: the same rule, the reset under
// the load (a client's own is killed: gpu_reload.c)
TEST(KernelStall, AKernelChannelsStuckLoadIsReloadedFatalOnlyForAnEngineNotBack)
{
	EXPECT_EQ(tgm_load_verdict(1, 0, tgm_load_reloaded(1, 3632, 3632)), TGM_CTXSW_FATAL)
		<< "T-v4, before";
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 1, 0, 1), TGM_CTXSW_RELOAD);
	EXPECT_TRUE(tgm_kernel_stall_kills(0, 1, 0, 1));
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 1, 0, 0), TGM_CTXSW_RELOAD);
	EXPECT_EQ(tgm_kernel_stall_verdict(0, 1, 1, 0), TGM_CTXSW_FATAL);
}

TEST(KernelStall, TheResidentContextsAreEachEnginesHeldAndIncomingChannels)
{
	int held[2] = {-1, -1};
	EXPECT_EQ(tgm_engine_contexts(0x802ac02au, held), 1) << "T-v3g: GR saving channel 42";
	EXPECT_EQ(held[0], 42);
	EXPECT_EQ(tgm_engine_contexts(engine(7, 22, 4), held), 2)
		<< "a switch from the present to a client";
	EXPECT_EQ(held[0], 22);
	EXPECT_EQ(held[1], 4);
	EXPECT_EQ(tgm_engine_contexts(engine(5, 22, 4), held), 1);
	EXPECT_EQ(held[0], 4);
	EXPECT_EQ(tgm_engine_contexts(engine(1, 22, 4) & ~kBusy, held), 0) << "idle";
	EXPECT_EQ(tgm_engine_contexts(engine(6, 22, 4) | 0x00001000u, held), 0) << "a TSG";
}

TEST(StuckSave, OnlyASwitchAwayFromTheContextIsASave)
{
	EXPECT_EQ(tgm_engine_saving(engine(6, 42, 4), 0), 42);
	EXPECT_EQ(tgm_engine_saving(engine(7, 42, 4), 0), 42);
	EXPECT_EQ(tgm_engine_saving(engine(7, 42, 4), 1), -1) << "FECS loading the next";
	EXPECT_EQ(tgm_engine_saving(engine(5, 42, 4), 0), -1);
	EXPECT_EQ(tgm_engine_saving(engine(1, 42, 0), 0), -1);
	EXPECT_EQ(tgm_engine_saving(engine(6, 42, 4) & ~kBusy, 0), -1);
	EXPECT_EQ(tgm_engine_saving(engine(6, 42, 4) | 0x00001000u, 0), -1) << "a TSG";
}

// T-v3g 13:31:15-32: 32 copy-engine saves stuck under HW Test's gpu.astc.layouts, each killed 517-553 ms after its
// submit: the 250 ms ticks waited for the 500 ms stuck rule. A copy engine's work is never long and finite, and the
// ticks now come every 100 ms (nvgpu's period).
TEST(StuckSave, ACopyEngineSwitchIsDecidedAtTheFirstTickAndGrsLongJobsAreWaitedOut)
{
	constexpr uint64_t tick = 100 * kMs;
	const uint64_t kick = 1000 * kMs, firstTick = kick + tick + 20 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 2934, 2935, firstTick, kick));
	EXPECT_TRUE(tgm_stuck_for(0, 2934, 2935, firstTick, kick, tgm_stuck_window(0)));
	EXPECT_FALSE(tgm_stuck_for(0, 2934, 2935, firstTick, kick, tgm_stuck_window(1)))
		<< "GR jobs up to 428 ms were measured";
	EXPECT_LT(TGM_STUCK_COPY_NS, tick);
	EXPECT_GT(TGM_STUCK_NS, 428 * kMs);
}

// T-v5a 195.8 s: "scrub ch 0 err 0 fence 686/750 ... ENG0 001b201b (ch 27 -> ch 27, ctxsw valid), PBDMA0 00002000 ch 0
// mthd 80040300 data 00000786": the scrubber's LAUNCH_DMA (subchannel 4, 0x300) held on PBDMA0, GR idle on client 27's
// context, no switch pending anywhere. The last resort reset only busy or switching engines (none) and the next check
// panicked. Its own step resets the copy engine whatever it holds while idle, and not GR, whose idle context is 27's.
TEST(KernelHang, Tv5asScrubberStallResetsTheIdleCopyEngineAndLeavesGrsClientContext)
{
	constexpr uint32_t eng0 = 0x001b201bu, pbdma = 0x00002000u;
	EXPECT_FALSE(eng0 & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW));
	EXPECT_EQ(PBDMA_CHID(pbdma), 0u);
	EXPECT_NE(pbdma & PBDMA_BUSY, 0u);
	EXPECT_EQ(PBDMA_METHOD_ADDR(0x80040300u), 0x300u);
	EXPECT_EQ((0x80040300u >> 16) & 7, 4u) << "the copy subchannel";
	int held[2] = {-1, -1};
	int none[2];
	EXPECT_EQ(tgm_engine_contexts(eng0, none), 0)
		<< "what the stall rules saw: nothing resident";
	ASSERT_EQ(tgm_engine_contexts_held(eng0, held), 1);
	EXPECT_EQ(held[0], 27);
	EXPECT_FALSE(tgm_kernel_hang_resets(eng0, 0, 1, 0)) << "GR keeps client 27's context";
	EXPECT_TRUE(tgm_kernel_hang_resets(eng0, 0, 0, 0))
		<< "a killed client's is nothing to keep";
	for (const uint32_t ce : {0x00000000u, 0x00002000u, 0x001b201bu})
		EXPECT_TRUE(tgm_kernel_hang_resets(ce, ce == 0x00002000u, ce == 0x001b201bu, 1))
			<< std::hex << ce;
	EXPECT_FALSE(tgm_kernel_hang_resets(engine(1, 27, 0), 0, 1, 1))
		<< "a copy engine busy with a live client's copy";
	EXPECT_TRUE(tgm_kernel_hang_resets(engine(1, 0, 0), 1, 0, 1)) << "busy with the scrubber's";
	const tgm_kernel_hang_state fresh = {};
	EXPECT_EQ(tgm_kernel_hang_next(&fresh, 686, 0, 195785 * kMs), TGM_KERNEL_HANG_OWN);
}

// One stall, checks a second apart: OWN, ALL (the clients on it killed), ALL again once they are gone, then fatal.
TEST(KernelHang, AStepThatFreesNothingEscalatesAndOnlyACleanResetOfEveryEngineIsFatal)
{
	const uint32_t done = 686;
	uint64_t now = 1000 * kMs;
	tgm_kernel_hang_state s = {};
	ASSERT_EQ(tgm_kernel_hang_next(&s, done, 0, now), TGM_KERNEL_HANG_OWN);
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_OWN, 0, -1, 0, done, now);
	now += 1000 * kMs;
	ASSERT_EQ(tgm_kernel_hang_next(&s, done, 0, now), TGM_KERNEL_HANG_ALL);
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_ALL, 2, -1, 0, done, now);
	now += 1000 * kMs;
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, 0, now), TGM_KERNEL_HANG_ALL)
		<< "clients were resident at that reset: they are gone now, reset again";
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_ALL, 0, -1, 0, done, now);
	now += 1000 * kMs;
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, 0, now), TGM_KERNEL_HANG_FATAL);
	EXPECT_EQ(tgm_kernel_hang_next(&s, done + 1, 0, now), TGM_KERNEL_HANG_OWN)
		<< "it completed work since: a new stall";
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, 1, now), TGM_KERNEL_HANG_OWN)
		<< "another channel completed work since: the GPU came back";
}

// The clean ALL step's hold asked for the channel to leave its PBDMA: if that preempt landed and the channel is stuck
// again, the host and the engines came back and what is stuck is the channel's own state, never a hardware failure.
TEST(KernelHang, AChannelThatLeftItsPbdmaAndStalledAgainIsNotAHardwareFailure)
{
	const uint32_t done = 686;
	const uint64_t step = 5000 * kMs, check = step + 1000 * kMs;
	tgm_kernel_hang_state s = {};
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_OWN, 0, 0, 1, done, step - 1000 * kMs);
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_ALL, 0, 0, 1, done, step);
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, tgm_kernel_hang_moved(&s, 0, 0, 0), check),
		  TGM_KERNEL_HANG_OWN)
		<< "its preempt landed";
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, tgm_kernel_hang_moved(&s, 0, 1, 0), check),
		  TGM_KERNEL_HANG_FATAL)
		<< "its preempt still in flight a second on: the host cannot take it off";
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, tgm_kernel_hang_moved(&s, check, 1, 0), check),
		  TGM_KERNEL_HANG_OWN)
		<< "another channel completed work";
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, tgm_kernel_hang_moved(&s, 0, 1, 27), check),
		  TGM_KERNEL_HANG_OWN)
		<< "the PBDMA is on another channel: the stall now is a new one";
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_ALL, 0, -1, 0, done, step);
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, tgm_kernel_hang_moved(&s, 0, 0, -1), check),
		  TGM_KERNEL_HANG_FATAL)
		<< "it was on no PBDMA at the reset and nothing moved";
}

// The scrubber's and the present's deadlines, a CPU wait and a copy queue's check can all find the channel stuck within
// milliseconds: a step's reset gets its time before the next step, whoever asks.
TEST(KernelHang, WaitersAtTheSameMomentTakeOneStep)
{
	const uint32_t done = 686;
	const uint64_t step = 5000 * kMs;
	tgm_kernel_hang_state s = {};
	tgm_kernel_hang_took(&s, TGM_KERNEL_HANG_OWN, 0, -1, 0, done, step);
	for (const uint64_t after : {1 * kMs, 3 * kMs, 200 * kMs, TGM_KERNEL_HANG_GAP_NS - 1})
		EXPECT_EQ(tgm_kernel_hang_next(&s, done, 0, step + after), TGM_KERNEL_HANG_WAIT)
			<< after / kMs;
	EXPECT_EQ(tgm_kernel_hang_next(&s, done, 0, step + TGM_KERNEL_HANG_GAP_NS),
		  TGM_KERNEL_HANG_ALL);
	EXPECT_EQ(tgm_kernel_hang_next(&s, done + 1, 0, step + 1 * kMs), TGM_KERNEL_HANG_OWN)
		<< "its own completion: a new stall at once";
}

// The channel stuck again after every OWN while the rest of the GPU runs: what does not come back is its own state, and
// every few OWN steps one resets every engine. Never fatal: the GPU runs.
TEST(KernelHang, AChannelStuckAgainAfterEveryOwnStepWhileOthersRunGetsAnAllStepAndNeverAPanic)
{
	const uint32_t done = 686;
	uint64_t now = 1000 * kMs;
	tgm_kernel_hang_state s = {};
	int alls = 0;
	for (int check = 0; check < 12; check++, now += 1000 * kMs) {
		const tgm_kernel_hang step = tgm_kernel_hang_next(&s, done, check > 0, now);
		ASSERT_NE(step, TGM_KERNEL_HANG_FATAL) << check;
		ASSERT_NE(step, TGM_KERNEL_HANG_WAIT) << check;
		alls += step == TGM_KERNEL_HANG_ALL;
		tgm_kernel_hang_took(&s, step, 0, -1, 0, done, now);
	}
	EXPECT_EQ(alls, 3);
}

// A reset while another preempt is in flight would leave the stuck channel on its PBDMA (the hold asks for none then)
TEST(KernelHang, AStepWaitsForAnotherPreemptInFlightAFewChecksAtMost)
{
	const uint32_t done = 686;
	uint64_t now = 1000 * kMs;
	tgm_kernel_hang_state s = {};
	for (int check = 1; check <= TGM_KERNEL_HANG_WAITS; check++, now += 1000 * kMs) {
		ASSERT_EQ(tgm_kernel_hang_next(&s, done, 0, now), TGM_KERNEL_HANG_OWN) << check;
		EXPECT_TRUE(tgm_kernel_hang_put_off(&s, TGM_KERNEL_HANG_OWN, 1, done, now))
			<< check;
		EXPECT_EQ(tgm_kernel_hang_next(&s, done, 0, now + 1 * kMs), TGM_KERNEL_HANG_WAIT)
			<< "other waiters meanwhile: " << check;
	}
	EXPECT_FALSE(tgm_kernel_hang_put_off(&s, TGM_KERNEL_HANG_OWN, 1, done, now))
		<< "one that never finishes: the step is taken";
	tgm_kernel_hang_state idle = {};
	EXPECT_FALSE(tgm_kernel_hang_put_off(&idle, TGM_KERNEL_HANG_OWN, 0, done, now));
}

TEST(KernelHang, AChannelGroupsContextIsAClientsTheOwnStepKeeps)
{
	const uint32_t gr = engine(1, 3, 3) & ~kBusy;
	EXPECT_FALSE(tgm_engine_holds_tsg(gr));
	EXPECT_TRUE(tgm_engine_holds_tsg(gr | 0x00001000u));
	EXPECT_TRUE(tgm_engine_holds_tsg(engine(5, 3, 4) | 0x10000000u)) << "loading a group";
	EXPECT_FALSE(tgm_engine_holds_tsg(engine(0, 3, 4) | 0x00001000u)) << "INVALID";
	EXPECT_FALSE(tgm_kernel_hang_resets(gr | 0x00001000u, 0,
					    tgm_engine_holds_tsg(gr | 0x00001000u), 0));
}

TEST(KernelHang, AnEngineHoldsItsIdleValidContextAndBothSidesOfASwitch)
{
	int held[2] = {-1, -1};
	EXPECT_EQ(tgm_engine_contexts_held(engine(1, 17, 0) & ~kBusy, held), 1);
	EXPECT_EQ(held[0], 17);
	EXPECT_EQ(tgm_engine_contexts_held(engine(7, 17, 4) & ~kBusy, held), 2);
	EXPECT_EQ(held[0], 17);
	EXPECT_EQ(held[1], 4);
	EXPECT_EQ(tgm_engine_contexts_held(engine(5, 17, 4), held), 1);
	EXPECT_EQ(held[0], 4);
	EXPECT_EQ(tgm_engine_contexts_held(engine(0, 17, 4), held), 0) << "INVALID";
	EXPECT_EQ(tgm_engine_contexts_held(engine(1, 17, 0) | 0x00001000u, held), 0) << "a TSG";
}

// nvkm's recovery holds runl->mutex through its runlist update and preempt waits (2 s each, recovery.c): a kill still
// under way is waited for, one never scheduled is asked for again, and only past both is it lost.
TEST(IsolateDeadline, AKillNvkmIsStillDoingIsWaitedForNotFatal)
{
	EXPECT_EQ(tgm_iso_verdict(1, 600 * kMs, 0), TGM_ISO_WAIT);
	EXPECT_EQ(tgm_iso_verdict(1, 4400 * kMs, 1), TGM_ISO_WAIT);
	EXPECT_EQ(tgm_iso_verdict(0, 600 * kMs, 0), TGM_ISO_RETRY);
	EXPECT_EQ(tgm_iso_verdict(0, 1100 * kMs, 1), TGM_ISO_LOST);
	EXPECT_EQ(tgm_iso_verdict(1, 4600 * kMs, 1), TGM_ISO_LOST);
	EXPECT_GT(TGM_ISOLATE_NVKM_NS, 4000 * kMs);
}

// T-v2 kill_one: the poisoned job P stopped GR, which finished P and released its fence before the kill
TEST(Kill, TheFirstFailedSeqnoIsTheJobAGrErrorStoppedIn)
{
	EXPECT_EQ(tgm_kill_first(1, 105731u, 105731u), 105731u);
	EXPECT_EQ(tgm_kill_first(1, 105731u, 105730u), 105731u);
	EXPECT_EQ(tgm_kill_first(0, 0u, 105731u), 105732u)
		<< "a kill with no GR error: the first not complete";
	EXPECT_EQ(tgm_kill_first(1, 0u, 0xffffffffu), 0u) << "across the fence word's wrap";
}

TEST(Unbind, AnIdleEngineOrATsgIdHoldsNoChannel)
{
	EXPECT_FALSE(tgm_engine_holds(engine(1, 17, 0) & ~kBusy, 17));
	EXPECT_FALSE(tgm_engine_holds(engine(0, 17, 0), 17));                // no context
	EXPECT_FALSE(tgm_engine_holds(engine(1, 17, 0) | 0x00001000u, 17));  // TSG 17
	EXPECT_FALSE(tgm_engine_holds(engine(5, 4, 17) | 0x10000000u, 17));  // loading TSG 17
}

TEST(Ctxsw, Boot7sKilledChannelOnGrIsReset)
{
	// iconservicesagent's channel 9, killed after the ENG_RESET; GR still switching its context at every tick
	EXPECT_EQ(tgm_ctxsw_verdict(1, 1, 0, 0, 0), TGM_CTXSW_RESET);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 1, 0, 1, 0), TGM_CTXSW_RESET);
}

TEST(Ctxsw, AContextOfNoChannelIsReset)
{
	EXPECT_EQ(tgm_ctxsw_verdict(0, 0, 0, 0, 0), TGM_CTXSW_RESET);
}

TEST(Ctxsw, ALiveChannelStillDrainingIsWaitedFor)
{
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	const int stuck = tgm_stuck(0, 90, 92, request + kTick, lastDone);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck, 0), TGM_CTXSW_WAIT);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, stuck, 0), TGM_CTXSW_WAIT);
}

TEST(Ctxsw, AStuckClientIsKilledAndAStuckKernelChannelIsRedone)
{
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	const int stuck = tgm_stuck(0, 90, 93, request + 2 * kTick, lastDone);
	ASSERT_TRUE(stuck);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck, 0), TGM_CTXSW_KILL);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, stuck, 0), TGM_CTXSW_REDO);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck, 1), TGM_CTXSW_KILL)
		<< "a client's channel has no redo";
}

// T-v3 251 s: the copy engine could not switch off the present's channel 22, fence 12/13, kicked 717 ms before. The
// engine is reset after the present's job 13 is marked to be redone: the reset lets 13 complete (12 -> 13), and the copy
// redone as 14. Only a timeout on the present again before 14 completes is fatal.
TEST(Ctxsw, TvThreePresentTimeoutResetsAndRedoesThenPanicsOnlyIfTheRedoHangsToo)
{
	const uint32_t completed = 12, submitted = 13;
	const int stuck = tgm_stuck(0, completed, submitted, 1717 * kMs, 1000 * kMs);
	ASSERT_TRUE(stuck);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, stuck, tgm_ctxsw_redo_pending(0, 0, completed)),
		  TGM_CTXSW_REDO);
	const uint32_t resetAfter = submitted;
	EXPECT_TRUE(tgm_ctxsw_redo_pending(1, resetAfter, 12));
	EXPECT_TRUE(tgm_ctxsw_redo_pending(1, resetAfter, 13))
		<< "the interrupted job's own fence is not the redo";
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, 1, tgm_ctxsw_redo_pending(1, resetAfter, 13)),
		  TGM_CTXSW_FATAL);
	EXPECT_FALSE(tgm_ctxsw_redo_pending(1, resetAfter, 14)) << "the redone copy landed";
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, 1, tgm_ctxsw_redo_pending(1, resetAfter, 14)),
		  TGM_CTXSW_REDO);
	EXPECT_FALSE(tgm_ctxsw_redo_pending(1, 0xffffffffu, 0u)) << "across the fence word's wrap";
}

TEST(PbdmaIntr, Boot7sEngResetOnBackboarddLeavesNvkmNothing)
{
	// PBDMA0: 01000000 [] ch 4 [backboardd] subc 1 mthd 0298
	EXPECT_EQ(tgm_pbdma_for_nvkm(0x01000000u), 0u);
}

TEST(PbdmaIntr, ChannelErrorsBesideItStillGoToNvkm)
{
	EXPECT_EQ(tgm_pbdma_for_nvkm(0x01200000u), 0x00200000u);  // METHOD
	EXPECT_EQ(tgm_pbdma_for_nvkm(0x00040000u), 0x00040000u);  // PBENTRY
}

// nvkm's gf100_runq_intr counts ACQUIRE among the channel-fatal bits (0xc67fe000) and writes a NOP over METHOD0: the
// waiter would be killed for waiting and its acquire skipped.
TEST(PbdmaIntr, AnAcquireTimeoutNeverReachesNvkm)
{
	EXPECT_NE(0xc67fe000u & TGM_PBDMA_INTR_ACQUIRE, 0u);
	EXPECT_EQ(tgm_pbdma_for_nvkm(TGM_PBDMA_INTR_ACQUIRE), 0u);
	EXPECT_EQ(tgm_pbdma_for_nvkm(TGM_PBDMA_INTR_ACQUIRE | 0x00200000u), 0x00200000u);
}

// F-v1, gpu.x1.twod_clear: "SwitchHWTest ch 31 err 0 fence 92920/92921 kicked 517 ms ago, gp 2574/2574 (put 2574),
// PGRAPH 03200083 ENG0 801fc01f (ch 31 -> ch 31, ctxsw save), PBDMA0 001f001f ch 31 mthd 8000001d data 00000002": GR
// busy with the 2D fill and unable to save the context, the PBDMA parked at the job's closing SEMAPHORED release (WFI).
TEST(Ctxsw, FV1sTwodClearSaveTimeoutKillsOnlyTheClient)
{
	constexpr uint32_t eng0 = 0x801fc01fu;
	EXPECT_EQ(PFIFO_ENG_CTX_STATUS(eng0), 6u);
	EXPECT_TRUE(tgm_engine_holds(eng0, 31));
	EXPECT_FALSE(tgm_engine_holds(eng0, 4));
	EXPECT_FALSE(tgm_pbdma_acquiring(
		0x8000001du, 0x00000002u));  // a release waiting for idle, not an acquire
	const int stuck = tgm_stuck(0, 92920, 92921, 517 * kMs, 0);
	ASSERT_TRUE(stuck);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck, 0), TGM_CTXSW_KILL);
}

TEST(Ctxsw, TheEngineLineNamesGrFecsTheTrappedClassAndThePbdma)
{
	tgm_engine_regs r = {};
	r.mailbox0 = 0x1;
	r.current_ctx = 0x8001234au;
	r.trapped_addr = 0x80030604u;  // subchannel 3, RENDER_SOLID_PRIM_POINT_SET_Y(0)
	r.trapped_class = 0x902du;
	r.gp_get = 2574;
	r.gp_put = 2574;
	r.get = 0x00120000u;
	r.put = 0x00120040u;
	char line[TGM_HANG_LINE];
	tgm_engine_format(line, sizeof(line), &r);
	const std::string text = line;
	EXPECT_NE(text.find("FECS mailbox0 00000001"), std::string::npos) << text;
	EXPECT_NE(text.find("ctx 8001234a"), std::string::npos) << text;
	EXPECT_NE(text.find("subc 3 mthd 0604 class 902d"), std::string::npos) << text;
	EXPECT_NE(text.find("gp 2574/2574 pb 0000120000/0000120040"), std::string::npos) << text;
	// beside the channel's own line (F-v1's was 330 characters), within os_log's 1,024
	EXPECT_LT(text.size(), 400u);
}

// gpu_hang.c's checks of a channel whose job's internal fences move its fence word with no interrupt: its progress time
// moves when the interrupt pass or a check sees the word move (chan_progress_ns), and a check blames it as
// tgm_ctxsw_decide and tgm_hang_recover do (stuck, confirmed by the next check).
struct SilentJob {
	uint32_t completed, submitted, irq_done, seen;
	uint64_t progress_ns;
	uint32_t strikes = 0, confirmed = 0;

	bool check(uint64_t now)
	{
		if (tgm_progress_seen(completed, irq_done, &seen))
			progress_ns = now;
		const int stuck = tgm_stuck(0, completed, submitted, now, progress_ns);

		return tgm_hang_confirm(stuck, completed, &strikes, &confirmed) != 0;
	}
};

TEST(SilentFences, ALongJobWhoseBatchesKeepEndingIsNeverBlamed)
{
	// 3 s of 27 batches, one ending every 110 ms, checked at every context-switch timeout tick
	SilentJob job = {100, 128, 100, 100, 0};
	for (uint64_t now = kTick; now < 3000 * kMs; now += kTick) {
		job.completed = 100 + uint32_t(now / (110 * kMs));
		EXPECT_FALSE(job.check(now)) << now / kMs;
	}
}

// The batch end a check finds counts from that check, not from when the GPU released it: a stop just after a silent
// fence is blamed up to one check period later than one after an interrupt.
TEST(SilentFences, AJobStoppedInABatchIsBlamedOnceTwoChecksFindItStill)
{
	SilentJob job = {105, 128, 100, 100, 0};
	EXPECT_FALSE(job.check(1 * kTick));
	EXPECT_EQ(job.progress_ns, kTick);
	EXPECT_FALSE(job.check(2 * kTick));
	EXPECT_FALSE(job.check(3 * kTick));
	EXPECT_TRUE(job.check(4 * kTick));
}

TEST(SilentFences, AMoveTheInterruptPassCountedKeepsItsTime)
{
	SilentJob job = {110, 128, 110, 100, 10 * kMs};
	EXPECT_FALSE(job.check(400 * kMs));
	EXPECT_EQ(job.progress_ns, 10 * kMs);
	EXPECT_FALSE(job.check(600 * kMs));
	EXPECT_TRUE(job.check(856 * kMs));
}

TEST(SilentFences, AnIdleChannelIsNeverStuck)
{
	SilentJob job = {128, 128, 120, 120, 0};
	EXPECT_FALSE(job.check(10 * kTick));
	EXPECT_FALSE(job.check(20 * kTick));
}
}  // namespace
