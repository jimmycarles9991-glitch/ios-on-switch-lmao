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
std::vector<std::string> calls;
uint32_t fence, firstFailure;
bool recorded;

class GrFault : public testing::Test
{
protected:
	nvkm_device device{0x10001};
	nvkm_runl run{};
	nvkm_cgrp group{&run, false};
	nvkm_chan channel{&group, false, 0};
	bool held = false;

	void SetUp() override
	{
		calls.clear();
		fence = 7;
		firstFailure = 0;
		recorded = false;
	}

	void acknowledge()
	{
		calls.emplace_back("ack");
		if (device.control & 0x10000)
			fence = 8;
	}
};
}

extern "C" {
u32 switch_gr_fault_mask(nvkm_device *device, u32 address, u32 mask, u32 value)
{
	EXPECT_EQ(address, 0x400500u);
	const u32 before = device->control;
	device->control = (before & ~mask) | value;
	calls.emplace_back("hold");
	return before;
}

void switch_gr_fault_write(nvkm_device *device, u32 address, u32 value)
{
	EXPECT_EQ(address, 0x400500u);
	device->control = value;
	calls.emplace_back(value ? "enable" : "held");
}

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

void nvkm_chan_error(nvkm_chan *channel, bool preempt)
{
	EXPECT_FALSE(preempt);
	channel->stopped = true;
	channel->errored = 1;
	calls.emplace_back("stop");
}

void nvkm_runl_rc_cgrp(nvkm_cgrp *group)
{
	if (!group->pending) {
		group->pending = true;
		++group->runl->rc_pending;
	}
	calls.emplace_back("recover");
}
}

TEST_F(GrFault, FatalAckCannotNaturallyReleaseTheFaultedFence)
{
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	acknowledge();
	switch_gpu_gr_fault_access(&device, held);
	EXPECT_EQ(calls,
		  (std::vector<std::string>{"hold", "record", "stop", "recover", "ack", "held"}));
	EXPECT_TRUE(held);
	EXPECT_TRUE(channel.stopped);
	EXPECT_EQ(run.rc_pending, 1);
	EXPECT_EQ(firstFailure, 8u);
	EXPECT_EQ(fence, 7u);
}

TEST_F(GrFault, ReinitializationKeepsTheExceptionalHold)
{
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	device.control = 0;
	switch_gpu_gr_fault_access(&device, held);
	acknowledge();
	EXPECT_EQ(device.control, 0u);
	EXPECT_EQ(fence, 7u);
}

TEST_F(GrFault, ANewerQueuedRecoveryKeepsAccessDisabled)
{
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	switch_gpu_gr_fault_release(&device, &held, &run.rc_pending);
	EXPECT_TRUE(held);
	EXPECT_EQ(device.control, 0u);
	run.rc_pending = 0;
	switch_gpu_gr_fault_release(&device, &held, &run.rc_pending);
	EXPECT_FALSE(held);
	EXPECT_EQ(device.control, 0x10001u);
	const auto completed = calls;
	switch_gpu_gr_fault_release(&device, &held, &run.rc_pending);
	EXPECT_EQ(calls, completed);
}

TEST_F(GrFault, RepeatedFaultsKeepTheFirstFailedSequence)
{
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	EXPECT_EQ(firstFailure, 8u);
	EXPECT_EQ(run.rc_pending, 1);
}

TEST_F(GrFault, AnUnreferenceableChannelDoesNotAcquireAnUnownedHold)
{
	switch_gpu_gr_fault_stop(&device, &held, nullptr, -1);
	EXPECT_FALSE(held);
	EXPECT_EQ(run.rc_pending, 0);
	EXPECT_EQ(device.control, 0x10001u);
	EXPECT_TRUE(calls.empty());
}

TEST_F(GrFault, AQuiescedChannelsLateInterruptDoesNotAcquireAnotherHold)
{
	channel.errored = 2;
	switch_gpu_gr_fault_stop(&device, &held, &channel, 3);
	EXPECT_FALSE(held);
	EXPECT_EQ(run.rc_pending, 0);
	EXPECT_EQ(device.control, 0x10001u);
	EXPECT_EQ(calls, (std::vector<std::string>{"record"}));
}
