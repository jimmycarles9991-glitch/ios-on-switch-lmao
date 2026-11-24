// SPDX-License-Identifier: MIT
// A direct present of a frame behind a writer promise (switch_gpu_present_model.h): no present job, the frame flipped
// from the interrupt pass of the job that keeps the promise; and the frame dropped, never shown unwritten, when the
// promise is abandoned.
#include <gtest/gtest.h>
#include "switch_gpu_present_model.h"

using present_model::Channel;
using present_model::Deadline;
using present_model::Display;
using present_model::kA;
using present_model::Producer;

namespace
{
constexpr uint32_t kWord = 2052, kValue = 9;
constexpr switch_gpu_backing_token kPromise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kWord};

// frames delivered with their writes; a failed delivery is TEGRA_SCANOUT_SOURCE_LOST, dropped
unsigned shown(const Display &d)
{
	return d.presented - d.failed;
}
}  // namespace

// The swap came before the job: the watch waits for the promise, the job's submission hands it the job, and the job's
// own interrupt (its fence push releases the word after its fence) presents the frame. No present job ran.
TEST(PresentPromise, APromisedFrameIsPresentedByTheInterruptOfTheJobThatKeepsIt)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	EXPECT_EQ(switch_gpu_present_watch_state(&d.watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_PROMISED));
	a.channel.run();
	EXPECT_EQ(d.presented, 0u);
	const auto job = a.token(a.channel.submit(1, true));
	d.kept(kWord, kValue, job);
	EXPECT_TRUE(a.channel.asked) << "a quiet job is asked for the interrupt";
	d.ownerWakes();
	a.channel.run();
	EXPECT_EQ(shown(d), 1u);
	EXPECT_EQ(d.jobInterrupts, 0u);
	EXPECT_EQ(d.workItems, 0u) << "the pass's end delivers it";
}

// The job was submitted between the offer's snapshot and its arm: the offer finds it among the promises kept lately.
TEST(PresentPromise, AJobSubmittedBeforeTheArmIsWatchedFromTheStart)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto job = a.token(a.channel.submit(1, false));
	d.kept(kWord, kValue, job);
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	EXPECT_EQ(switch_gpu_present_watch_state(&d.watch),
		  uint32_t(SWITCH_GPU_PRESENT_WATCH_ARMED));
	a.channel.run();
	EXPECT_EQ(shown(d), 1u);
}

// A job submitted and finished before the conversion: its pass found the watch PROMISED, so the conversion's own look
// hands it to the work item.
TEST(PresentPromise, AJobThatEndedBeforeItsConversionIsTakenByTheLastLook)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	const auto job = a.token(a.channel.submit(1, false));
	a.channel.run();
	EXPECT_EQ(d.presented, 0u);
	d.kept(kWord, kValue, job);
	EXPECT_EQ(shown(d), 1u);
	EXPECT_EQ(d.workItems, 1u);
}

// No job keeps the promise before its bound: the deadline has nothing to do with it, nor the give-up's pass; its maker
// withdraws it (the held command buffer failed at Metal's bound), and the frame is dropped (the panel keeps the one
// before it); it is never shown without its writes.
TEST(PresentPromise, AnUnkeptPromiseWithdrawnDropsTheFrame)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	EXPECT_EQ(d.deadlineAt, 0u) << "no deadline while the promise has no job";
	EXPECT_EQ(d.deadline(1000000000), Deadline::Idle);
	d.giveUp();
	EXPECT_EQ(d.presented, 0u);
	d.cpuRelease(kWord, kValue, true);
	EXPECT_EQ(d.failed, 1u);
	EXPECT_EQ(shown(d), 0u);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&d.watch));
	const auto late = a.token(a.channel.submit(1, false));
	d.kept(kWord, kValue, late);
	a.channel.run();
	EXPECT_EQ(d.presented, 1u) << "the late job delivers nothing more";
}

// Its holder stopped (an app suspended mid-hold, or before its submit worker armed anything): the kernel gives the
// promise up at its bound, which reports its writer once and drops the frame; a job keeping it later delivers nothing
// more.
TEST(PresentPromise, AnUnkeptPromisePastItsBoundDropsTheFrame)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.atBound();
	EXPECT_EQ(d.reported, 1u);
	EXPECT_EQ(d.failed, 1u);
	EXPECT_EQ(shown(d), 0u);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&d.watch));
	const auto late = a.token(a.channel.submit(1, false));
	d.kept(kWord, kValue, late);
	a.channel.run();
	EXPECT_EQ(d.presented, 1u) << "the late job delivers nothing more";
}

// A cancel gives the promise back with nothing written, so the frame is what the surface holds: shown. A kill or a close
// gives it up: dropped.
TEST(PresentPromise, ACancelShowsTheFrameAndAKillDropsIt)
{
	for (const bool abandon : {false, true}) {
		Producer a{kA, Channel(40)};
		Display d({&a});
		ASSERT_EQ(d.offer(&kPromise, 1), 0);
		d.cpuRelease(kWord, kValue, abandon);
		EXPECT_EQ(d.presented, 1u);
		EXPECT_EQ(shown(d), abandon ? 0u : 1u);
	}
}

// A word released by a job the watch never heard of is delivered by the give-up's pass.
TEST(PresentPromise, AWordAlreadyReleasedAtTheGiveUpShowsTheFrame)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.words[kWord] = kValue;
	d.giveUp();
	EXPECT_EQ(shown(d), 1u);
}

// The compositor's frame 2 is promised (its command buffer held on a shared event) and offered for window A's direct
// scanout while frame 1 shows. Its four outcomes, each with no present job.
TEST(PresentPromiseDirect, KeptFlipsFromTheFilterBeforeTheJobsPass)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.filter();
	EXPECT_EQ(d.panel, 1) << "no view while the promise waits for its job";
	const auto job = a.token(a.channel.submit(1, false));
	d.kept(kWord, kValue, job);
	a.channel.run(~size_t(0), false);
	d.filter();
	EXPECT_EQ(d.panel, 2);
	EXPECT_EQ(d.filterFlips, 1u);
	a.channel.pass();
	EXPECT_EQ(shown(d), 1u);
	EXPECT_EQ(d.panel, 2);
	EXPECT_EQ(d.jobInterrupts, 0u);
}

TEST(PresentPromiseDirect, KeptFlipsFromTheJobsInterruptPass)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.kept(kWord, kValue, a.token(a.channel.submit(1, false)));
	EXPECT_EQ(d.deadlineAt, d.clock + Display::kPresentNs) << "the deadline comes with the job";
	a.channel.run();
	EXPECT_EQ(d.panel, 2);
	EXPECT_EQ(d.filterFlips, 0u);
	EXPECT_EQ(d.jobInterrupts, 0u);
}

TEST(PresentPromiseDirect, ACancelShowsItsFrame)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.cpuRelease(kWord, kValue, false);
	EXPECT_EQ(d.panel, 2);
}

// Past its bound with the holder stopped: given up, and the panel keeps frame 1. The holder's job, when it runs later
// (nothing armed it to fail, so its keep is accepted), writes the surface for the frames after: this present flips
// nothing, and the next offer of the surface shows the job's frame.
TEST(PresentPromiseDirect, UnkeptPastItsBoundIsDroppedAndTheLateJobFlipsNothing)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	d.filter();
	d.atBound();
	EXPECT_EQ(d.reported, 1u);
	EXPECT_EQ(d.failed, 1u);
	EXPECT_EQ(d.panel, 1);
	const auto late = a.token(a.channel.submit(1, false));
	d.kept(kWord, kValue, late);
	a.channel.run();
	d.filter();
	EXPECT_EQ(d.panel, 1) << "the late job's write flipped a present already dropped";
	EXPECT_EQ(d.presented, 1u);
}

// Never kept: its maker's withdraw drops it, and the panel keeps frame 1. Once kept, the watch has its deadline from the
// keep.
TEST(PresentPromiseDirect, UnkeptIsDroppedKeepingThePreviousFrame)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	d.clock = 5 * 1000000ull;
	ASSERT_EQ(d.offer(&kPromise, 1), 0);
	EXPECT_EQ(d.deadlineAt, 0u);
	d.filter();
	d.giveUp();
	d.cpuRelease(kWord, kValue, true);
	EXPECT_EQ(d.failed, 1u);
	EXPECT_EQ(d.panel, 1);
}
