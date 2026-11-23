// SPDX-License-Identifier: MIT
#include "switch_gpu_gr_fault_fixture.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BGrFault.h"
}
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
constexpr int kRcNone = 0, kRcPending = 1, kRcRunning = 2;

std::vector<std::string> calls;
uint32_t fence, firstFailure;
bool recorded;

class GrFault : public testing::Test
{
protected:
	nvkm_runl run{};
	nvkm_cgrp group{&run, kRcNone};
	nvkm_chan channel{&group, 0};

	void SetUp() override
	{
		calls.clear();
		fence = 7;
		firstFailure = 0;
		recorded = false;
	}
};
}

extern "C" {
void tgm_gr_fault(int chid)
{
	if (chid < 0)
		return;
	calls.emplace_back("record");
	if (!recorded) {
		firstFailure = fence + 1;
		recorded = true;
	}
}

/* chan.c nvkm_chan_error: errored 0 -> 1 once, the channel blocked then */
void nvkm_chan_error(nvkm_chan *channel, bool preempt)
{
	EXPECT_FALSE(preempt)
		<< "PFIFO_PREEMPT is written under runl->mutex, not from the GR interrupt";
	if (channel->errored == 0) {
		channel->errored = 1;
		calls.emplace_back("stop");
	}
}

/* runl.c nvkm_runl_rc_cgrp: NONE -> PENDING once, one recovery scheduled then */
void nvkm_runl_rc_cgrp(nvkm_cgrp *group)
{
	if (group->rc == kRcNone) {
		group->rc = kRcPending;
		++group->runl->rc_pending;
		calls.emplace_back("recover");
	}
}
}

// The fault is charged to the job GR stopped in before the acknowledgement lets GR finish it; the channel and its group
// alone go to recovery, and GR's FIFO access is not this path's (the header takes no device to write it with).
TEST_F(GrFault, TheFaultIsChargedThenItsChannelStoppedAndItsGroupRecovered)
{
	switch_gpu_gr_fault_stop(&channel, 3);
	fence = 8;
	EXPECT_EQ(calls, (std::vector<std::string>{"record", "stop", "recover"}));
	EXPECT_EQ(channel.errored, 1);
	EXPECT_EQ(group.rc, kRcPending);
	EXPECT_EQ(run.rc_pending, 1);
	EXPECT_EQ(firstFailure, 8u);
}

TEST_F(GrFault, RepeatedFaultsKeepTheFirstFailedSequenceAndOneRecovery)
{
	switch_gpu_gr_fault_stop(&channel, 3);
	fence = 8;
	switch_gpu_gr_fault_stop(&channel, 3);
	EXPECT_EQ(firstFailure, 8u);
	EXPECT_EQ(run.rc_pending, 1);
	EXPECT_EQ(calls, (std::vector<std::string>{"record", "stop", "recover", "record"}));
}

TEST_F(GrFault, AnUnreferenceableChannelStopsNothing)
{
	switch_gpu_gr_fault_stop(nullptr, -1);
	EXPECT_EQ(run.rc_pending, 0);
	EXPECT_TRUE(calls.empty());
}

// A late interrupt for a channel nvkm_runl_rc already quiesced (errored 2, its group RUNNING) starts no second
// recovery: both nvkm calls are no-ops then, so the path needs no check of its own.
TEST_F(GrFault, AQuiescedChannelsLateInterruptStartsNoSecondRecovery)
{
	channel.errored = 2;
	group.rc = kRcRunning;
	switch_gpu_gr_fault_stop(&channel, 3);
	EXPECT_EQ(run.rc_pending, 0);
	EXPECT_EQ(channel.errored, 2);
	EXPECT_EQ(calls, (std::vector<std::string>{"record"}));
}
