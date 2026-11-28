// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <vector>
#include "switch_gpu_bridge_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuBridge.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
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
std::mutex stateMutex;
std::map<Key, int> statuses, physicalStatuses, refs, external;
std::function<void(const switch_gpu_backing_token &)>
	onTokenRead;  // at a token's status read, before it answers
std::array<uint32_t, 256> syncpoints;
std::array<uint32_t, 8192> pushWords;
tgm_vm space;
tgm_mem memories[2];
tgm_chan gpu;
uint32_t memoryCount, syncpointCount = 160, maps, execs, roomWaits, failureReleases, poolFrees;
int vmFailures, gateVaFailures, releaseFailures;
volatile uint32_t fence;
std::vector<switch_gpu_backing_token> submittedTokens;
std::vector<uint64_t> execOrder;
// A slot's push: the gate's acquire (SwitchGpuBridgeGate.cpp), then the drain and the increments
constexpr uint32_t kGateDwords = 5, kSlotDwords = kGateDwords + TGM_SYNCPT_INCR_DWORDS;
constexpr uint64_t kGateVa = 0x300000100ull;
uint32_t submittedWords[kSlotDwords];
volatile uint32_t gateWord;
std::vector<std::pair<uint32_t, uint32_t>>
	cancelledWaiters;  // tegra_host1x_syncpt_cancel_waiters's (id, threshold)
std::function<void(uint32_t, uint32_t)> onCancelWaiters;
std::function<void()> onExec;
std::function<void()> onSubmitEnter, onOpenChannel;
uint32_t physicalStatusNonGpuCalls;
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

// Allocated with the space, after the two above
thread_call_t gatePass()
{
	return switch_bridge_calls.at(2);
}

uint32_t loadGate()
{
	return __atomic_load_n(&gateWord, __ATOMIC_ACQUIRE);
}

bool runGatePass()
{
	if (switch_bridge_calls.size() > 2 &&
	    gatePass()->pending.exchange(false, std::memory_order_acq_rel)) {
		gatePass()->fn(gatePass()->arg, nullptr);
		return true;
	}
	return false;
}

void pump()
{
	if (!switch_bridge_calls.empty() &&
	    retirement()->pending.exchange(false, std::memory_order_acq_rel))
		retirement()->fn(retirement()->arg, nullptr);
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
		onCancelWaiters = nullptr;
		onExec = nullptr;
		onSubmitEnter = nullptr;
		onOpenChannel = nullptr;
		gateVaFailures = 0;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			physicalStatuses.clear();
			for (auto &[k, status] : statuses)
				if (!status)
					status = 1;
		}
		while (runGatePass())
			;
		finish();
		for (auto *value : held)
			switch_gpu_bridge_release(&value);
		for (const auto &[k, count] : refs)
			EXPECT_EQ(count, 0);
		// The hang timer is never cancelled at idle (SwitchGpuBridgeRetire.cpp checkHang): its one run finds nothing
		// waited and stays down, so the next test starts with it down
		if (switch_bridge_calls.size() > 1 && hangCheck()->deadline) {
			hangCheck()->deadline = 0;
			hangCheck()->fn(hangCheck()->arg, nullptr);
		}
	}

	switch_gpu_bridge *submitWatched(switch_gpu_backing_token token)
	{
		switch_gpu_bridge *bridge = nullptr;
		EXPECT_EQ(switch_gpu_bridge_submit_watched(&token, 1, &bridge), 0);
		if (bridge)
			held.push_back(bridge);
		return bridge;
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

extern "C" int tgm_fence_kernel(unsigned which, volatile uint32_t **cpu, uint64_t *page,
				uint32_t *offset)
{
	*cpu = which == TGM_FENCE_KERNEL_BRIDGE_GATE ? &gateWord : &fence;
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
	if (onOpenChannel)
		onOpenChannel();
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

extern "C" int tegra_host1x_syncpt_alloc_named(uint32_t *id, const char *)
{
	*id = syncpointCount++;
	return 0;
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
	return syncpoints[id];
}

// Syncpoints of GM20B channels (SwitchGpuTimelineSyncpt.cpp) in these tests: a jobless bridge's producer's
constexpr uint32_t kProducerSyncpoint = 230;

// A GM20B token's draining job: the channel's syncpoint, its threshold and the job's last seqno
struct Covering {
	uint32_t syncpoint, threshold, last;
};

std::map<Key, Covering> covered;
std::map<Key, int> coveringStatus;  // by the covering job's (queue, generation, last): 1, 0 or -5
std::set<uint32_t> stoppedChannels, unassigned, producerIds;
std::function<void()> onCoveredRead;  // at the covering job's read, before it answers
std::vector<std::pair<uint32_t, uint32_t>> adopted;
std::vector<std::pair<uint32_t, uint32_t>> returned;  // (syncpoint, promised)
std::vector<std::pair<uint32_t, uint32_t>> producerChecks;
std::vector<switch_gpu_backing_token> needed;

extern "C" int switch_gpu_timeline_syncpoint(const switch_gpu_backing_token *token, uint32_t *id,
					     uint32_t *threshold, uint32_t *last)
{
	if (unassigned.count(token->queue))
		return -61;
	const auto at = covered.find(key(*token));
	if (at == covered.end())
		return 0;
	*id = at->second.syncpoint;
	*threshold = at->second.threshold;
	*last = at->second.last;
	return 1;
}

extern "C" int switch_gpu_timeline_covered(const switch_gpu_backing_token *token, uint32_t last,
					   int *stopped)
{
	if (onCoveredRead)
		onCoveredRead();
	*stopped = stoppedChannels.count(token->queue) != 0;
	const auto at = coveringStatus.find({token->queue, token->reset_generation, last});
	return at == coveringStatus.end() ? 0 : at->second;
}

extern "C" int switch_gpu_timeline_syncpt_adopt(const switch_gpu_backing_token *token, uint32_t id,
						uint32_t base)
{
	unassigned.erase(token->queue);
	adopted.emplace_back(id, base);
	producerIds.insert(id);
	return 0;
}

extern "C" uint32_t switch_gpu_timeline_syncpt_reclaim(uint32_t *ids, uint32_t *promised,
						       uint32_t max)
{
	uint32_t taken = 0;
	for (; taken < max && !returned.empty(); returned.pop_back(), ++taken) {
		ids[taken] = returned.back().first;
		promised[taken] = returned.back().second;
	}
	return taken;
}

extern "C" void tgm_chan_need_tokens(const switch_gpu_backing_token *tokens, uint32_t count)
{
	needed.insert(needed.end(), tokens, tokens + count);
}

std::vector<uint32_t> notedJobless;  // tgm_chan_note_jobless's channels, in order

extern "C" void tgm_chan_note_jobless(uint32_t uid)
{
	notedJobless.push_back(uid);
}

extern "C" uint64_t tgm_writer_check_progress(uint32_t uid, uint32_t seqno, const char *)
{
	producerChecks.emplace_back(uid, seqno);
	return nextCheckNs;
}

std::function<void()> onReleaseFailed;  // runs at a release, before its increments

extern "C" uint64_t tgm_fence_kernel_va(const tgm_vm *, unsigned which)
{
	if (which == TGM_FENCE_KERNEL_BRIDGE_GATE && gateVaFailures) {
		--gateVaFailures;
		return 0;
	}
	return which == TGM_FENCE_KERNEL_BRIDGE_GATE ? kGateVa : 0x300000000ull + which * 64;
}

extern "C" int tegra_host1x_syncpt_cancel_waiters(uint32_t id, uint32_t threshold)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		cancelledWaiters.emplace_back(id, threshold);
	}
	if (onCancelWaiters)
		onCancelWaiters(id, threshold);
	return 0;
}

extern "C" int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t maximum)
{
	if (onReleaseFailed)
		onReleaseFailed();
	EXPECT_TRUE(gpu.failed || id >= kProducerSyncpoint || producerIds.count(id));
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
	if (onSubmitEnter)
		onSubmitEnter();
}

extern "C" void switch_gpu_chan_submit_leave()
{
}

std::function<int()> roomWait;

extern "C" int switch_gpu_chan_wait_submission_room(tgm_chan *, const tgm_push *, uint32_t,
						    uint32_t, const tgm_prof *)
{
	++roomWaits;
	return roomWait ? roomWait() : 0;
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
	EXPECT_EQ(push->dwords, kSlotDwords);
	std::copy_n(pushWords.data() + (push->va - (1ULL << 20)) / 4, kSlotDwords, submittedWords);
	submittedTokens.assign(dependencies, dependencies + dependencyCount);
	execOrder.push_back(dependencies[0].value);
	*sequence = ++chan->submitted;
	if (onExec)
		onExec();
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
	if (onTokenRead)
		onTokenRead(*token);
	std::lock_guard<std::mutex> guard(stateMutex);
	return statuses[key(*token)];
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token,
							int externalVisibility)
{
	EXPECT_EQ(externalVisibility, 0);
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (token->engine != SWITCH_GPU_ENGINE_GM20B &&
		    token->engine != SWITCH_GPU_ENGINE_PROMISE)
			++physicalStatusNonGpuCalls;
		if (const auto at = physicalStatuses.find(key(*token));
		    at != physicalStatuses.end())
			return at->second;
	}
	return switch_gpu_backing_token_status(token);
}

// switch_gpu_bridge_why's: the first token of `causes` with a cause to name
std::map<Key, int> causes;

extern "C" int switch_gpu_backing_tokens_failure(const switch_gpu_backing_token *tokens,
						 uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
		if (const auto at = causes.find(key(tokens[i])); at != causes.end())
			return at->second;
	return 0;
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> guard(stateMutex);
	const auto at = external.find(key(*token));
	if (at != external.end())
		return at->second;
	const auto st = statuses.find(key(*token));
	return st == statuses.end() ? 0 : st->second;
}

extern "C" int switch_gpu_timeline_promote(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> guard(stateMutex);
	const auto st = statuses.find(key(*token));
	const int ready = st == statuses.end() ? 0 : st->second;
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
	// A failed gateOpen() also discards the space and leaves no published gate; reopening initializes the gate open.
	gateVaFailures = 1;
	gateWord = 0xdeadbeefu;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), -12);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(poolFrees, 2u);
	EXPECT_EQ(memoryCount, 0u);
	switch_gpu_bridge_gate_close();
	EXPECT_EQ(loadGate(), 0xdeadbeefu) << "gate not published yet";
	ASSERT_NE(submit({source}), nullptr);
	EXPECT_EQ(loadGate(), 0u) << "initialised open";
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
	EXPECT_EQ(submittedWords[0], (1u << 29) | (4u << 16) | (0x10u >> 2))
		<< "SEMAPHOREA..D: the gate";
	EXPECT_EQ(submittedWords[1], uint32_t(kGateVa >> 32));
	EXPECT_EQ(submittedWords[2], uint32_t(kGateVa));
	EXPECT_EQ(submittedWords[3], 0u) << "open";
	EXPECT_EQ(submittedWords[4], 1u | (1u << 12)) << "ACQUIRE, switching away while it waits";
	EXPECT_EQ(submittedWords[kGateDwords], tgm_syncpt_header(0x78));
	EXPECT_EQ(submittedWords[kGateDwords + 14], tgm_syncpt_header(0x74));
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

TEST_F(Bridge, AWaitThatCompletesBeforeItsCheckLeavesOneRunThatChecksNothing)
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
	EXPECT_NE(hangCheck()->deadline, 0u) << "not cancelled when the last waiter goes";
	fireHangCheck(switch_bridge_now + 1000000000);
	EXPECT_EQ(hangCheck()->deadline, 0u) << "its one run finds nothing waited";
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
	fireHangCheck(switch_bridge_now + 1000000000);
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
	const uint32_t *begin = submittedWords, *end = begin + kSlotDwords;
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
	fireHangCheck(switch_bridge_now + 1000000000);
	EXPECT_EQ(hangCheck()->deadline, 0u) << "its one run finds nothing waited";
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

// A submission sleeping for ring room holds the queue's turn, not its lock
class BridgeRoom : public Bridge
{
protected:
	std::promise<void> waiting, room;

	std::future<switch_gpu_bridge *> submitWaitingForRoom(switch_gpu_backing_token source)
	{
		nextExecError = -11;
		roomWait = [this] {
			waiting.set_value();
			room.get_future().wait();
			return 0;
		};
		auto submitted = std::async(std::launch::async, [source] {
			switch_gpu_bridge *bridge = nullptr;
			EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), 0);
			return bridge;
		});
		waiting.get_future().wait();
		return submitted;
	}

	void TearDown() override
	{
		roomWait = nullptr;
		Bridge::TearDown();
	}
};

TEST_F(BridgeRoom, TheQueueLockIsFreeWhileASubmissionWaitsForRoom)
{
	using namespace std::chrono_literals;
	auto first = token(170), second = token(171);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	auto *earlier = submit({first});
	ASSERT_NE(earlier, nullptr);
	auto waiter = submitWaitingForRoom(second);
	auto status = std::async(std::launch::async, [earlier] {
		return switch_gpu_bridge_status(earlier);
	});
	// base 1: a status read waited behind the room wait
	const int statusWaited = status.wait_for(500ms) == std::future_status::timeout ? 1 : 0;
	room.set_value();
	EXPECT_EQ(statusWaited, 0);
	EXPECT_EQ(status.get(), 0);
	auto *later = waiter.get();
	ASSERT_NE(later, nullptr);
	held.push_back(later);
	EXPECT_EQ(roomWaits, 1u);
}

TEST_F(BridgeRoom, ASubmissionThatArrivesDuringARoomWaitGoesAfterIt)
{
	using namespace std::chrono_literals;
	auto first = token(172), second = token(173);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	auto waiter = submitWaitingForRoom(first);
	auto behind = std::async(std::launch::async, [second] {
		switch_gpu_bridge *bridge = nullptr;
		EXPECT_EQ(switch_gpu_bridge_submit(&second, 1, &bridge), 0);
		return bridge;
	});
	EXPECT_EQ(behind.wait_for(100ms), std::future_status::timeout);
	room.set_value();
	auto *earlier = waiter.get(), *later = behind.get();
	ASSERT_NE(earlier, nullptr);
	ASSERT_NE(later, nullptr);
	held.push_back(earlier);
	held.push_back(later);
	ASSERT_GE(execOrder.size(), 2u);
	EXPECT_EQ(execOrder[execOrder.size() - 2], first.value);
	EXPECT_EQ(execOrder.back(), second.value);
}

TEST_F(Bridge, OriginalProducerFailureIsVisibleBeforeHardwareOrRetirementCanReleaseTheBridge)
{
	const auto source = token(72);
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	uint32_t syncpoint = 0, threshold = 0;
	switch_gpu_bridge_syncpoint(bridge, &syncpoint, &threshold);
	ASSERT_LT(syncpoints[syncpoint], threshold);
	const auto releasedBefore = failureReleases;
	EXPECT_EQ(switch_gpu_bridge_source_failure(bridge), 0);
	statuses[key(source)] = -5;
	EXPECT_EQ(switch_gpu_bridge_source_failure(bridge), -5);
	EXPECT_EQ(refs[key(source)], 1);
	EXPECT_LT(syncpoints[syncpoint], threshold);
	EXPECT_EQ(failureReleases, releasedBefore);
	EXPECT_LT(gpu.completed, gpu.submitted);
}

TEST_F(Bridge, SourceInspectionDoesNotEnterAnotherHostEngine)
{
	auto source = token(73, 162);
	source.engine = SWITCH_GPU_ENGINE_NVDEC;
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = -5;
	const uint32_t before = physicalStatusNonGpuCalls;
	EXPECT_EQ(switch_gpu_bridge_source_failure(bridge), 0);
	EXPECT_EQ(physicalStatusNonGpuCalls, before);
	EXPECT_EQ(refs[key(source)], 1);
}

TEST_F(Bridge, AbandonedPromiseIsVisibleBeforeTheBridgeHardwarePointMoves)
{
	const switch_gpu_backing_token source = {7, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
	statuses[key(source)] = 0;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	uint32_t syncpoint = 0, threshold = 0;
	switch_gpu_bridge_syncpoint(bridge, &syncpoint, &threshold);
	const auto reached = syncpoints[syncpoint];
	ASSERT_LT(reached, threshold);
	statuses[key(source)] = -5;
	EXPECT_EQ(switch_gpu_bridge_source_failure(bridge), -5);
	EXPECT_EQ(syncpoints[syncpoint], reached);
	EXPECT_EQ(refs[key(source)], 1);
}

// host1x.md A2: a token inside a job its producer drained and counted on its own syncpoint needs no GPU job of the
// bridge's. The consumer waits on the producer's syncpoint, the producer's channel is asked to announce that job, and
// the bridge retires when that job (not the token, which can complete with its writes still in the L2) has drained.
TEST_F(Bridge, AJoblessBridgeRetiresOnItsCoveringJobNotItsToken)
{
	const auto source = token(300, 7);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint, 14, 302};
	const uint32_t execsBefore = execs;
	needed.clear();
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(execs, execsBefore);
	ASSERT_EQ(needed.size(), 1u);
	EXPECT_EQ(needed[0].value, 302u) << "the channel announces the covering job";
	uint32_t id = 0;
	uint32_t threshold = 0;
	switch_gpu_bridge_syncpoint(bridge, &id, &threshold);
	EXPECT_EQ(id, kProducerSyncpoint);
	EXPECT_EQ(threshold, 14u);
	std::vector<int> heard;
	const auto record = [](void *context, int status) {
		static_cast<std::vector<int> *>(context)->push_back(status);
	};
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, record, &heard), 0);
	statuses[key(source)] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_TRUE(heard.empty()) << "the token completed, its covering job has not drained";
	EXPECT_EQ(external.count(key(source)), 0u) << "not promoted visible";
	coveringStatus[{7, 1, 302}] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(heard, std::vector<int>{0});
	EXPECT_EQ(external[key(source)], 1);
	fireHangCheck(switch_bridge_now + 1000000000);
	EXPECT_EQ(hangCheck()->deadline, 0u);
	release(bridge);
	EXPECT_EQ(refs[key(source)], 0);
}

// The producer's channel stopped short of the covering job: its increments never come, so the CPU makes them, as it
// does for a failed bridge job's own syncpoint, and the consumer hears the failure.
TEST_F(Bridge, AProducerThatStoppedShortHasTheCpuMakeTheIncrementsItOwed)
{
	const auto source = token(301, 7);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 1, 20, 301};
	syncpoints[kProducerSyncpoint + 1] = 16;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = -5;
	coveringStatus[{7, 1, 301}] = -5;
	stoppedChannels.insert(7);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(7);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	EXPECT_EQ(failureReleases, releasesBefore + 1);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 1], 20u);
	release(bridge);
}

// gpu.x1.bridge_jobless counts its own chain: a host1x consumer's jobless bridge is noted on the producing channel; the
// display's fence and a CPU wait on the same covered token are not, nor is a consumer that took a GPU job.
TEST_F(Bridge, OnlyAHost1xConsumersJoblessBridgeIsCountedOnItsProducingChannel)
{
	const auto source = token(322, 13);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 12, 60, 322};
	notedJobless.clear();
	const auto fenced = [](void *, int) {};
	ASSERT_EQ(switch_gpu_bridge_fence(&source, 1, fenced, nullptr), 0);
	EXPECT_TRUE(notedJobless.empty()) << "the display's fence";
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(notedJobless, std::vector<uint32_t>{13});
	const auto plain = token(323, 14);
	statuses[key(plain)] = 0;
	auto *job = submit({plain});
	ASSERT_NE(job, nullptr);
	EXPECT_EQ(notedJobless, std::vector<uint32_t>{13}) << "a bridge with a GPU job of its own";
	statuses[key(source)] = 1;
	statuses[key(plain)] = 1;
	coveringStatus[{13, 1, 322}] = 1;
	finish();
	release(bridge);
	release(job);
}

// The display takes its stream route only for a jobless bridge, whose every failure short of DRAM cancels the stream
// first; a bridge with its own GPU job goes through the notify fence.
TEST_F(Bridge, OnlyAJoblessBridgeAnswersJobless)
{
	const auto source = token(340, 16);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 14, 80, 340};
	const auto plain = token(341, 17);
	statuses[key(plain)] = 0;
	auto *jobless = submit({source});
	auto *job = submit({plain});
	ASSERT_NE(jobless, nullptr);
	ASSERT_NE(job, nullptr);
	EXPECT_EQ(switch_gpu_bridge_jobless(jobless), 1);
	EXPECT_EQ(switch_gpu_bridge_jobless(job), 0);
	EXPECT_EQ(switch_gpu_bridge_jobless(nullptr), 0);
	statuses[key(source)] = 1;
	statuses[key(plain)] = 1;
	coveringStatus[{16, 1, 340}] = 1;
	finish();
	release(jobless);
	release(job);
}

// Two jobless bridges on one producer that stopped short: every failure of the pass is stored before either release,
// since a release cancels the streams of both and a stream queued after it must find its own bridge failed. Before
// that, a bridge reads not failed.
TEST_F(Bridge, AStoppedProducersBridgesAllReadFailedBeforeAnyRelease)
{
	const auto first = token(350, 18);
	const auto second = token(351, 18);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	covered[key(first)] = {kProducerSyncpoint + 15, 90, 350};
	covered[key(second)] = {kProducerSyncpoint + 15, 92, 351};
	syncpoints[kProducerSyncpoint + 15] = 88;
	auto *a = submit({first});
	auto *b = submit({second});
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(switch_gpu_bridge_failed(a), 0);
	EXPECT_EQ(switch_gpu_bridge_failed(b), 0);
	std::vector<std::pair<int, int>> seen;
	onReleaseFailed = [&] {
		seen.emplace_back(switch_gpu_bridge_failed(a), switch_gpu_bridge_failed(b));
	};
	statuses[key(first)] = -5;
	statuses[key(second)] = -5;
	coveringStatus[{18, 1, 350}] = -5;
	coveringStatus[{18, 1, 351}] = -5;
	stoppedChannels.insert(18);
	switch_gpu_bridge_completed();
	pump();
	onReleaseFailed = nullptr;
	stoppedChannels.erase(18);
	ASSERT_FALSE(seen.empty());
	for (const auto &[x, y] : seen) {
		EXPECT_LT(x, 0);
		EXPECT_LT(y, 0);
	}
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 15], 92u);
	release(a);
	release(b);
}

// A host1x stream that waits on a bridge in hardware with no completion callback (the display's plane stream, NVENC's
// direct input) watches it: from then until it retires the hang check covers its producer as a waiter's, the bridge
// channel at a bridge job and the producer's channel at a jobless one's covering job. A submit alone arms nothing.
TEST_F(Bridge, AWatchedBridgeIsHangCheckedWithNoCallback)
{
	const auto stalled = token(330, 3);
	statuses[key(stalled)] = 0;
	const auto source = token(331, 15);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 13, 70, 331};
	switch_bridge_now = 5000;
	auto *jobless = submit({source});
	ASSERT_NE(jobless, nullptr);
	EXPECT_EQ(hangCheck()->deadline, 0u) << "a submit alone";
	progressChecks = 0;
	producerChecks.clear();
	auto *job = submitWatched(stalled);
	ASSERT_NE(job, nullptr);
	EXPECT_EQ(switch_gpu_bridge_watch(jobless), 0);
	EXPECT_EQ(hangCheck()->deadline, 5000u + 1000000000u);
	fireHangCheck(1000005000);
	EXPECT_EQ(progressChecks.load(), 1u) << "the bridge channel, at the watched bridge job";
	ASSERT_EQ(producerChecks.size(), 1u);
	EXPECT_EQ(producerChecks[0], std::make_pair(15u, 331u))
		<< "its channel, at the covering job";
	statuses[key(stalled)] = 1;
	statuses[key(source)] = 1;
	coveringStatus[{15, 1, 331}] = 1;
	finish();
	const auto checks = progressChecks.load();
	fireHangCheck(switch_bridge_now + 1000000000);
	EXPECT_EQ(hangCheck()->deadline, 0u) << "nothing watched is left";
	EXPECT_EQ(progressChecks.load(), checks);
	release(job);
	release(jobless);
}

// A stream of watched frames (one bridge each, retiring well inside the hang bound) pays no per-frame timer work: the
// first watch arms the check, the frames after it find it armed, and no retirement cancels it.
TEST_F(Bridge, AWatchedFrameStreamArmsTheHangCheckOnceAndNeverCancelsIt)
{
	const unsigned arms = switch_bridge_arms, cancels = switch_bridge_cancels;
	for (uint32_t frame = 0; frame < 4; ++frame) {
		const auto source = token(390 + frame, 29);
		statuses[key(source)] = 0;
		auto *bridge = submitWatched(source);
		ASSERT_NE(bridge, nullptr);
		statuses[key(source)] = 1;
		releaseSyncpoint(bridge);
		finish();
		release(bridge);
	}
	EXPECT_EQ(switch_bridge_arms - arms, 1u);
	EXPECT_EQ(switch_bridge_cancels, cancels);
}

// Every token completed at its own fence inside the covering job, and the channel was killed later in that job. A
// token's own fence flushes no L2, so the writes reached DRAM only if the job's drain ran, which its increments follow.
// Killed before them: the bridge fails, the CPU makes the increments after cancelling the consumers, and why names the
// covering job's cause.
TEST_F(Bridge, ACoveringJobKilledBeforeItsIncrementsFailsTheBridgeWithItsCause)
{
	const auto source = token(370, 23);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 16, 100, 371};
	syncpoints[kProducerSyncpoint + 16] = 98;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = 1;
	coveringStatus[{23, 1, 371}] = -5;
	causes[key(token(371, 23))] = SWITCH_GPU_BACKING_WRITER_GONE;
	stoppedChannels.insert(23);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(23);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	EXPECT_EQ(failureReleases, releasesBefore + 1);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 16], 100u);
	EXPECT_EQ(switch_gpu_bridge_why(bridge), SWITCH_GPU_BACKING_WRITER_GONE);
	causes.clear();
	release(bridge);
}

// The same kill after the covering job's increments: its drain ran, the writes are in DRAM, and the bridge succeeds
// with no CPU increment, so the frame shows.
TEST_F(Bridge, ACoveringJobKilledAfterItsIncrementsLeavesTheBridgeComplete)
{
	const auto source = token(372, 24);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 17, 110, 373};
	syncpoints[kProducerSyncpoint + 17] = 108;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = 1;
	syncpoints[kProducerSyncpoint + 17] = 110;
	coveringStatus[{24, 1, 373}] = -5;
	causes[key(token(373, 24))] = SWITCH_GPU_BACKING_WRITER_GONE;
	stoppedChannels.insert(24);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(24);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 1);
	EXPECT_EQ(failureReleases, releasesBefore);
	EXPECT_EQ(switch_gpu_bridge_why(bridge), 0);
	causes.clear();
	release(bridge);
}

// `source` completed at its own fence inside the job ending at `last` on its channel, and that channel was then killed
void killCoveringJob(const switch_gpu_backing_token &source, uint32_t last)
{
	statuses[key(source)] = 1;
	coveringStatus[{source.queue, source.reset_generation, last}] = -5;
	causes[key(token(last, source.queue))] = SWITCH_GPU_BACKING_WRITER_GONE;
	stoppedChannels.insert(source.queue);
}

const auto recordStatus = [](void *context, int status) {
	static_cast<std::vector<int> *>(context)->push_back(status);
};

// The display reads a bare -5 as its frame lost (TEGRA_SCANOUT_SOURCE_LOST) and panics. A notify or fence callback on a
// slot whose tokens completed and whose covering job died short of its increments hears that job's cause.
TEST_F(Bridge, ANotifiedConsumerOfACoveringJobKilledBeforeItsIncrementsHearsItsCause)
{
	const auto source = token(610, 30);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 20, 150, 611};
	syncpoints[kProducerSyncpoint + 20] = 148;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	std::vector<int> heard;
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, recordStatus, &heard), 0);
	killCoveringJob(source, 611);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(30);
	causes.clear();
	EXPECT_EQ(heard, std::vector<int>{SWITCH_GPU_BACKING_WRITER_GONE});
	EXPECT_EQ(switch_gpu_bridge_failed(bridge), -5);
	release(bridge);
}

// A consumer that read the stored failure and refused the stream notifies after the slot retired
TEST_F(Bridge, ANotifyAfterACoveringJobKilledBeforeItsIncrementsHearsItsCause)
{
	const auto source = token(612, 31);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 21, 160, 613};
	syncpoints[kProducerSyncpoint + 21] = 158;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	killCoveringJob(source, 613);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(31);
	ASSERT_EQ(switch_gpu_bridge_failed(bridge), -5);
	std::vector<int> heard;
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, recordStatus, &heard), 0);
	pump();
	causes.clear();
	EXPECT_EQ(heard, std::vector<int>{SWITCH_GPU_BACKING_WRITER_GONE});
	release(bridge);
}

TEST_F(Bridge, AFenceOnACoveringJobKilledBeforeItsIncrementsHearsItsCause)
{
	const auto source = token(614, 32);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 22, 170, 615};
	syncpoints[kProducerSyncpoint + 22] = 168;
	const uint32_t releasesBefore = failureReleases;
	std::vector<int> heard;
	ASSERT_EQ(switch_gpu_bridge_fence(&source, 1, recordStatus, &heard), 0);
	killCoveringJob(source, 615);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(32);
	causes.clear();
	EXPECT_EQ(failureReleases, releasesBefore + 1) << "the fence's slot was jobless";
	EXPECT_EQ(heard, std::vector<int>{SWITCH_GPU_BACKING_WRITER_GONE});
}

// A stream that passed its WAIT on the covering job's increments reads its bridge before retirement stores a result.
// The writes are in DRAM, so status answers 1 as retirement will, not the covering job's -5.
TEST_F(Bridge, ACoveringJobKilledAfterItsIncrementsReadsCompleteBeforeRetirement)
{
	const auto source = token(616, 33);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 23, 180, 617};
	syncpoints[kProducerSyncpoint + 23] = 178;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	syncpoints[kProducerSyncpoint + 23] = 180;
	killCoveringJob(source, 617);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 1);
	EXPECT_EQ(switch_gpu_bridge_failed(bridge), 0);
	stoppedChannels.erase(33);
	causes.clear();
	release(bridge);
}

// The increments and the kill can both land between status's reads. The syncpoint is read after the covering job, as
// settleJobless reads it, so a job seen dead is judged by the increments it made before it died.
TEST_F(Bridge, StatusReadsTheProducerSyncpointAfterTheCoveringJob)
{
	const auto source = token(618, 34);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 24, 190, 619};
	syncpoints[kProducerSyncpoint + 24] = 188;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = 1;
	onCoveredRead = [&] {
		syncpoints[kProducerSyncpoint + 24] = 190;
		killCoveringJob(source, 619);
	};
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 1);
	onCoveredRead = nullptr;
	stoppedChannels.erase(34);
	causes.clear();
	release(bridge);
}

// A kill landing between the slot's reads fails the token and stops the channel at once. The tokens are read first, so
// a failure they show comes with the channel's end, and the CPU makes the increments the channel never will.
TEST_F(Bridge, AKillBetweenTheTokenAndTheChannelReadsStillHasTheCpuMakeTheIncrements)
{
	const auto source = token(320, 7);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 10, 40, 320};
	syncpoints[kProducerSyncpoint + 10] = 36;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	onTokenRead = [&](const switch_gpu_backing_token &) {
		statuses[key(source)] = -5;
		coveringStatus[{7, 1, 320}] = -5;
		stoppedChannels.insert(7);
	};
	switch_gpu_bridge_completed();
	pump();
	onTokenRead = nullptr;
	stoppedChannels.erase(7);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	EXPECT_EQ(failureReleases, releasesBefore + 1);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 10], 40u);
	release(bridge);
}

// A covering job's increments go ahead of its fence (gpu_exec.c chan_fence_job), so once that fence landed a channel
// that then stopped owes none: making them on the CPU would cancel a consumer that reads good data.
TEST_F(Bridge, AStoppedProducerWhoseCoveringFenceLandedOwesNoIncrements)
{
	const auto source = token(321, 7);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 11, 50, 321};
	syncpoints[kProducerSyncpoint + 11] = 48;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	statuses[key(source)] = 1;
	coveringStatus[{7, 1, 321}] = 1;
	stoppedChannels.insert(7);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(7);
	EXPECT_EQ(switch_gpu_bridge_status(bridge), 1);
	EXPECT_EQ(failureReleases, releasesBefore);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 11], 48u);
	release(bridge);
}

// A token of another channel that read complete at the submit and fails later (a GR fault's job) fails the consumer,
// but the producer is still running: its syncpoint is its own increments' alone.
TEST_F(Bridge, AnotherTokensFailureLeavesARunningProducersSyncpointAlone)
{
	const auto source = token(302, 7);
	const auto other = token(50, 11);
	statuses[key(source)] = 0;
	statuses[key(other)] = 1;
	covered[key(source)] = {kProducerSyncpoint + 4, 30, 302};
	syncpoints[kProducerSyncpoint + 4] = 28;
	const uint32_t releasesBefore = failureReleases;
	const uint32_t execsBefore = execs;
	auto *bridge = submit({other, source});
	ASSERT_NE(bridge, nullptr);
	EXPECT_EQ(execs, execsBefore);
	statuses[key(other)] = -5;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	EXPECT_EQ(failureReleases, releasesBefore);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 4], 28u);
	coveringStatus[{7, 1, 302}] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(failureReleases, releasesBefore) << "its covering job drained";
	release(bridge);
}

// The same failure elsewhere, then the producer stops short of its covering job: the slot failed at once and stayed for
// its producer, so its consumers are cancelled and the CPU makes the increments, as for any slot on a stopped producer.
TEST_F(Bridge, ASlotFailedElsewhereStaysForItsProducerAndIsReleasedWhenItStopsShort)
{
	const auto other = token(380, 26);
	const auto source = token(381, 27);
	statuses[key(other)] = 1;
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 18, 120, 381};
	syncpoints[kProducerSyncpoint + 18] = 118;
	const uint32_t releasesBefore = failureReleases;
	auto *bridge = submit({other, source});
	ASSERT_NE(bridge, nullptr);
	ASSERT_EQ(switch_gpu_bridge_jobless(bridge), 1);
	statuses[key(other)] = -5;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(switch_gpu_bridge_failed(bridge), -5);
	EXPECT_EQ(failureReleases, releasesBefore) << "its producer still runs";
	statuses[key(source)] = -5;
	coveringStatus[{27, 1, 381}] = -5;
	stoppedChannels.insert(27);
	switch_gpu_bridge_completed();
	pump();
	stoppedChannels.erase(27);
	EXPECT_EQ(failureReleases, releasesBefore + 1);
	EXPECT_EQ(syncpoints[kProducerSyncpoint + 18], 120u);
	release(bridge);
}

// A kill landing between two jobless slots' reads in one pass: the first read its producer running, the second its stop.
// The first reads it again before the release passes its threshold, so a stream queued after that release finds it failed.
TEST_F(Bridge, AKillBetweenTwoSlotsReadsFailsBothBeforeTheRelease)
{
	const auto first = token(382, 28);
	const auto second = token(383, 28);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	covered[key(first)] = {kProducerSyncpoint + 19, 130, 382};
	covered[key(second)] = {kProducerSyncpoint + 19, 132, 383};
	syncpoints[kProducerSyncpoint + 19] = 128;
	auto *a = submit({first});
	auto *b = submit({second});
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	onTokenRead = [&](const switch_gpu_backing_token &read) {
		if (key(read) != key(second))
			return;
		onTokenRead = nullptr;
		statuses[key(first)] = -5;
		statuses[key(second)] = -5;
		coveringStatus[{28, 1, 382}] = -5;
		coveringStatus[{28, 1, 383}] = -5;
		stoppedChannels.insert(28);
	};
	std::vector<int> firstAtRelease;
	onReleaseFailed = [&] {
		firstAtRelease.push_back(switch_gpu_bridge_failed(a));
	};
	switch_gpu_bridge_completed();
	pump();
	onTokenRead = nullptr;
	onReleaseFailed = nullptr;
	stoppedChannels.erase(28);
	ASSERT_FALSE(firstAtRelease.empty());
	for (const int failed : firstAtRelease)
		EXPECT_LT(failed, 0) << "the first slot read failed at every release";
	EXPECT_EQ(switch_gpu_bridge_status(a), -5);
	EXPECT_EQ(switch_gpu_bridge_status(b), -5);
	release(a);
	release(b);
}

// A waiter on a jobless bridge waits on the producer's covering job, so the hang rule checks that channel at that job;
// an older waited bridge job whose channel is idle does not hide it.
TEST_F(Bridge, EveryWaitedJoblessBridgeHasItsProducerCheckedBesideAnIdleBridgeJob)
{
	const auto older = token(303, 3);
	statuses[key(older)] = 0;
	auto *job = submit({older});
	ASSERT_NE(job, nullptr);
	finish();
	const auto source = token(304, 9);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 2, 6, 305};
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	progressChecks = 0;
	producerChecks.clear();
	switch_bridge_now = 5000;
	auto olderWaiter = waitArmed(job);
	std::vector<int> heard;
	const auto record = [](void *context, int status) {
		static_cast<std::vector<int> *>(context)->push_back(status);
	};
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, record, &heard), 0);
	fireHangCheck(1000005000);
	ASSERT_EQ(producerChecks.size(), 1u);
	EXPECT_EQ(producerChecks[0], std::make_pair(9u, 305u))
		<< "its channel, at the covering job";
	EXPECT_NE(hangCheck()->deadline, 0u);
	statuses[key(older)] = 1;
	statuses[key(source)] = 1;
	coveringStatus[{9, 1, 305}] = 1;
	switch_gpu_bridge_completed();
	pump();
	EXPECT_EQ(olderWaiter.get(), 1);
	EXPECT_EQ(heard, std::vector<int>{0});
	release(job);
	release(bridge);
}

// A failed bridge channel is replaced once no bridge job holds a slot: a jobless slot never used it
TEST_F(Bridge, AFailedBridgeChannelReopensWhileJoblessSlotsAreHeld)
{
	const auto source = token(310, 7);
	statuses[key(source)] = 0;
	covered[key(source)] = {kProducerSyncpoint + 5, 8, 310};
	auto *jobless = submit({source});
	ASSERT_NE(jobless, nullptr);
	const auto lost = token(311, 12);
	statuses[key(lost)] = 0;
	auto *job = submit({lost});
	ASSERT_NE(job, nullptr);
	statuses[key(lost)] = -5;
	finish(true);
	release(job);
	const auto next = token(312, 13);
	statuses[key(next)] = 1;
	const uint32_t execsBefore = execs;
	EXPECT_NE(submit({next}), nullptr) << "not -16 behind the jobless slot";
	EXPECT_EQ(execs, execsBefore + 1);
	statuses[key(source)] = 1;
	coveringStatus[{7, 1, 310}] = 1;
	release(jobless);
}

// One syncpoint carries one channel's order; a token none covers yet needs the bridge's own flush and increment.
TEST_F(Bridge, TokensOfTwoChannelsOrOneNoSyncpointCoversKeepTheBridgeJob)
{
	const auto first = token(306, 7);
	const auto second = token(307, 8);
	const auto uncovered = token(308, 7);
	covered[key(first)] = {kProducerSyncpoint, 30, 306};
	covered[key(second)] = {kProducerSyncpoint + 3, 2, 307};
	const uint32_t before = execs;
	ASSERT_NE(submit({first, second}), nullptr);
	EXPECT_EQ(execs, before + 1);
	ASSERT_NE(submit({first, uncovered}), nullptr);
	EXPECT_EQ(execs, before + 2);
}

// A host1x consumer's first ask on a channel gives it a syncpoint while the GPU's channels hold fewer than their share;
// one a gone timeline handed back first gets the increments its channel never ran. A CPU wait or the display's fence
// waits on no syncpoint and gives none.
TEST_F(Bridge, OnlyAHost1xConsumersFirstAskGivesAChannelASyncpointWithinTheGpusShare)
{
	const auto once = [&](switch_gpu_backing_token source) {
		auto *bridge = submit({source});
		ASSERT_NE(bridge, nullptr);
		statuses[key(source)] = 1;
		finish();
		release(bridge);
	};
	unassigned.insert(450);
	statuses[key(token(450, 450))] = 0;
	const auto fenced = [](void *, int) {};
	ASSERT_EQ(switch_gpu_bridge_fence(std::array{token(450, 450)}.data(), 1, fenced, nullptr),
		  0);
	statuses[key(token(450, 450))] = 1;
	finish();
	EXPECT_TRUE(adopted.empty()) << "the display's fence";
	unassigned.erase(450);
	for (uint32_t channel = 400; adopted.size() < SWITCH_GPU_TIMELINE_SYNCPTS; ++channel) {
		ASSERT_LT(channel, 400u + SWITCH_GPU_TIMELINE_SYNCPTS);
		unassigned.insert(channel);
		once(token(channel, channel));
		EXPECT_EQ(adopted.back().second, syncpoints[adopted.back().first]);
	}
	unassigned.insert(500);
	once(token(500, 500));
	EXPECT_EQ(adopted.size(), SWITCH_GPU_TIMELINE_SYNCPTS);
	const uint32_t handedBack = adopted.front().first;
	returned.emplace_back(handedBack, syncpoints[handedBack] + 4);
	const uint32_t promised = syncpoints[handedBack] + 4;
	once(token(501, 500));
	ASSERT_EQ(adopted.size(), SWITCH_GPU_TIMELINE_SYNCPTS + 1);
	EXPECT_EQ(adopted.back().first, handedBack);
	EXPECT_EQ(adopted.back().second, promised)
		<< "its base, after the increments its last channel never ran";
}

TEST_F(Bridge, AFailedProducersBridgeJobHasItsConsumersCancelledAtTheGateWithNoIncrement)
{
	const auto source = token(360, 19);
	const auto healthy = token(361, 20);
	statuses[key(source)] = 0;
	statuses[key(healthy)] = 0;
	auto *failing = submit({source});
	auto *fine = submit({healthy});
	ASSERT_NE(failing, nullptr);
	ASSERT_NE(fine, nullptr);
	ASSERT_EQ(switch_gpu_bridge_jobless(failing), 0);
	uint32_t id = 0;
	uint32_t threshold = 0;
	switch_gpu_bridge_syncpoint(failing, &id, &threshold);
	const uint32_t before = syncpoints[id];
	const uint32_t releasesBefore = failureReleases;
	cancelledWaiters.clear();
	statuses[key(source)] = -5;
	switch_gpu_bridge_gate_close();
	EXPECT_EQ(loadGate(), 1u) << "closed before the producer's word is raised";
	runGatePass();
	EXPECT_EQ(cancelledWaiters, (std::vector<std::pair<uint32_t, uint32_t>>{{id, threshold}}));
	EXPECT_EQ(switch_gpu_bridge_failed(failing), -5);
	EXPECT_EQ(switch_gpu_bridge_failed(fine), 0);
	EXPECT_EQ(failureReleases, releasesBefore);
	EXPECT_EQ(syncpoints[id], before) << "no CPU increment";
	EXPECT_EQ(loadGate(), 0u) << "reopened";
	statuses[key(healthy)] = 1;
	finish();
	EXPECT_EQ(switch_gpu_bridge_status(failing), -5);
	release(failing);
	release(fine);
}

TEST_F(Bridge, ACloseDuringTheGatesPassKeepsItClosedForAnotherPass)
{
	const auto first = token(362, 21);
	const auto second = token(363, 22);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	auto *a = submit({first});
	auto *b = submit({second});
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	cancelledWaiters.clear();
	statuses[key(first)] = -5;
	switch_gpu_bridge_gate_close();
	onCancelWaiters = [&](uint32_t, uint32_t) {
		onCancelWaiters = nullptr;
		statuses[key(second)] = -5;
		switch_gpu_bridge_gate_close();
	};
	runGatePass();
	EXPECT_EQ(loadGate(), 1u) << "a close the pass did not see";
	EXPECT_TRUE(gatePass()->pending.load());
	runGatePass();
	EXPECT_EQ(loadGate(), 0u);
	EXPECT_EQ(cancelledWaiters.size(), 2u);
	EXPECT_EQ(switch_gpu_bridge_failed(a), -5);
	EXPECT_EQ(switch_gpu_bridge_failed(b), -5);
	finish();
	release(a);
	release(b);
}

// VIC and NVDEC publish physical cancellation before retirement sets status: gatePass and submit must both see it.
TEST_F(Bridge, ACancelledHost1xProducerIsDetectedPhysicallyAtTheGateAndRefusedOnSubmit)
{
	auto vic = token(364, 165);
	vic.engine = SWITCH_GPU_ENGINE_VIC;
	statuses[key(vic)] = 0;
	physicalStatuses[key(vic)] = 0;
	auto *bridge = submit({vic});
	ASSERT_NE(bridge, nullptr);
	uint32_t id = 0, threshold = 0;
	switch_gpu_bridge_syncpoint(bridge, &id, &threshold);
	cancelledWaiters.clear();
	physicalStatuses[key(vic)] = -5;
	switch_gpu_bridge_gate_close();
	runGatePass();
	EXPECT_EQ(cancelledWaiters, (std::vector<std::pair<uint32_t, uint32_t>>{{id, threshold}}));
	EXPECT_EQ(switch_gpu_bridge_failed(bridge), -5);
	EXPECT_EQ(loadGate(), 0u);
	switch_gpu_bridge *refused = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&vic, 1, &refused), -5);
	EXPECT_EQ(refused, nullptr);
	finish();
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	release(bridge);
}

// Cancelling bridge A's waiters can physically fail a VIC/NVDEC job that active bridge B waits on: gatePass cancels
// bridge B too before reopening the gate.
TEST_F(Bridge, TransitiveHost1xCancellationAtTheGateCancelsChainedBridgeBeforeReopening)
{
	const auto gpuSource = token(365, 23);
	auto vic = token(366, 166);
	vic.engine = SWITCH_GPU_ENGINE_VIC;
	statuses[key(gpuSource)] = 0;
	statuses[key(vic)] = 0;
	physicalStatuses[key(vic)] = 0;
	auto *a = submit({gpuSource});
	auto *b = submit({vic});
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	uint32_t idA = 0, thrA = 0, idB = 0, thrB = 0;
	switch_gpu_bridge_syncpoint(a, &idA, &thrA);
	switch_gpu_bridge_syncpoint(b, &idB, &thrB);
	cancelledWaiters.clear();
	onCancelWaiters = [&](uint32_t id, uint32_t) {
		EXPECT_EQ(loadGate(), 1u) << "gate stays closed across transitive cancels";
		if (id == idA)
			physicalStatuses[key(vic)] = -5;
	};
	statuses[key(gpuSource)] = -5;
	switch_gpu_bridge_gate_close();
	runGatePass();
	EXPECT_EQ(cancelledWaiters,
		  (std::vector<std::pair<uint32_t, uint32_t>>{{idA, thrA}, {idB, thrB}}));
	EXPECT_EQ(switch_gpu_bridge_failed(a), -5);
	EXPECT_EQ(switch_gpu_bridge_failed(b), -5);
	EXPECT_EQ(loadGate(), 0u);
	finish();
	release(a);
	release(b);
}

TEST_F(BridgeRoom, AProducerFailingDuringRoomWaitIsRefusedWithoutSubmitting)
{
	const auto source = token(367, 24);
	statuses[key(source)] = 0;
	nextExecError = -11;
	const uint32_t execsBefore = execs;
	roomWait = [this] {
		waiting.set_value();
		room.get_future().wait();
		return 0;
	};
	switch_gpu_bridge *bridge = nullptr;
	auto submitted = std::async(std::launch::async, [&] {
		return switch_gpu_bridge_submit(&source, 1, &bridge);
	});
	waiting.get_future().wait();
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		statuses[key(source)] = -5;
	}
	switch_gpu_bridge_gate_close();
	runGatePass();
	EXPECT_EQ(loadGate(), 0u);
	room.set_value();
	EXPECT_EQ(submitted.get(), -5);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(execs, execsBefore + 1u) << "no retry after producer failed during room wait";
	EXPECT_EQ(refs[key(source)], 0);
}

TEST_F(Bridge, AProducerFailingDuringExecAbandonsTheSlotWithoutHandingOutABridge)
{
	const auto source = token(368, 25);
	statuses[key(source)] = 0;
	onExec = [&] {
		onExec = nullptr;
		statuses[key(source)] = -5;
	};
	switch_gpu_bridge *bridge = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), -5);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(refs[key(source)], 1) << "retained until the kicked GPU job retires";
	finish();
	EXPECT_EQ(refs[key(source)], 0);
}

// A promise temporarily marked abandoned during RAISING (or later evicted from the abandoned ring) may read 1 by the
// time retire() runs: the failure already recorded by gatePass must stay failed.
TEST_F(Bridge, RetirementPreservesAFailureRecordedAtTheGateEvenIfTheProducerLaterReadsComplete)
{
	const switch_gpu_backing_token promise = {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2056};
	statuses[key(promise)] = 0;
	auto *bridge = submit({promise});
	ASSERT_NE(bridge, nullptr);
	std::vector<int> heard;
	ASSERT_EQ(switch_gpu_bridge_notify(bridge, recordStatus, &heard), 0);
	statuses[key(promise)] = -5;
	switch_gpu_bridge_gate_close();
	runGatePass();
	ASSERT_EQ(switch_gpu_bridge_failed(bridge), -5);
	statuses[key(promise)] = 1;
	releaseSyncpoint(bridge);
	finish();
	EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
	EXPECT_EQ(heard, std::vector<int>{-5});
	release(bridge);
}

TEST_F(Bridge, ReallocatedSlotSyncpointRewritesItsGateAndIncrementPush)
{
	const auto source = token(369, 26);
	statuses[key(source)] = 1;
	auto *bridge = submit({source});
	ASSERT_NE(bridge, nullptr);
	uint32_t oldId = 0, unused = 0;
	switch_gpu_bridge_syncpoint(bridge, &oldId, &unused);
	releaseFailures = 1;
	finish(true);
	release(bridge);
	auto *reused = submit({source});
	ASSERT_NE(reused, nullptr);
	uint32_t newId = 0;
	switch_gpu_bridge_syncpoint(reused, &newId, &unused);
	EXPECT_NE(newId, oldId);
	EXPECT_EQ(submittedWords[0], (1u << 29) | (4u << 16) | (0x10u >> 2));
	EXPECT_EQ(submittedWords[1], uint32_t(kGateVa >> 32));
	EXPECT_EQ(submittedWords[2], uint32_t(kGateVa));
	EXPECT_EQ(submittedWords[3], 0u);
	EXPECT_EQ(submittedWords[4], 1u | (1u << 12));
	EXPECT_EQ(submittedWords[kGateDwords + 13], ((newId & 0xffu) << 8) | 1u)
		<< "SYNCPOINTB carries the reallocated syncpoint";
	EXPECT_EQ(submittedWords[kGateDwords + 15], ((newId & 0xffu) << 8) | 1u);
	finish();
	release(reused);
}

TEST_F(Bridge, ConcurrentGateClosesPassesAndSubmitsAreRaceFree)
{
	std::atomic<bool> stopCloser{false}, stopPasser{false};
	std::thread passer([&] {
		while (!stopPasser.load(std::memory_order_acquire)) {
			if (!runGatePass())
				std::this_thread::yield();
		}
		while (runGatePass())
			;
	});
	std::thread closer([&] {
		while (!stopCloser.load(std::memory_order_acquire)) {
			switch_gpu_bridge_gate_close();
			std::this_thread::yield();
		}
	});
	for (uint32_t round = 0; round < 120; ++round) {
		const auto src = token(700 + round, 40);
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			statuses[key(src)] = 0;
		}
		switch_gpu_bridge *bridge = nullptr;
		ASSERT_EQ(switch_gpu_bridge_submit(&src, 1, &bridge), 0);
		ASSERT_NE(bridge, nullptr);
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			statuses[key(src)] = -5;
		}
		switch_gpu_bridge_gate_close();
		while (switch_gpu_bridge_failed(bridge) == 0)
			std::this_thread::yield();
		EXPECT_EQ(switch_gpu_bridge_failed(bridge), -5);
		finish();
		EXPECT_EQ(switch_gpu_bridge_status(bridge), -5);
		switch_gpu_bridge_release(&bridge);
	}
	stopCloser.store(true, std::memory_order_release);
	closer.join();
	stopPasser.store(true, std::memory_order_release);
	passer.join();
	EXPECT_EQ(loadGate(), 0u);
}

TEST_F(Bridge, GatePassRunsWhileSubmitterWaitsInSubmitEnter)
{
	const auto first = token(750, 41);
	const auto second = token(751, 42);
	statuses[key(first)] = 0;
	statuses[key(second)] = 0;
	auto *a = submit({first});
	ASSERT_NE(a, nullptr);
	bool ran = false;
	onSubmitEnter = [&] {
		onSubmitEnter = nullptr;
		statuses[key(first)] = -5;
		switch_gpu_bridge_gate_close();
		EXPECT_TRUE(runGatePass());
		EXPECT_EQ(loadGate(), 0u);
		ran = true;
	};
	auto *b = submit({second});
	EXPECT_TRUE(ran);
	ASSERT_NE(b, nullptr);
	EXPECT_EQ(switch_gpu_bridge_failed(a), -5);
	EXPECT_EQ(switch_gpu_bridge_failed(b), 0);
	statuses[key(second)] = 1;
	finish();
	release(a);
	release(b);
}

TEST_F(Bridge, AProducerFailingDuringSubmitEnterIsRefusedWithoutSubmitting)
{
	const auto source = token(752, 43);
	statuses[key(source)] = 0;
	const uint32_t execsBefore = execs;
	onSubmitEnter = [&] {
		onSubmitEnter = nullptr;
		statuses[key(source)] = -5;
		switch_gpu_bridge_gate_close();
		runGatePass();
	};
	switch_gpu_bridge *bridge = nullptr;
	EXPECT_EQ(switch_gpu_bridge_submit(&source, 1, &bridge), -5);
	EXPECT_EQ(bridge, nullptr);
	EXPECT_EQ(execs, execsBefore);
	EXPECT_EQ(refs[key(source)], 0);
	EXPECT_EQ(loadGate(), 0u);
}

TEST_F(Bridge, GatePassRunsWhileChannelSlowPathReopensErroredChannel)
{
	const auto lost = token(753, 44);
	statuses[key(lost)] = 0;
	auto *old = submit({lost});
	ASSERT_NE(old, nullptr);
	statuses[key(lost)] = -5;
	finish(true);
	release(old);

	const auto next = token(754, 45);
	statuses[key(next)] = 1;
	bool ran = false;
	onOpenChannel = [&] {
		onOpenChannel = nullptr;
		switch_gpu_bridge_gate_close();
		EXPECT_TRUE(runGatePass());
		EXPECT_EQ(loadGate(), 0u);
		ran = true;
	};
	auto *reopened = submit({next});
	EXPECT_TRUE(ran);
	ASSERT_NE(reopened, nullptr);
	finish();
	release(reopened);
}
