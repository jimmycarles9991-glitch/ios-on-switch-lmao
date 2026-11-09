// SPDX-License-Identifier: MIT
// TegraGM20BRecover.h's scheduler error names, the decision on an engine whose switch timed out, and which PBDMA
// interrupts nvkm's handler still gets, over device boot 7 (iOS 27): GR reset for iconservicesagent's trapped channel 10
// while PBDMA0 served backboardd's channel 4 (ENG_RESET), then GR stuck switching a killed channel's context, a
// CTXSW_TIMEOUT tick every 256 ms and SCHED_ERROR 0x0d.
#include <cstdint>
#include <string>
#include <gtest/gtest.h>
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
	EXPECT_EQ(tgm_ctxsw_verdict(1, 1, 0, 0), TGM_CTXSW_RESET);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 1, 0, 1), TGM_CTXSW_RESET);
}

TEST(Ctxsw, AContextOfNoChannelIsReset)
{
	EXPECT_EQ(tgm_ctxsw_verdict(0, 0, 0, 0), TGM_CTXSW_RESET);
}

TEST(Ctxsw, ALiveChannelStillDrainingIsWaitedFor)
{
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	const int stuck = tgm_stuck(0, 90, 92, request + kTick, lastDone);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck), TGM_CTXSW_WAIT);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, stuck), TGM_CTXSW_WAIT);
}

TEST(Ctxsw, AStuckClientIsKilledAndAStuckKernelChannelIsFatal)
{
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	const int stuck = tgm_stuck(0, 90, 93, request + 2 * kTick, lastDone);
	ASSERT_TRUE(stuck);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 0, stuck), TGM_CTXSW_KILL);
	EXPECT_EQ(tgm_ctxsw_verdict(1, 0, 1, stuck), TGM_CTXSW_FATAL);
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
}  // namespace
