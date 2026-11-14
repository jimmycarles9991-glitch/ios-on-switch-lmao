// SPDX-License-Identifier: MIT
// SwitchGpuPresentFilter.cpp over the real timeline: the nonstall filter runs the display's callout once the watched
// producer is complete and visible, never takes a blocking lock, never after the claim's detach, and leaves a token
// whose status needs the timeline's lock (a semantic wait, a killed channel's abandoned fence) to the work loop.
#include <gtest/gtest.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <vector>
#include "bazel/testing/source_text.h"
#include "switch_gpu_timeline_stubs.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "drivers/TegraGM20B/SwitchGpuPresentWatch.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

struct switch_gpu_event_waits {
	unsigned refs = 1;
	int result = 0;
};

namespace
{
switch_gpu_backing_engine operations{};
uint64_t nextId = 0x5000;
std::vector<int> callouts;

void callout(void *ctx, int status)
{
	EXPECT_EQ(ctx, &callouts);
	EXPECT_TRUE(switchTimelineAtomicContext) << "outside the filter's simple lock";
	callouts.push_back(status);
}
}  // namespace

extern "C" int switch_gpu_backing_register_engine(uint32_t, const switch_gpu_backing_engine *ops)
{
	operations = *ops;
	return 0;
}

// SwitchGpuBacking.cpp's dispatch, to the one engine here
extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *token)
{
	return operations.retain_token(token);
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *token)
{
	operations.release_token(token);
}

extern "C" int switch_gpu_backing_register_visibility(uint32_t,
						      const switch_gpu_backing_visibility *)
{
	return 0;
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t, const switch_gpu_backing_semantics *)
{
	return 0;
}

extern "C" int switch_gpu_backing_register_timeline_refs(uint32_t,
							 const switch_gpu_backing_timeline_refs *)
{
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	++batch->refs;
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	if (*batch) {
		--(*batch)->refs;
		*batch = nullptr;
	}
}

extern "C" int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch->result;
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result;
}

extern "C" int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *token)
{
	return switch_gpu_timeline_promote(token);
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
}

extern "C" void switch_gpu_backing_completed()
{
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t)
{
}

namespace
{
class PresentFilter : public testing::Test
{
protected:
	volatile uint32_t fence = 4, visible = 4;
	switch_gpu_timeline *timeline = nullptr;
	switch_gpu_backing_token watched{};

	void SetUp() override
	{
		ASSERT_EQ(switch_gpu_timeline_init(), 0);
		callouts.clear();
		tgm_present_filter_callout(callout, &callouts);
		timeline = switch_gpu_timeline_create(++nextId, &fence);
		ASSERT_NE(timeline, nullptr);
		switch_gpu_timeline_visibility(timeline, &visible);
		switch_gpu_timeline_token(timeline, 5, &watched);
		ASSERT_EQ(operations.retain_token(&watched), 0);
	}

	void TearDown() override
	{
		tgm_present_filter_detach();
		operations.release_token(&watched);
		switch_gpu_timeline_close(timeline);
	}

	static void filter()
	{
		switchTimelineAtomicContext = true;
		tgm_present_filter();
		switchTimelineAtomicContext = false;
	}
};

// The filter runs in primary interrupt context on every GPU interrupt, and takes no lock: nothing watched, watched, and
// the run that calls the display.
TEST_F(PresentFilter, TheFilterTakesNoLock)
{
	const unsigned before = switchSimpleLocks;
	filter();
	tgm_present_filter_attach(&watched);
	filter();
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
	EXPECT_EQ(switchSimpleLocks, before);
}

// A detach while another CPU's filter run is inside the callout keeps the view's own reference until that run has
// left, so the producer's timeline outlives the read; the next attach or detach releases it.
TEST_F(PresentFilter, ADetachDuringAFilterRunKeepsTheTimelineUntilItLeaves)
{
	switch_gpu_backing_token earlier{};
	switch_gpu_timeline_token(timeline, 4, &earlier);
	auto *closing = timeline;
	timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_NE(timeline, nullptr);

	struct Gate {
		std::mutex lock;
		std::condition_variable changed;
		bool inside = false, leave = false;
	} gate;

	static Gate *open;
	open = &gate;
	tgm_present_filter_callout(
		[](void *, int) {
			std::unique_lock held(open->lock);
			open->inside = true;
			open->changed.notify_all();
			open->changed.wait(held, [] {
				return open->leave;
			});
		},
		nullptr);
	tgm_present_filter_attach(&earlier);
	std::thread cpu3([] {
		switchCpu = 3;
		filter();
	});
	{
		std::unique_lock held(gate.lock);
		gate.changed.wait(held, [&] {
			return gate.inside;
		});
	}
	operations.release_token(&watched);
	tgm_present_filter_detach();
	switch_gpu_timeline_close(closing);
	EXPECT_EQ(operations.status(&earlier), 1)
		<< "held by the dropped view while CPU 3 reads it";
	{
		std::lock_guard held(gate.lock);
		gate.leave = true;
		gate.changed.notify_all();
	}
	cpu3.join();
	tgm_present_filter_detach();
	EXPECT_NE(operations.status(&earlier), 1) << "released once CPU 3 left";
	tgm_present_filter_callout(callout, &callouts);
}

// Attach and detach on one thread against the filter on two CPUs, each view's timeline closed right after its detach:
// no filter run reads a freed timeline (ASan, ThreadSanitizer), and each view's callout runs at most once.
TEST_F(PresentFilter, ViewsComeAndGoUnderTheFilter)
{
	constexpr int kViews = 3000;
	static std::atomic<unsigned> calls;
	calls = 0;
	tgm_present_filter_callout(
		[](void *, int) {
			calls.fetch_add(1, std::memory_order_relaxed);
		},
		nullptr);
	std::atomic<bool> stop{false};
	std::vector<std::thread> cpus;
	for (int cpu = 1; cpu <= 2; ++cpu)
		cpus.emplace_back([&stop, cpu] {
			switchCpu = cpu;
			while (!stop.load(std::memory_order_acquire))
				filter();
		});
	static volatile uint32_t done = 7, shown = 7;
	for (int i = 0; i < kViews; ++i) {
		auto *producer = switch_gpu_timeline_create(++nextId, &done);
		ASSERT_NE(producer, nullptr);
		switch_gpu_timeline_visibility(producer, &shown);
		switch_gpu_backing_token token{};
		switch_gpu_timeline_token(producer, 7, &token);
		tgm_present_filter_attach(&token);
		std::this_thread::yield();
		tgm_present_filter_detach();
		switch_gpu_timeline_close(producer);
	}
	stop.store(true, std::memory_order_release);
	for (auto &cpu : cpus)
		cpu.join();
	tgm_present_filter_detach();
	EXPECT_LE(calls.load(), unsigned(kViews));
	tgm_present_filter_callout(callout, &callouts);
}

TEST_F(PresentFilter, TheCalloutRunsOnceThePassedFenceIsVisibleAndOnlyOnce)
{
	tgm_present_filter_attach(&watched);
	filter();
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty()) << "complete, its writes still in the L2";
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PresentFilter, NothingRunsAfterTheClaimDetaches)
{
	tgm_present_filter_attach(&watched);
	tgm_present_filter_detach();
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
}

TEST_F(PresentFilter, AProducerAlreadyVisibleAtTheAttachRunsAtTheNextFilter)
{
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	tgm_present_filter_attach(&watched);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PresentFilter, AKilledChannelsAdvancedFenceIsNeverVisible)
{
	tgm_present_filter_attach(&watched);
	switch_gpu_timeline_failed(timeline, 4);
	__atomic_store_n(&fence, 100u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 100u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
	EXPECT_EQ(operations.status(&watched), -5);
}

TEST_F(PresentFilter, ASemanticWaitAtOrBelowTheTokenIsLeftToTheWorkLoop)
{
	switch_gpu_event_waits authority;
	switch_gpu_timeline_semantic *node = nullptr;
	ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&watched, &authority, &node), 0);
	tgm_present_filter_attach(&watched);
	__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
	__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
	filter();
	EXPECT_TRUE(callouts.empty());
	switch_gpu_timeline_semantic_finish(&node, 0);
}

TEST_F(PresentFilter, AClosedChannelsTokenReadsWhatItCompletedBeforeTheClose)
{
	switch_gpu_backing_token earlier{};
	switch_gpu_timeline_token(timeline, 4, &earlier);
	tgm_present_filter_attach(&earlier);
	auto *closing = timeline;
	timeline = switch_gpu_timeline_create(++nextId, &fence);
	ASSERT_EQ(operations.retain_token(&earlier), 0);
	switch_gpu_timeline_close(closing);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
	tgm_present_filter_detach();
	operations.release_token(&earlier);
}

constexpr uint32_t kPromiseOffset = 2052, kPromiseValue = 9;
uint32_t promiseWord = kPromiseValue - 1;
bool promiseAbandoned;

// SwitchGpuWriterPromise.cpp's status for the promise; the timeline's for the job
int claimStatus(const switch_gpu_backing_token *token)
{
	if (token->engine != SWITCH_GPU_ENGINE_PROMISE)
		return operations.status(token);
	if (promiseAbandoned)
		return -5;
	return int32_t(promiseWord - uint32_t(token->value)) >= 0 ? 1 : 0;
}

// A frame behind a writer promise: the watch waits for it with no view in the filter (the offer attaches only a GM20B
// token), and gpu_promise.c tgm_present_promise_kept hands it to the keeping job, attaching that job's view before the
// re-arm. The work item claims what the CPU released or gave up on, and detaches first (present_watch_work).
class PromisedPresentFilter : public PresentFilter
{
protected:
	switch_gpu_present_watch watch{};
	const switch_gpu_backing_token promise = {kPromiseValue, 0, SWITCH_GPU_ENGINE_PROMISE,
						  kPromiseOffset};

	void SetUp() override
	{
		PresentFilter::SetUp();
		promiseWord = kPromiseValue - 1;
		promiseAbandoned = false;
		switch_gpu_present_watch_promise(&watch, &promise);
	}

	bool handOver()
	{
		if (!switch_gpu_present_watch_kept(&watch, kPromiseOffset, kPromiseValue, &watched))
			return false;
		tgm_present_filter_attach(&watched);
		switch_gpu_present_watch_rearm(&watch);
		return true;
	}

	int workItem()
	{
		const int found = switch_gpu_present_watch_claim(&watch, claimStatus, claimStatus);
		if (found != SWITCH_GPU_PRESENT_WATCH_PENDING)
			tgm_present_filter_detach();
		return found;
	}

	void jobEnds()
	{
		__atomic_store_n(&fence, 5u, __ATOMIC_RELEASE);
		__atomic_store_n(&visible, 5u, __ATOMIC_RELEASE);
		promiseWord = kPromiseValue;
	}
};

TEST_F(PromisedPresentFilter, TheKeepingJobsEndFlipsFromTheFilter)
{
	filter();
	ASSERT_TRUE(handOver());
	filter();
	EXPECT_TRUE(callouts.empty());
	jobEnds();
	filter();
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PromisedPresentFilter, ACancelShowsTheFrameFromTheWorkItem)
{
	promiseWord = kPromiseValue;
	EXPECT_EQ(workItem(), SWITCH_GPU_PRESENT_WATCH_VISIBLE);
	filter();
	EXPECT_TRUE(callouts.empty());
	EXPECT_FALSE(handOver()) << "claimed: a job submitted after the cancel watches nothing";
}

TEST_F(PromisedPresentFilter, AnUnkeptPromiseIsDroppedAtTheDeadlineAndALateJobFlipsNothing)
{
	promiseAbandoned = true;
	EXPECT_EQ(workItem(), SWITCH_GPU_PRESENT_WATCH_FAILED);
	EXPECT_FALSE(handOver());
	jobEnds();
	filter();
	EXPECT_TRUE(callouts.empty());
}

// The keeping job ends (and its fence push releases the word) before its hand-over: the filter had no view then, and
// the view attached at the hand-over is already visible. Whichever of the next filter and the hand-over's last look's
// work item comes first delivers it; the other finds nothing to do.
TEST_F(PromisedPresentFilter, AJobThatEndsBeforeItsHandOverIsDeliveredOnce)
{
	jobEnds();
	filter();
	EXPECT_TRUE(callouts.empty());
	ASSERT_TRUE(handOver());
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
	EXPECT_EQ(workItem(), SWITCH_GPU_PRESENT_WATCH_VISIBLE);
	filter();
	EXPECT_EQ(callouts, std::vector<int>{0});
}

TEST_F(PromisedPresentFilter, AWorkItemThatClaimsFirstLeavesTheFilterNothing)
{
	jobEnds();
	ASSERT_TRUE(handOver());
	EXPECT_EQ(workItem(), SWITCH_GPU_PRESENT_WATCH_VISIBLE);
	filter();
	EXPECT_TRUE(callouts.empty());
}

// The order SwitchGpuPresentFilter.cpp relies on: the view goes before the watch is armed and is dropped right after
// each claim, before the tokens drop and done runs; the nonstall line alone is filtered, masked until its pass.
// A direct present of a producer that ended without external visibility (every compositor job on boot 19: d_filter 0 in
// 301 frames) is delivered by present_watch_flush's fence-only present job. That job's fence, on the kernel present
// channel, is its visibility, so the filter watches it: attached before its kick, detached before its completion's done.
TEST(PresentFilterSource, ThePresentJobsOwnFenceIsWatchedByTheFilter)
{
	using switch_ios::test::Between;
	using switch_ios::test::SourceText;
	const std::string present = SourceText("../gpu_present.c");
	const std::string copy = Between(present, "static int present_copy(", "\n}\n");
	const auto attach = copy.find("tgm_present_filter_attach(&own);");
	ASSERT_NE(attach, std::string::npos);
	EXPECT_NE(copy.find("switch_gpu_chan_next_token(chan, &own);"), std::string::npos);
	EXPECT_LT(attach, copy.find("chan_fence_volatile(chan);"));
	const std::string complete =
		Between(present, "static void present_typed_complete(void *unused)", "\n}\n");
	ASSERT_NE(complete.find("tgm_present_filter_detach();"), std::string::npos);
	EXPECT_LT(complete.find("tgm_present_filter_detach();"),
		  complete.find("done(arg, status);"));
}

TEST(PresentFilterSource, TheWatchAttachesBeforeItsArmAndEachClaimDetachesFirst)
{
	using switch_ios::test::Between;
	using switch_ios::test::SourceText;
	const std::string present = SourceText("../gpu_present.c");
	const std::string watch = Between(present, "int tgm_present_watch_dependencies(", "\n}\n");
	EXPECT_LT(watch.find("tgm_present_filter_attach(&tgm_present.tokens.tokens[0]);"),
		  watch.find("switch_gpu_present_watch_arm("));
	const std::string pass = Between(present, "void tgm_present_pass(void)\n{", "\n}\n");
	EXPECT_LT(pass.find("switch_gpu_present_watch_take_visible("),
		  pass.find("tgm_present_filter_detach();"));
	EXPECT_LT(pass.find("tgm_present_filter_detach();"), pass.find("present_watch_complete("));
	const std::string work =
		Between(present, "static void present_watch_work(void *unused)", "\n}\n");
	EXPECT_LT(work.find("switch_gpu_present_watch_claim("),
		  work.find("tgm_present_filter_detach();"));
	EXPECT_LT(work.find("tgm_present_filter_detach();"), work.find("present_watch_flush();"));
	const std::string kept =
		Between(SourceText("../gpu_promise.c"), "void tgm_present_promise_kept(", "\n}\n");
	EXPECT_LT(kept.find("switch_gpu_present_watch_kept("),
		  kept.find("tgm_present_filter_attach(producer);"));
	EXPECT_LT(kept.find("tgm_present_filter_attach(producer);"),
		  kept.find("switch_gpu_present_watch_rearm("));
	EXPECT_NE(kept.find("tgm_present_filter_attach(producer);"), std::string::npos);
	const std::string glue = SourceText("../shim/xnu_glue.cpp");
	const std::string filterBody = Between(glue, "bool gpuFilter(", "\n}\n");
	EXPECT_NE(filterBody.find("tgm_present_filter();"), std::string::npos);
	EXPECT_NE(filterBody.find("return true;"), std::string::npos);
	// both lines: nvkm routes the jobs' interrupts to the stall line, where the filter must run
	const std::string start = Between(glue, "extern \"C\" void nvxnu_irq_start(void)", "\n}\n");
	EXPECT_NE(start.find("gIrqLoop, gpuInterrupt, gpuFilter, nub, 0)"), std::string::npos);
	EXPECT_EQ(start.find("IOInterruptEventSource::interruptEventSource("), std::string::npos)
		<< "a line with no filter";
}
}  // namespace
