// SPDX-License-Identifier: MIT
// gpu_chan.c's channel creation on the host (switch_gpu_chan_new_driver.c): a new channel's first RAMFC words, its
// timeslice (the compositor's 4x one for a critical channel) and its acquire timeout, hold once the host has run a job
// on it, as boot 22's gpu.ramfc_timeslice found they did not (4 of 22 client channels and the scrubber's read the
// default timeslice where the high one had been written at their creation).
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/test/exec/switch_gpu_chan_new_harness.h"

namespace
{
constexpr uint32_t kDefault = 0x10003080, kHigh = 0x10005080;

struct Made {
	switch_chan_new_ramfc memory;
	uint32_t recorded;
};

Made makeAndRun(uint32_t chid, int critical)
{
	EXPECT_EQ(switch_chan_new_make(chid, critical), 0);
	switch_chan_new_job();
	const Made made = {switch_chan_new_host()->memory, switch_chan_new_recorded_timeslice()};
	switch_chan_new_delete();
	return made;
}

TEST(GpuChanNew, ACriticalChannelKeepsItsHighTimesliceThroughItsFirstJobs)
{
	const Made made = makeAndRun(2, 1);
	EXPECT_EQ(made.recorded, kHigh);
	EXPECT_EQ(made.memory.timeslice, kHigh) << "the host's save of the RAMFC it loaded at the "
						   "runlist insert landed on the creation's write";
	EXPECT_EQ(switch_chan_new_host()->quiesced_waited, 1u)
		<< "creation is a setup path: it waits for the runlist lock rather than leave the "
		   "words";
}

TEST(GpuChanNew, EveryChannelKeepsItsAcquireTimeout)
{
	for (const int critical : {0, 1}) {
		const Made made = makeAndRun(3, critical);
		EXPECT_EQ(made.memory.acquire, SWITCH_CHAN_NEW_ACQUIRE) << "critical " << critical;
	}
}

TEST(GpuChanNew, AnOrdinaryChannelKeepsTheDefaultTimeslice)
{
	const Made made = makeAndRun(4, 0);
	EXPECT_EQ(made.recorded, kDefault);
	EXPECT_EQ(made.memory.timeslice, kDefault);
}
}  // namespace
