// SPDX-License-Identifier: MIT
#include <array>
#include <gtest/gtest.h>
#include <nvif/chan.h>

unsigned switch_gpu_test_delays, switch_gpu_test_sleeps, switch_gpu_test_warnings;
unsigned long switch_gpu_test_sleep_us;
int switch_gpu_test_can_sleep = 1;

namespace
{
struct Model {
	std::array<u32, 1024> push = {};
	std::array<u32, 2048> fifo = {};
	nvif_chan channel = {};
	u32 completedPush = 768, completedFifo = 0;
	unsigned publications = 0, entries = 0;
	unsigned stallReads = 0;
};

Model &model(nvif_chan *channel)
{
	return *static_cast<Model *>(channel->userd.map.ptr);
}

u32 pushGet(nvif_chan *channel)
{
	return model(channel).completedPush;
}

u32 fifoGet(nvif_chan *channel)
{
	auto &state = model(channel);
	if (state.stallReads) {
		if (--state.stallReads == 0)
			state.completedFifo = channel->gpfifo.cur;
	}
	return state.completedFifo;
}

void pushEntry(nvif_chan *channel, bool, u64, u32, bool)
{
	ASSERT_GT(channel->gpfifo.free, 0);
	channel->gpfifo.cur = (channel->gpfifo.cur + 1) & channel->gpfifo.max;
	--channel->gpfifo.free;
	++model(channel).entries;
}

void publish(nvif_chan *channel)
{
	auto &state = model(channel);
	++state.publications;
	state.completedPush = static_cast<u32>(channel->push.cur - state.push.data());
	state.completedFifo = channel->gpfifo.cur;
}

int post(nvif_chan *channel, u32, u32)
{
	if (channel->push.end - channel->push.cur < 5)
		return -EINVAL;
	channel->push.cur += 5;
	return 0;
}

const nvif_chan_func operations = {{pushGet}, {fifoGet, pushEntry, publish, post, 5}};

void runSubmission(Model &state)
{
	auto &channel = state.channel;
	channel.defer_kick = true;
	ASSERT_EQ(nvif_chan_gpfifo_wait(&channel, 8, 256), 0);
	*channel.push.cur++ = 1;
	PUSH_KICK(&channel.push);
	const unsigned before = state.publications;
	nvif_chan_gpfifo_push(&channel, 0x200000, 128, false);
	nvif_chan_gpfifo_push(&channel, 0x300000, 64, true);
	EXPECT_EQ(state.publications, before);
	*channel.push.cur++ = 2;
	channel.defer_kick = false;
	PUSH_KICK(&channel.push);
	EXPECT_EQ(state.publications, before + 1);
}

void initialize(Model &state, u32 cursor)
{
	switch_gpu_test_delays = switch_gpu_test_sleeps = switch_gpu_test_warnings = 0;
	switch_gpu_test_sleep_us = 0;
	switch_gpu_test_can_sleep = 1;
	nvif_chan_gpfifo_ctor(&operations, &state, state.fifo.data(), sizeof(state.fifo),
			      state.push.data(), 0x100000, sizeof(state.push), &state.channel);
	state.channel.push.bgn = state.channel.push.cur = state.channel.push.end =
		state.push.data() + cursor;
}
}  // namespace

TEST(Gpfifo, WrappedKernelPacketsAndUserEntriesPublishTogether)
{
	Model state;
	initialize(state, 900);
	runSubmission(state);
	EXPECT_EQ(state.entries, 4u);
	EXPECT_EQ(state.publications, 1u);
	EXPECT_EQ(state.channel.push.cur, state.push.data() + 12);
	EXPECT_EQ(switch_gpu_test_delays, 0u);
	EXPECT_EQ(switch_gpu_test_sleeps, 0u);
	EXPECT_EQ(switch_gpu_test_warnings, 0u);
}

TEST(Gpfifo, RepeatedRingWrapKeepsOnePublicationPerSubmission)
{
	Model state;
	initialize(state, 0);
	for (unsigned i = 0; i < 2048; ++i)
		ASSERT_NO_FATAL_FAILURE(runSubmission(state));
	EXPECT_EQ(state.entries, 8192u);
	EXPECT_EQ(state.publications, 2048u);
	EXPECT_EQ(switch_gpu_test_delays, 0u);
	EXPECT_EQ(switch_gpu_test_sleeps, 0u);
	EXPECT_EQ(switch_gpu_test_warnings, 0u);
}

TEST(Gpfifo, WaitSpinsBrieflyThenSleepsInSleepableContext)
{
	Model state;
	initialize(state, 0);
	state.channel.gpfifo.cur = 10;
	state.channel.gpfifo.free = 0;
	state.completedFifo = 11;
	state.stallReads = 20;
	EXPECT_EQ(nvif_chan_gpfifo_wait(&state.channel, 4, 0), 0);
	EXPECT_EQ(switch_gpu_test_delays, 16u);
	EXPECT_EQ(switch_gpu_test_sleeps, 3u);
	EXPECT_GT(switch_gpu_test_sleep_us, 0u);
}

TEST(Gpfifo, WaitTimesOutWithBoundedSleepsInsteadOfBusySpin)
{
	Model state;
	initialize(state, 0);
	state.channel.gpfifo.cur = 10;
	state.channel.gpfifo.free = 0;
	state.completedFifo = 11;
	EXPECT_EQ(nvif_chan_gpfifo_wait(&state.channel, 4, 0), -ETIMEDOUT);
	EXPECT_EQ(switch_gpu_test_delays, 16u);
	EXPECT_GT(switch_gpu_test_sleeps, 100u);
	EXPECT_LT(switch_gpu_test_sleeps, 2000u);
	EXPECT_GE(switch_gpu_test_sleep_us, 999000u);
}

TEST(Gpfifo, WaitNeverSleepsInNonSleepableContext)
{
	Model state;
	initialize(state, 0);
	switch_gpu_test_can_sleep = 0;
	state.channel.gpfifo.cur = 10;
	state.channel.gpfifo.free = 0;
	state.completedFifo = 11;
	EXPECT_EQ(nvif_chan_gpfifo_wait(&state.channel, 4, 0), -ETIMEDOUT);
	EXPECT_EQ(switch_gpu_test_sleeps, 0u);
	EXPECT_GT(switch_gpu_test_delays, 16u);
}
