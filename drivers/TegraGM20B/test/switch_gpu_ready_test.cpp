// SPDX-License-Identifier: MIT
#include "switch_gpu_ready_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuReady.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t)
{
}
#endif

thread_call_t switch_event_call;

namespace
{
struct Context {
	std::atomic<int> state{0}, calls{0}, notices{0}, result{99};
	std::function<void()> hook;
	switch_gpu_ready_watch *watch = nullptr;
};

int query(void *context)
{
	auto &value = *static_cast<Context *>(context);
	++value.calls;
	if (value.hook)
		value.hook();
	return value.state;
}

void completed(void *context, int result)
{
	auto &value = *static_cast<Context *>(context);
	value.result = result;
	++value.notices;
}

void drain()
{
	unsigned iterations = 0;
	while (switch_event_call && switch_event_call->pending.exchange(false)) {
		ASSERT_LT(++iterations, 1000u);
		switch_event_call->fn(switch_event_call->arg, nullptr);
	}
}

TEST(Ready, ClearedBeforeArmCompletesWithoutAnotherNotification)
{
	Context value;
	value.state = 1;
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	drain();
	EXPECT_EQ(value.notices, 1);
	EXPECT_EQ(value.result, 0);
	switch_gpu_ready_cancel(&value.watch);
	EXPECT_EQ(value.notices, 1);
}

TEST(Ready, BlockedQueriesDoNotPoll)
{
	Context value;
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	drain();
	drain();
	EXPECT_EQ(value.calls, 1);
	EXPECT_EQ(value.notices, 0);
	value.state = 1;
	switch_gpu_admission_changed();
	drain();
	EXPECT_EQ(value.notices, 1);
	switch_gpu_ready_cancel(&value.watch);
}

TEST(Ready, ReleaseWhileQueryRunsCannotLoseWake)
{
	Context value;
	value.hook = [&] {
		if (value.calls == 1)
			switch_gpu_admission_changed();
		else
			value.state = 1;
	};
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	drain();
	EXPECT_EQ(value.calls, 2);
	EXPECT_EQ(value.notices, 1);
	switch_gpu_ready_cancel(&value.watch);
}

TEST(Ready, CancellationWaitsForQueryToStopUsingContext)
{
	Context value;
	std::promise<void> entered, resume;
	auto gate = resume.get_future();
	value.hook = [&] {
		entered.set_value();
		gate.wait();
	};
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	std::thread worker(drain);
	entered.get_future().wait();
	switch_gpu_ready_cancel(&value.watch);
	EXPECT_EQ(value.notices, 0);
	resume.set_value();
	worker.join();
	EXPECT_EQ(value.notices, 1);
	EXPECT_EQ(value.result, -125);
}

TEST(Ready, ReadyAndCancelArbitrateExactlyOneCallback)
{
	Context value;
	value.state = 1;
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	switch_gpu_ready_cancel(&value.watch);
	drain();
	EXPECT_EQ(value.notices, 1);
	EXPECT_EQ(value.result, -125);
}

TEST(Ready, BoundedBatchesReachPendingListTail)
{
	std::array<Context, 80> values;
	values.front().state = 1;
	for (auto &value : values)
		ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	drain();
	EXPECT_EQ(values.front().notices, 1);
	for (auto &value : values) {
		EXPECT_EQ(value.calls, 1);
		switch_gpu_ready_cancel(&value.watch);
	}
}

TEST(Ready, GlobalBoundRejectsWithoutCallbackAndReclaimsAfterCancel)
{
	std::array<Context, 257> values;
	for (unsigned i = 0; i < 256; ++i)
		ASSERT_EQ(switch_gpu_ready_observe(query, completed, &values[i], &values[i].watch),
			  0);
	EXPECT_EQ(switch_gpu_ready_observe(query, completed, &values.back(), &values.back().watch),
		  -28);
	EXPECT_EQ(values.back().notices, 0);
	for (auto &value : values)
		switch_gpu_ready_cancel(&value.watch);
	drain();
}

TEST(Ready, QueryFailureIsTerminalWithoutSyntheticSuccess)
{
	Context value;
	value.state = -5;
	ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	drain();
	EXPECT_EQ(value.result, -5);
	EXPECT_EQ(value.notices, 1);
	switch_gpu_ready_cancel(&value.watch);
}
}

TEST(Ready, RepeatedReleasesCannotStarveOlderBlockedWatch)
{
	std::array<Context, 40> values;
	values.back().state = 1;
	for (auto &value : values)
		ASSERT_EQ(switch_gpu_ready_observe(query, completed, &value, &value.watch), 0);
	for (unsigned pass = 0; pass < 3; ++pass) {
		switch_gpu_admission_changed();
		ASSERT_TRUE(switch_event_call->pending.exchange(false));
		switch_event_call->fn(switch_event_call->arg, nullptr);
	}
	EXPECT_EQ(values.back().notices, 1);
	for (auto &value : values)
		switch_gpu_ready_cancel(&value.watch);
	drain();
}
