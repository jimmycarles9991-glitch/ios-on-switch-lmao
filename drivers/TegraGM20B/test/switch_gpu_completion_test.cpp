// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstddef>
#include <gtest/gtest.h>
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;

struct tgm_chan {
	uint32_t completed = 0, submitted = 0, progress_done = 0, high = 0, chid = 1, kernel = 0,
		 event_through = 0;
	uint64_t progress_held = 0, progress_ns = 0;
	void (*fenced)(void *) = nullptr;
	void *fenced_arg = nullptr;
};

#define container_of(pointer, type, member) \
	reinterpret_cast<type *>(reinterpret_cast<char *>(pointer) - offsetof(type, member))
#define WRITE_ONCE(target, value) ((target) = (value))
#define READ_ONCE(target) (target)
constexpr int CHAN_SCRUB = 2;
unsigned highCalls, idleCalls, statsCalls, presentCalls, scrubCalls, wakeCalls, channelWakes,
	watchCalls, callbacks, timestamps, eventPasses;
int tgm_sc7_gate;
bool diagnostic, suspended;

uint32_t tgm_chan_completed(tgm_chan *channel)
{
	return channel->completed;
}

void high_fenced(tgm_chan *, uint32_t, uint32_t)
{
	++highCalls;
}

uint64_t nvxnu_nanotime()
{
	++timestamps;
	return 42;
}

uint64_t tgm_hold_held_ns(uint32_t, uint64_t)
{
	return 7;
}

void idle_fenced(tgm_chan *)
{
	++idleCalls;
}

void scrub_fenced(tgm_chan *)
{
	++scrubCalls;
}

bool tgm_gate_is_shut(int *)
{
	return suspended;
}

void nvxnu_wake()
{
	++wakeCalls;
}

bool stats_on()
{
	return diagnostic;
}

void stats_fences(tgm_chan *)
{
	++statsCalls;
}

void present_fenced(tgm_chan *)
{
	++presentCalls;
}

void chan_wake_waiters(tgm_chan *)
{
	++channelWakes;
}

void present_watch_fenced(tgm_chan *, int)
{
	++watchCalls;
}

#include "drivers/TegraGM20B/SwitchGpuBusy.h"
#include "drivers/TegraGM20B/SwitchGpuChanTable.h"
switch_gpu_busy tgm_busy;
switch_gpu_chan_table tgm_chan_table;
#include "drivers/TegraGM20B/SwitchGpuChannelCompletion.h"

void switch_gpu_events_completed()
{
	++eventPasses;
}

class Completion : public testing::Test
{
protected:
	tgm_chan channel;

	void SetUp() override
	{
		highCalls = idleCalls = statsCalls = presentCalls = scrubCalls = wakeCalls =
			channelWakes = watchCalls = callbacks = timestamps = eventPasses = 0;
		diagnostic = suspended = false;
		channel.fenced = [](void *) {
			++callbacks;
		};
	}

	void interrupt()
	{
		chan_completion(&channel);
	}
};

TEST_F(Completion, UnrelatedInterruptDoesNotWakeOrScheduleAnyChannelWork)
{
	diagnostic = true;
	channel.high = 1;
	interrupt();
	interrupt();
	EXPECT_EQ(idleCalls + highCalls + statsCalls + callbacks + wakeCalls + channelWakes +
			  timestamps,
		  0u);
}

TEST_F(Completion, MovingFenceSchedulesAndWakesOnce)
{
	channel.completed = 3;
	interrupt();
	interrupt();
	EXPECT_EQ(channel.progress_done, 3u);
	EXPECT_EQ(callbacks, 1u);
	EXPECT_EQ(channelWakes, 1u);
	EXPECT_EQ(wakeCalls, 0u);
	EXPECT_EQ(idleCalls, 0u)
		<< "the idle clock reads tgm_busy: the pass adds to no counter of its own";
	EXPECT_EQ(timestamps, 1u);
	EXPECT_EQ(channel.progress_ns, 42u);
}

TEST_F(Completion, LaterCompletionStillNotifiesAfterQuietInterrupts)
{
	channel.completed = 1;
	interrupt();
	interrupt();
	channel.completed = 5;
	interrupt();
	EXPECT_EQ(callbacks, 2u);
	EXPECT_EQ(channel.progress_done, 5u);
}

TEST_F(Completion, SequenceWrapIsStillACompletionTransition)
{
	channel.progress_done = UINT32_MAX;
	channel.completed = 0;
	interrupt();
	EXPECT_EQ(callbacks, 1u);
	EXPECT_EQ(channelWakes, 1u);
	EXPECT_EQ(watchCalls, 1u);
}

TEST_F(Completion, PresentAndHighPolicyRunOnlyForItsOwnProgress)
{
	channel.kernel = 1;
	channel.high = 1;
	diagnostic = true;
	interrupt();
	channel.completed = 2;
	interrupt();
	interrupt();
	EXPECT_EQ(presentCalls, 1u);
	EXPECT_EQ(highCalls, 1u);
	EXPECT_EQ(statsCalls, 1u);
	EXPECT_EQ(callbacks, 1u);
	EXPECT_EQ(wakeCalls, 0u);
	EXPECT_EQ(channelWakes, 1u) << "its own room and teardown waits";
	EXPECT_EQ(watchCalls, 0u);  // a kernel channel's fence is no user producer
}

TEST_F(Completion, ScrubProgressWakesSuspendDrainWithoutGenericCallbacks)
{
	channel.kernel = CHAN_SCRUB;
	suspended = true;
	interrupt();
	channel.completed = 1;
	interrupt();
	interrupt();
	EXPECT_EQ(scrubCalls, 1u);
	EXPECT_EQ(wakeCalls, 1u);
	EXPECT_EQ(channelWakes, 1u);
	EXPECT_EQ(callbacks, 0u);
}

TEST_F(Completion, MidJobEventReleaseRunsEventRetirementWithoutFenceProgress)
{
	channel.completed = 3;
	interrupt();
	EXPECT_EQ(eventPasses, 0u);
	channel.event_through = 5;
	interrupt();
	EXPECT_EQ(eventPasses, 1u);
	EXPECT_EQ(callbacks, 1u);
	channel.completed = 5;
	interrupt();
	EXPECT_EQ(callbacks, 2u);
	interrupt();
	EXPECT_EQ(eventPasses, 1u);
}

// The pass publishes how far each channel's fence got (tgm_busy's done word, written by the pass alone): one short of the
// newest seqno kicked is work in flight, the newest none.
TEST_F(Completion, ThePassPublishesTheChannelsDoneSeqno)
{
	tgm_busy = {};
	switch_gpu_busy_level(&tgm_busy, channel.chid, 1);
	switch_gpu_busy_kick(&tgm_busy, channel.chid, 5);
	channel.completed = 4;
	interrupt();
	EXPECT_TRUE(switch_gpu_busy_chan(&tgm_busy, channel.chid));
	EXPECT_TRUE(switch_gpu_busy_low(&tgm_busy));
	channel.completed = 5;
	interrupt();
	EXPECT_FALSE(switch_gpu_busy_chan(&tgm_busy, channel.chid));
	EXPECT_FALSE(switch_gpu_busy_low(&tgm_busy));
}

// The pass completes the live channels with a kick it has not seen done, from their tgm_chan_table slots: no per-job
// event to arm, allow, complete or drop. A channel with nothing pending, or no slot, is not touched.
TEST_F(Completion, ThePassCompletesOnlyChannelsWithWorkPending)
{
	tgm_busy = {};
	tgm_chan_table = {};
	switch_gpu_busy_open(&tgm_busy, channel.chid, 0);
	switch_gpu_chan_table_set(&tgm_chan_table, channel.chid, &channel);
	EXPECT_EQ(chans_completed(), 0u);
	EXPECT_EQ(timestamps, 0u) << "nothing kicked";
	switch_gpu_busy_kick(&tgm_busy, channel.chid, 2);
	channel.completed = 2;
	EXPECT_EQ(chans_completed(), 1u) << "one channel progressed: a job interrupt";
	EXPECT_EQ(channel.progress_done, 2u);
	EXPECT_EQ(callbacks, 1u);
	EXPECT_EQ(chans_completed(), 0u);
	EXPECT_EQ(callbacks, 1u) << "seen done: not touched again";
	switch_gpu_busy_kick(&tgm_busy, channel.chid, 3);
	switch_gpu_chan_table_set(&tgm_chan_table, channel.chid, nullptr);
	EXPECT_EQ(chans_completed(), 0u);
	EXPECT_EQ(callbacks, 1u) << "torn down";
}
