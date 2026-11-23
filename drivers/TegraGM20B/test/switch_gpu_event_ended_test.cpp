// SPDX-License-Identifier: MIT
// What another process can no longer do to the compositor's event waits. A producer that failed or whose client died
// ends the waits on it as its completion does: the waiting job is admitted and runs on what the producer left (the
// compositor's job waiting on a killed app's event or write was refused at EventPrepare, EventAppend or EventInherit,
// which panics the device, or failed after its admission, which ends backboardd). And no task can take the event
// records the compositor's frames need (an exhausted pool answered Busy, which panics it).
#include "switch_gpu_event_fixture.h"

namespace
{
constexpr int kGone = -19, kKilled = -5;

void setStatus(const switch_gpu_backing_token &t, int status)
{
	std::lock_guard held(tokensLock);
	tokens[key(t)].execution = tokens[key(t)].external = status;
}

std::vector<switch_gpu_event_waits *> reserveUpTo(int pid, size_t most)
{
	eventPid = pid;
	std::vector<switch_gpu_event_waits *> held;
	for (switch_gpu_event_waits *batch = nullptr;
	     held.size() < most && !switch_gpu_event_waits_prepare(nullptr, 0, &batch);)
		held.push_back(batch);
	return held;
}

void releaseAll(std::vector<switch_gpu_event_waits *> &held)
{
	for (auto *&batch : held)
		switch_gpu_event_waits_abort(&batch);
	held.clear();
	pump();
}
}  // namespace

TEST_F(Events, AWaitOnAProducerWhoseClientDiedIsAdmitted)
{
	uint64_t value = 5;
	for (const int death : {kGone, kKilled}) {
		auto t = token();
		signal(value, t);
		setStatus(t, death);
		switch_gpu_event_use *use = nullptr;
		switch_gpu_backing_token waited{};
		EXPECT_EQ(switch_gpu_event_resolve_use(event, value, &waited, &use), 1)
			<< death << ": before the worker retires the signal";
		EXPECT_EQ(use, nullptr);
		switch_gpu_events_completed();
		pump();
		EXPECT_EQ(switch_gpu_event_resolve_use(event, value, &waited, &use), 1)
			<< death << ": after";
		EXPECT_EQ(use, nullptr);
		++value;
	}
	EXPECT_EQ(native->value, 6u);
}

TEST_F(Events, TheCompositorsJobOnAKilledClientsWriteHasNothingLeftToWaitFor)
{
	auto a = token(), b = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *write = batch(use, b);
	switch_gpu_event_use_release(&use);
	commit(write);
	semantics[key(b)] = write;
	finish(b, kKilled);
	EXPECT_EQ(switch_gpu_event_waits_covers(nullptr, &b), 1);
	switch_gpu_event_waits *inherited = nullptr;
	EXPECT_EQ(switch_gpu_event_waits_prepare_dependencies(&b, 1, &inherited), 0);
	EXPECT_EQ(inherited, nullptr);
	semantics.clear();
	finish(a, kKilled);
	switch_gpu_event_waits_release(&write);
	pump();
}

TEST_F(Events, AnAcceptedWaitersOutcomeSurvivesItsProducersClientDying)
{
	auto a = token(), c = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *wait = batch(use, c);
	switch_gpu_event_use_release(&use);
	int outcome = 99;
	ASSERT_EQ(switch_gpu_event_waits_completion(
			  wait,
			  [](void *context, int status) {
				  *static_cast<int *>(context) = status;
			  },
			  &outcome),
		  0);
	commit(wait);
	finish(c);
	EXPECT_EQ(outcome, 99);
	finish(a, kGone);
	EXPECT_EQ(outcome, 0) << "below 0, switch_gpu_timeline_semantic_finish fails the waiter";
	switch_gpu_event_waits_release(&wait);
	pump();
}

TEST_F(Events, ATaskAtItsQuotaLeavesOtherTasksAndTheCompositorTheirRecords)
{
	auto app = reserveUpTo(101, 10000);
	EXPECT_EQ(app.size(), 512u);
	auto other = reserveUpTo(102, 1);
	EXPECT_EQ(other.size(), 1u);
	ASSERT_EQ(switch_gpu_events_compositor(103, 1), 0);
	auto compositor = reserveUpTo(103, 1);
	EXPECT_EQ(compositor.size(), 1u);
	EXPECT_EQ(switch_gpu_events_compositor(103, 0), 0);
	releaseAll(app);
	releaseAll(other);
	releaseAll(compositor);
	app = reserveUpTo(101, 1);
	EXPECT_EQ(app.size(), 1u) << "a released record goes back to its task";
	releaseAll(app);
}

TEST_F(Events, ThePoolsLastRecordsAreTheCompositors)
{
	std::vector<std::vector<switch_gpu_event_waits *>> apps;
	for (int pid = 201; pid <= 206; ++pid)
		apps.push_back(reserveUpTo(pid, 512));
	auto late = reserveUpTo(207, 1);
	EXPECT_TRUE(late.empty()) << "six tasks hold every record but the compositor's";
	ASSERT_EQ(switch_gpu_events_compositor(208, 1), 0);
	auto compositor = reserveUpTo(208, 2000);
	EXPECT_EQ(compositor.size(), 1024u);
	EXPECT_EQ(switch_gpu_events_compositor(208, 0), 0);
	releaseAll(compositor);
	for (auto &held : apps)
		releaseAll(held);
}

TEST_F(Events, AnEventsObserversLeaveTheCompositorsWatch)
{
	auto &n = makeNotices();
	eventPid = 301;
	switch_gpu_event_watch *w = nullptr;
	for (uint64_t value = 1000; value < 1256; ++value) {
		ASSERT_EQ(switch_gpu_event_observe(event, value, value, notice, &n, &w), 0);
		watches.push_back(w);
	}
	EXPECT_EQ(switch_gpu_event_observe(event, 2000, 0, notice, &n, &w), -16);
	ASSERT_EQ(switch_gpu_events_compositor(302, 1), 0);
	eventPid = 302;
	ASSERT_EQ(switch_gpu_event_observe(event, 2000, 0, notice, &n, &w), 0);
	watches.push_back(w);
	EXPECT_EQ(switch_gpu_events_compositor(302, 0), 0);
}

// Each task has a count of its own: two whose pids are 256 apart once shared one
TEST_F(Events, TwoTasksNeverShareARecordCount)
{
	auto first = reserveUpTo(501, 1000);
	auto second = reserveUpTo(501 + 256, 1000);
	EXPECT_EQ(first.size(), 512u);
	EXPECT_EQ(second.size(), 512u);
	releaseAll(first);
	releaseAll(second);
}

// The VIC's engine thread runs a process's jobs: their records are that process's, not the kernel threads' 512
TEST_F(Events, AKernelThreadsRecordsCountAsTheProcessItWorksFor)
{
	switch_gpu_events_charge(601);
	auto charged = reserveUpTo(0, 1000);
	EXPECT_EQ(charged.size(), 512u) << "the process's quota";
	switch_gpu_events_charge(0);
	auto kernel = reserveUpTo(0, 1);
	EXPECT_EQ(kernel.size(), 1u) << "the kernel threads' own count untouched";
	auto process = reserveUpTo(601, 1);
	EXPECT_TRUE(process.empty()) << "the process's own threads share its count";
	releaseAll(charged);
	releaseAll(kernel);
}
