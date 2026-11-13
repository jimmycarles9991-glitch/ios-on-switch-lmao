// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <chrono>
#include <map>
#include <thread>
#include <tuple>
#include <vector>
#include "switch_gpu_bridge_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuBridge.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BSyncpt.h"
#include "drivers/TegraHost1x/TegraHost1x.h"

struct tgm_vm {};

struct tgm_mem {
	const uint64_t *pages;
	uint32_t count;
};

struct tgm_chan {
	uint32_t submitted, completed;
	bool failed;
};

struct switch_gpu_mapping {
	uint64_t address;
};

namespace
{
using Key = std::tuple<uint32_t, uint64_t, uint64_t>;
std::map<Key, int> statuses, refs, external;
std::array<uint32_t, 256> syncpoints;
std::array<uint32_t, 8192> pushWords;
tgm_vm space;
tgm_mem memories[2];
tgm_chan gpu;
uint32_t memoryCount, syncpointCount = 160, maps, execs, roomWaits, failureReleases, poolFrees;
int vmFailures, releaseFailures;
volatile uint32_t fence;
std::vector<switch_gpu_backing_token> submittedTokens;
uint32_t submittedWords[TGM_SYNCPT_INCR_DWORDS];
int nextExecError;
bool pushVolatile;
uint32_t submittedFlags;
switch_gpu_mapping pushWindow;
void (*fenceCallback)(void *);
void *fenceContext;

Key key(const switch_gpu_backing_token &token)
{
	return {token.queue, token.reset_generation, token.value};
}

switch_gpu_backing_token token(uint32_t value = 1, uint32_t queue = 1, uint64_t generation = 1)
{
	return {value, generation, SWITCH_GPU_ENGINE_GM20B, queue};
}

thread_call_t retirement()
{
	return switch_bridge_calls.at(0);
}

thread_call_t hangCheck()
{
	return switch_bridge_calls.at(1);
}

void pump()
{
	if (!switch_bridge_calls.empty() && retirement()->pending) {
		retirement()->pending = false;
		retirement()->fn(retirement()->arg, nullptr);
	}
}

void finish(bool failed = false)
{
	gpu.failed = failed;
	gpu.completed = gpu.submitted;
	if (fenceCallback)
		fenceCallback(fenceContext);
	pump();
}

class Bridge : public testing::Test
{
protected:
	std::vector<switch_gpu_bridge *> held;

	void SetUp() override
	{
		nextExecError = 0;
		roomWaits = 0;
		submittedTokens.clear();
	}

	void TearDown() override
	{
		for (auto &[k, status] : statuses)
			if (!status)
				status = 1;
		finish();
		for (auto *value : held)
			switch_gpu_bridge_release(&value);
		for (const auto &[k, count] : refs)
			EXPECT_EQ(count, 0);
	}

	switch_gpu_bridge *submit(std::initializer_list<switch_gpu_backing_token> tokens)
	{
		switch_gpu_bridge *bridge = nullptr;
		EXPECT_EQ(
			switch_gpu_bridge_submit(tokens.begin(), uint32_t(tokens.size()), &bridge),
			0);
		if (bridge)
			held.push_back(bridge);
		return bridge;
	}

	void release(switch_gpu_bridge *bridge)
	{
		for (auto &at : held)
			if (at == bridge)
				switch_gpu_bridge_release(&at);
	}

	void releaseSyncpoint(switch_gpu_bridge *bridge)
	{
		uint32_t id, threshold;
		switch_gpu_bridge_syncpoint(bridge, &id, &threshold);
		syncpoints[id] = threshold;
	}
};
}

extern "C" void *tgm_pool_alloc(uint64_t, uint64_t, uint64_t *pa)
{
	*pa = 0x100000;
	return pushWords.data();
}

extern "C" int tgm_fence_kernel(unsigned, volatile uint32_t **cpu, uint64_t *page, uint32_t *offset)
{
	*cpu = &fence;
	*page = 0x200000;
	*offset = 128;
	return 0;
}

extern "C" int tgm_vm_new(tgm_vm **out)
{
	if (vmFailures) {
		--vmFailures;
		return -12;
	}
	*out = &space;
	return 0;
}

extern "C" void tgm_vm_del(tgm_vm **vm)
{
	*vm = nullptr;
}

extern "C" void tgm_mem_del(tgm_mem **mem)
{
	if (*mem)
		--memoryCount;
	*mem = nullptr;
}

extern "C" void tgm_pool_free(void *, uint64_t, uint64_t)
{
	++poolFrees;
}

extern "C" void tgm_vm_claim(tgm_vm *, const void *)
{
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *pages, uint32_t count, tgm_mem **out)
{
	*out = &memories[memoryCount++];
	**out = {pages, count};
	return 0;
}

extern "C" int tgm_vm_map_ordered(tgm_vm *, uint64_t, uint64_t, tgm_mem *, uint64_t, uint8_t, int,
				  uint8_t *)
{
	++maps;
	return 0;
}

extern "C" int switch_gpu_vm_map_volatile(tgm_vm *, uint64_t va, uint64_t, tgm_mem *, uint64_t,
					  uint8_t, int, int vol, switch_gpu_mapping **out)
{
	++maps;
	pushVolatile = vol != 0;
	pushWindow = {va};
	*out = &pushWindow;
	return 0;
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **mapping)
{
	*mapping = nullptr;
}

extern "C" int tgm_vm_kmap(tgm_vm *, tgm_mem *, uint64_t, uint64_t *va)
{
	++maps;
	*va = 0x400000;
	return 0;
}

extern "C" int tgm_chan_new_copy(tgm_vm *, const void *, volatile uint32_t *, uint64_t,
				 const char *, int, tgm_chan **out)
{
	gpu = {};
	*out = &gpu;
	return 0;
}

extern "C" void tgm_chan_del(tgm_chan **chan)
{
	*chan = nullptr;
}

extern "C" uint32_t tgm_chan_error(tgm_chan *chan)
{
	return chan->failed ? 5 : 0;
}

extern "C" uint32_t tgm_chan_completed(tgm_chan *chan)
{
	return chan->completed;
}

extern "C" uint32_t tgm_chan_settled(tgm_chan *chan, int *failed)
{
	const uint32_t done = chan->completed;
	*failed = chan->failed;
	return done;
}

extern "C" void tgm_chan_on_fence(tgm_chan *, void (*fn)(void *), void *arg)
{
	fenceCallback = fn;
	fenceContext = arg;
}

std::atomic<uint32_t> progressChecks;
uint32_t checkedSequence;
uint64_t nextCheckNs = 2000000000;
std::function<void()> recoverStuckProducer;

extern "C" uint64_t tgm_chan_check_progress(tgm_chan *chan, const char *, uint32_t seqno)
{
	EXPECT_EQ(chan, &gpu);
	++progressChecks;
	checkedSequence = seqno;
	if (recoverStuckProducer)
		std::exchange(recoverStuckProducer, nullptr)();
	return nextCheckNs;
}

extern "C" int tegra_host1x_syncpt_alloc(uint32_t *id)
{
	*id = syncpointCount++;
	return 0;
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
	return syncpoints[id];
}

extern "C" int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t maximum)
{
	EXPECT_TRUE(gpu.failed);
	EXPECT_LE(uint32_t(threshold - syncpoints[id]), maximum);
	++failureReleases;
	if (releaseFailures) {
		--releaseFailures;
		return -5;
	}
	syncpoints[id] = threshold;
	return 0;
}

[[noreturn]] void panic(const char *, ...)
{
	std::abort();
}

extern "C" void switch_gpu_chan_submit_enter(tgm_chan *)
{
}

extern "C" void switch_gpu_chan_submit_leave()
{
}

extern "C" int switch_gpu_chan_wait_submission_room(tgm_chan *, const tgm_push *, uint32_t,
						    uint32_t, const tgm_prof *)
{
	++roomWaits;
	return 0;
}

extern "C" int switch_gpu_chan_exec_visibility(tgm_chan *chan, const tgm_push *push, uint32_t count,
					       const switch_gpu_backing_token *dependencies,
					       uint32_t dependencyCount, uint32_t *sequence,
					       const tgm_prof *, uint32_t flags)
{
	++execs;
	submittedFlags = flags;
	if (nextExecError) {
		const int error = nextExecError;
		nextExecError = 0;
		return error;
	}
	EXPECT_EQ(count, 1u);
	EXPECT_EQ(push->dwords, TGM_SYNCPT_INCR_DWORDS);
	std::copy_n(pushWords.data() + (push->va - (1ULL << 20)) / 4, TGM_SYNCPT_INCR_DWORDS,
		    submittedWords);
	submittedTokens.assign(dependencies, dependencies + dependencyCount);
	*sequence = ++chan->submitted;
	return 0;
}

extern "C" int switch_gpu_chan_exec(tgm_chan *chan, const tgm_push *push, uint32_t count,
				    const switch_gpu_backing_token *dependencies,
				    uint32_t dependencyCount, uint32_t *sequence,
				    const tgm_prof *prof)
{
	return switch_gpu_chan_exec_visibility(chan, push, count, dependencies, dependencyCount,
					       sequence, prof, 0);
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *token)
{
	++refs[key(*token)];
	return 0;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *token)
{
	--refs[key(*token)];
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
	return statuses[key(*token)];
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *token)
{
	const auto at = external.find(key(*token));
	return at == external.end() ? statuses[key(*token)] : at->second;
}

extern "C" int switch_gpu_timeline_promote(const switch_gpu_backing_token *token)
{
	const int ready = statuses[key(*token)];
	if (ready == 1)
		external[key(*token)] = 1;
	return ready;
}

// First in the file: the queue opens once per process.
TEST_F(Bridge, FailedFirstOpenIsUndoneAndTheNextSubmitOpensAgain)
{
	const auto source = token(40);
	statuses[key(source)] = 1;
	vmFailures = 1;
	switch_gpu_bridge *bridge = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), -12);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(poolFrees, 1u);
	EXPECT_EQ(memoryCount, 0u);
	ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(syncpointCount, 160u + 8u);
}

TEST_F(Bridge, PendingProducerPublishesHardwareAcquireWithoutCpuWait)
{
	const auto source = token(41);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	ASSERT_EQ(submittedTokens.size(), 1u);
	EXPECT_EQ(submittedTokens[0].value, 41u);
	EXPECT_EQ(refs[key(source)], 1);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 0);
	EXPECT_EQ(roomWaits, 0u);
	EXPECT_EQ(submittedWords[0], tgm_syncpt_header(0x78));
	EXPECT_EQ(submittedWords[14], tgm_syncpt_header(0x74));
}

TEST_F(Bridge, SuccessRequiresProducerAndHardwareThreshold)
{
	const auto source = token(42);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	releaseSyncpoint(bridge);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 0);
	statuses[key(source)] = 1;
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 1);
}

TEST_F(Bridge, FailedProducerCannotBecomeSuccessfulThroughSyntheticFenceAdvance)
{
	const auto source = token(43);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	releaseSyncpoint(bridge);
	statuses[key(source)] = -5;
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	finish();
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
}

TEST_F(Bridge, RetryCoverageChecksExactQueueResetGenerationAndFence)
{
	auto source = token(44);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &source), 1);
	auto other = token(45);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &other), 0);
	other = token(44, 2);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &other), 0);
	other = token(44, 1, 2);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &other), 0);
	other = token(43);
	statuses[key(other)] = 0;
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &other), 1);
}

TEST_F(Bridge, ConsumerReleaseKeepsGpuReferencesUntilFenceRetirement)
{
	auto source = token(46);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	release(bridge);
	EXPECT_EQ(refs[key(source)], 1);
	statuses[key(source)] = 1;
	finish();
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, HeldConsumerPreventsSlotReuseAfterGpuCompletion)
{
	auto source = token(47);
	statuses[key(source)] = 1;
	for (unsigned i = 0; i < 8; ++i)
		ASSERT_NE(submit({source}), nullptr);
	finish();
	switch_gpu_bridge *extra = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &extra), -16);
	EXPECT_EQ(extra, nullptr);
	release(held.front());
	ASSERT_NE(submit({source}), nullptr);
}

// NVENC's read of a native picture is a dependency like NVDEC's and VIC's: a bridge carries it to the GPU, which waits
// for it on NVENC's syncpoint (tgm_backing_dependencies).
TEST_F(Bridge, AnNvencReadIsADependencyItCarries)
{
	auto source = token(53, 7);
	source.engine = SWITCH_GPU_ENGINE_NVENC;
	statuses[key(source)] = 0;
	ASSERT_NE(submit({source}), nullptr);
	ASSERT_EQ(submittedTokens.size(), 1u);
	EXPECT_EQ(submittedTokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_NVENC));
	EXPECT_EQ(submittedTokens[0].queue, 7u);
}

TEST_F(Bridge, FailedSubmissionReleasesEveryRetainedToken)
{
	auto source = token(48);
	statuses[key(source)] = 0;
	nextExecError = -22;
	switch_gpu_bridge *bridge = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), -22);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, RingCapacityWaitDoesNotDropOrReplaceDependencies)
{
	auto source = token(49);
	statuses[key(source)] = 0;
	nextExecError = -11;
	ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(roomWaits, 1u);
	ASSERT_EQ(submittedTokens.size(), 1u);
	EXPECT_EQ(submittedTokens[0].value, source.value);
}

TEST_F(Bridge, FailedChannelLeavesNoSuccessfulBridgeAndCanReopenAfterRelease)
{
	auto source = token(50);
	statuses[key(source)] = 1;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	finish(true);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	release(bridge);
	ASSERT_NE(submit({source}), nullptr);
}

TEST_F(Bridge, WarmSubmissionsDoNotMapAndPageVectorsRemainPersistent)
{
	auto source = token(51);
	statuses[key(source)] = 1;
	ASSERT_NE(submit({source}), nullptr);
	const auto before = maps;
	ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(maps, before);
	ASSERT_EQ(memoryCount, 2u);
	EXPECT_EQ(memories[0].pages[7], 0x107000u);
	EXPECT_EQ(memories[1].pages[0], 0x200000u);
}

TEST_F(Bridge, KilledChannelRoomWaitRequiresAllHeldConsumersToRetire)
{
	using namespace std::chrono_literals;
	auto source = token(52);
	statuses[key(source)] = 1;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	finish(true);
	auto waiter = std::async(std::launch::async, [] {
		return switch_gpu_bridge_wait_room();
	});
	EXPECT_EQ(waiter.wait_for(20ms), std::future_status::timeout);
	release(bridge);
	EXPECT_EQ(waiter.get(), 0);
}

TEST_F(Bridge, QuiescedChannelFailureReleasesHardwareWaitWithoutSuccess)
{
	auto source = token(53);
	statuses[key(source)] = 1;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	uint32_t id, threshold;
	switch_gpu_bridge_syncpoint(bridge, &id, &threshold);
	const auto before = failureReleases;
	EXPECT_LT(int32_t(syncpoints[id] - threshold), 0);
	finish(true);
	EXPECT_EQ(failureReleases, before + 1);
	EXPECT_EQ(syncpoints[id], threshold);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
}

TEST_F(Bridge, UnreleasableFailedSyncpointLeavesItsSlotForAnotherWithoutPanic)
{
	auto source = token(54);
	statuses[key(source)] = 1;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	uint32_t id, threshold;
	switch_gpu_bridge_syncpoint(bridge, &id, &threshold);
	releaseFailures = 1;
	const unsigned logged = switch_bridge_logs;
	finish(true);
	EXPECT_EQ(switch_bridge_logs, logged + 1);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	release(bridge);
	const uint32_t made = syncpointCount;
	for (unsigned i = 0; i < 8; ++i)
		ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(syncpointCount, made + 1);
	for (auto *each : held) {
		if (!each)
			continue;
		uint32_t other, unused;
		switch_gpu_bridge_syncpoint(each, &other, &unused);
		EXPECT_NE(other, id);
	}
}

TEST_F(Bridge, ExactDisplayFenceWaitsForNativeProducerAndRetainsUntilCallback)
{
	auto source = token();
	source.value = 0x100000001ULL;
	source.engine = SWITCH_GPU_ENGINE_NVDEC;
	statuses[key(source)] = 0;
	int calls = 0;
	ASSERT_EQ(switch_gpu_bridge_fence(
			  &source, 1,
			  [](void *context, int error) {
				  EXPECT_EQ(error, 0);
				  ++*static_cast<int *>(context);
			  },
			  &calls),
		  0);
	pump();
	EXPECT_EQ(calls, 0);
	EXPECT_EQ(refs[key(source)], 1);
	statuses[key(source)] = 1;
	finish();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, ExactDisplayFencePropagatesSourceFailure)
{
	auto source = token(54);
	statuses[key(source)] = 0;
	int result = 0;
	ASSERT_EQ(switch_gpu_bridge_fence(
			  &source, 1,
			  [](void *context, int error) {
				  *static_cast<int *>(context) = error;
			  },
			  &result),
		  0);
	statuses[key(source)] = -5;
	finish();
	EXPECT_EQ(result, -5);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, HardwareFenceBeforeMediaRetirementDefersCallbackUntilCompletionNotification)
{
	auto source = token(61);
	source.engine = SWITCH_GPU_ENGINE_NVDEC;
	statuses[key(source)] = 0;
	int calls = 0;
	ASSERT_EQ(switch_gpu_bridge_fence(
			  &source, 1,
			  [](void *context, int error) {
				  EXPECT_EQ(error, 0);
				  ++*static_cast<int *>(context);
			  },
			  &calls),
		  0);
	finish();
	EXPECT_EQ(calls, 0);
	EXPECT_EQ(refs[key(source)], 1);
	EXPECT_FALSE(retirement()->pending);
	statuses[key(source)] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, ExecutedInternalProducerStillRequiresBridgeCoverage)
{
	auto source = token(150), other = token(151);
	statuses[key(source)] = statuses[key(other)] = 1;
	external[key(source)] = external[key(other)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &source), 1);
	EXPECT_EQ(switch_gpu_bridge_covers(bridge, &other), 0);
	EXPECT_EQ(external[key(source)], 0);
	releaseSyncpoint(bridge);
	finish();
	EXPECT_EQ(external[key(source)], 1);
	EXPECT_EQ(external[key(other)], 0);
}

TEST_F(Bridge, RetainedConsumerWaitJoinsLaggingSemanticProducerWithoutPolling)
{
	using namespace std::chrono_literals;
	auto source = token(91);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	releaseSyncpoint(bridge);
	finish();
	auto waiter = std::async(std::launch::async, [bridge] {
		return switch_gpu_bridge_wait(bridge);
	});
	EXPECT_EQ(waiter.wait_for(20ms), std::future_status::timeout);
	statuses[key(source)] = -5;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(waiter.get(), -5);
	EXPECT_EQ(refs[key(source)], 1);
	EXPECT_EQ(switch_gpu_bridge_wait(bridge), -5);
	release(bridge);
	EXPECT_EQ(refs[key(source)], 0);
}

// Start a waiter and return once it has armed the hang check, asleep or about to sleep on the bridge.
std::future<int> waitArmed(switch_gpu_bridge *bridge)
{
	using namespace std::chrono_literals;
	const unsigned arms = switch_bridge_arms;
	auto waiter = std::async(std::launch::async, [bridge] {
		return switch_gpu_bridge_wait(bridge);
	});
	for (unsigned i = 0; i < 2000 && switch_bridge_arms == arms; ++i)
		std::this_thread::sleep_for(1ms);
	EXPECT_NE(switch_bridge_arms.load(), arms);
	return waiter;
}

void fireHangCheck(uint64_t at)
{
	switch_bridge_now = at;
	hangCheck()->deadline = 0;
	hangCheck()->fn(hangCheck()->arg, nullptr);
}

// The wait arms one timer for the first check, a second after it begins; each check re-arms it only for when
// tgm_chan_check_progress says its rule can take the next step; the recovery that check starts completes the bridge,
// which wakes the waiter and cancels the timer.
TEST_F(Bridge, AStalledBridgeIsCheckedByOneTimerTheHangRuleReArmsUntilRecoveryCompletesIt)
{
	auto source = token(95);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	progressChecks = 0;
	switch_bridge_now = 5000;
	auto waiter = waitArmed(bridge);
	EXPECT_EQ(hangCheck()->deadline, 5000u + 1000000000u);
	EXPECT_EQ(progressChecks.load(), 0u);
	fireHangCheck(1000005000);
	EXPECT_EQ(progressChecks.load(), 1u);
	EXPECT_EQ(checkedSequence, gpu.submitted);
	EXPECT_EQ(hangCheck()->deadline, 1000005000u + nextCheckNs);
	recoverStuckProducer = [&] {
		statuses[key(source)] = -5;
		releaseSyncpoint(bridge);
		finish();
	};
	fireHangCheck(3000005000);
	EXPECT_EQ(waiter.get(), -5);
	EXPECT_EQ(progressChecks.load(), 2u);
	EXPECT_EQ(hangCheck()->deadline, 0u);
	release(bridge);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, AWaitThatCompletesBeforeItsCheckLeavesNoTimerAndNoCheck)
{
	auto source = token(96);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(hangCheck()->deadline, 0u);
	progressChecks = 0;
	auto waiter = waitArmed(bridge);
	EXPECT_NE(hangCheck()->deadline, 0u);
	statuses[key(source)] = 1;
	releaseSyncpoint(bridge);
	finish();
	EXPECT_EQ(waiter.get(), 1);
	EXPECT_EQ(hangCheck()->deadline, 0u);
	EXPECT_EQ(progressChecks.load(), 0u);
}

// A channel with nothing pending has no next step: the timer stays down until another wait begins, or new work goes on
// the channel while a waiter is still there (here one whose bridge waits on a media producer after its GPU work).
TEST_F(Bridge, ACheckThatFindsNothingPendingLeavesTheTimerDownUntilNewWorkComes)
{
	auto source = token(97);
	source.engine = SWITCH_GPU_ENGINE_NVDEC;
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	releaseSyncpoint(bridge);
	finish();
	auto waiter = waitArmed(bridge);
	nextCheckNs = 0;
	fireHangCheck(switch_bridge_now + 1000000000);
	nextCheckNs = 2000000000;
	EXPECT_EQ(hangCheck()->deadline, 0u);
	const auto other = token(98);
	statuses[key(other)] = 0;
	ASSERT_NE(submit({other}), nullptr);
	EXPECT_EQ(hangCheck()->deadline, switch_bridge_now + 1000000000u);
	statuses[key(source)] = statuses[key(other)] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(waiter.get(), 1);
	finish();
	EXPECT_EQ(hangCheck()->deadline, 0u);
}

TEST_F(Bridge, ABridgeNobodyWaitsForArmsNoCheck)
{
	const auto source = token(99);
	statuses[key(source)] = 0;
	ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(hangCheck()->deadline, 0u);
}

// A bridge job reads no memory: its volatile push slot needs no L2 invalidate ahead of it, and the flush that puts its
// producers' writes in DRAM before host1x sees the increment is in the push itself.
TEST_F(Bridge, AJobNeedsNoL2InvalidateAndItsPushFlushesTheProducers)
{
	submit({token(77)});
	EXPECT_TRUE(pushVolatile);
	EXPECT_TRUE(submittedFlags & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT);
	EXPECT_TRUE(submittedFlags & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
	EXPECT_FALSE(submittedFlags & SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	const uint32_t *begin = submittedWords, *end = begin + TGM_SYNCPT_INCR_DWORDS;
	const auto *flush = std::find(begin, end, 0x10u << 27);
	const auto *increment = std::find(begin, end, tgm_syncpt_header(0x0074));
	ASSERT_NE(flush, end);
	EXPECT_LT(flush, increment);
}

// VIC and NVDEC retire without a thread: the bridge their job acquires reports to them from its own retirement.
TEST_F(Bridge, ANotifiedConsumerHearsFromRetirementOnceAndIsAWaiterForTheHangCheck)
{
	auto source = token(160);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	std::vector<int> heard;
	const auto record = [](void *context, int status) {
		static_cast<std::vector<int> *>(context)->push_back(status);
	};
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, record, &heard), 0);
	EXPECT_NE(hangCheck()->deadline, 0u);
	EXPECT_EQ(switch_gpu_bridge_notify(bridge, record, &heard), -16);
	pump();
	EXPECT_TRUE(heard.empty());
	statuses[key(source)] = 1;
	releaseSyncpoint(bridge);
	finish();
	EXPECT_EQ(heard, std::vector<int>{0});
	EXPECT_EQ(hangCheck()->deadline, 0u);
	finish();
	EXPECT_EQ(heard.size(), 1u);
	release(bridge);
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, ANotifyOnAFinishedBridgeIsDeliveredByRetirementNotInline)
{
	auto source = token(161);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = -5;
	releaseSyncpoint(bridge);
	finish();
	int heard = 1;
	ASSERT_EQ(switch_gpu_bridge_notify(
			  bridge,
			  [](void *context, int status) {
				  *static_cast<int *>(context) = status;
			  },
			  &heard),
		  0);
	EXPECT_EQ(heard, 1);
	EXPECT_TRUE(retirement()->pending);
	pump();
	EXPECT_EQ(heard, -5);
	release(bridge);
}
