// SPDX-License-Identifier: MIT
// The present deadline of a watched producer that finished (gpu_present_deadline.c, over switch_gpu_present_model.h):
// it follows the work the asked interrupt is queued behind, waits for an owed wake entry, gives a due interrupt a wake's
// grace and then panics. It never delivers the watch itself.
#include <gtest/gtest.h>
#include "switch_gpu_present_model.h"

using present_model::Channel;
using present_model::Deadline;
using present_model::Display;
using present_model::kA;
using present_model::kWakeGraceNs;
using present_model::Producer;

// The asked wake entry ran and its interrupt never came: the deadline gives the pass a wake's grace, then panics.
TEST(PresentDeadline, ALostAskedInterruptPanics)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	ASSERT_EQ(d.offer(&token, 1), 0);
	d.ownerWakes();
	ASSERT_EQ(a.channel.wakeEntries, 1u);
	a.channel.run(~size_t(0), false);
	EXPECT_EQ(d.deadline(1000), Deadline::Grace);
	EXPECT_EQ(d.deadline(1000 + kWakeGraceNs - 1), Deadline::Grace);
	EXPECT_EQ(d.deadline(1000 + kWakeGraceNs), Deadline::Panic);
	EXPECT_EQ(d.presented, 0u);
	EXPECT_EQ(d.workItems, 0u) << "nothing is taken silently";
}

// A producer whose own job interrupts and whose interrupt was lost panics the same way.
TEST(PresentDeadline, ALostInterruptOfAnInterruptingProducerPanics)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, false));
	ASSERT_EQ(d.offer(&token, 1), 0);
	a.channel.run(~size_t(0), false);
	EXPECT_EQ(d.deadline(1), Deadline::Grace);
	EXPECT_EQ(d.deadline(1 + kWakeGraceNs), Deadline::Panic);
	EXPECT_EQ(d.presented, 0u);
}

// The wake entry behind a long job: past the producer and short of the long job's end, the deadline follows the channel
// to the wake's position, and the frame is delivered when the entry runs.
TEST(PresentDeadline, AWakeQueuedBehindALongJobIsWaitedFor)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	const uint32_t longEnd = a.channel.submit(30, true);
	ASSERT_EQ(d.offer(&token, 1), 0);
	d.ownerWakes();
	a.channel.run(1);
	EXPECT_EQ(a.channel.fence, 41u);
	EXPECT_EQ(d.deadline(0), Deadline::Follows);
	EXPECT_EQ(d.followed, longEnd);
	EXPECT_EQ(d.deadline(10 * kWakeGraceNs), Deadline::Follows)
		<< "no grace while the GPU is short of it";
	EXPECT_EQ(d.presented, 0u);
	a.channel.run();
	EXPECT_EQ(a.channel.interrupts, 1u);
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.deadline(20 * kWakeGraceNs), Deadline::Idle);
}

// Later jobs overwrote the record the wake entry marked: the newest announced seqno stands for its position.
TEST(PresentDeadline, AnOverwrittenRecordWaitsForTheNewestAnnouncedSeqno)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	const uint32_t longEnd = a.channel.submit(5, true);
	ASSERT_EQ(d.offer(&token, 1), 0);
	d.ownerWakes();
	for (int i = 0; i < 9; ++i)
		a.channel.submit(1, true);
	a.channel.run(1);
	EXPECT_EQ(d.deadline(0), Deadline::Follows);
	EXPECT_EQ(d.followed, longEnd);
	a.channel.run();
	EXPECT_EQ(d.presented, 1u);
}

// The GPU just passed the fence the entry follows, and the pass is under way: the grace covers it.
TEST(PresentDeadline, AnInterruptUnderWayIsGivenItsGrace)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	ASSERT_EQ(d.offer(&token, 1), 0);
	d.ownerWakes();
	a.channel.run(1);
	EXPECT_EQ(d.deadline(1), Deadline::Grace);
	a.channel.run();
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.deadline(1 + kWakeGraceNs), Deadline::Idle);
}

// Asked, and the owner has not pushed the entry yet (its submission lock is held): the deadline waits for the owner.
TEST(PresentDeadline, AnOwedWakeEntryIsWaitedFor)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	ASSERT_EQ(d.offer(&token, 1), 0);
	a.channel.run();
	EXPECT_EQ(d.deadline(0), Deadline::Rearmed);
	EXPECT_EQ(d.deadline(10 * kWakeGraceNs), Deadline::Rearmed);
	EXPECT_EQ(d.presented, 0u);
	d.ownerWakes();
	a.channel.run();
	EXPECT_EQ(d.presented, 1u);
}

// A producer still running is the pending producer's path, as before.
TEST(PresentDeadline, ARunningProducerIsThePendingProducersPath)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	ASSERT_EQ(d.offer(&token, 1), 0);
	EXPECT_EQ(d.deadline(0), Deadline::Producer);
}

// The channel torn down after its fence passed, with the asked entry's pass not taken: the teardown delivers the watch.
TEST(PresentDeadline, ATeardownDeliversWhatItsPassWouldHave)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	ASSERT_EQ(d.offer(&token, 1), 0);
	d.ownerWakes();
	a.channel.run(1);
	d.teardown(a);
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.workItems, 1u);
	EXPECT_EQ(d.deadline(0), Deadline::Idle);
}
