// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuCpuComplete.h"
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = -22, kIOReturnNotOpen = -19,
	      kIOReturnNotReady = -11, kIOReturnNoMemory = -12;

struct tgm_chan {
	uint64_t uid;
	uint32_t submitted;
};

static uint64_t switch_gpu_chan_identity(tgm_chan *c)
{
	return c->uid;
}

static uint32_t tgm_chan_submitted(tgm_chan *c)
{
	return c->submitted;
}

static void switch_gpu_chan_token(tgm_chan *c, uint32_t value, switch_gpu_backing_token *out)
{
	*out = {value, (c->uid >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(c->uid)};
}

static int ioreturn(int result)
{
	return result;
}

static int copyin(uint64_t address, void *out, size_t bytes)
{
	if (address == 1)
		return -1;
	memcpy(out, reinterpret_cast<void *>(address), bytes);
	return 0;
}

class AGXDeviceUserClient
{
public:
	struct Bo {
		uint64_t handle, bytes;
		switch_gpu_backing *backing;
		bool chunked = false, nativeView = false, representation = false;
	};

	struct Gate {
		bool stopped = false;

		bool closed() const
		{
			return stopped;
		}
	} submitGate_;

	static constexpr uint32_t kMaxChannels = 2;
	tgm_chan *chans_[kMaxChannels] = {};
	IOLock *lock_ = IOLockAlloc();
	std::vector<Bo> bos;

	~AGXDeviceUserClient()
	{
		IOLockFree(lock_);
	}

	const Bo *findBo(uint64_t handle)
	{
		for (const auto &bo : bos)
			if (bo.handle == handle)
				return &bo;
		return nullptr;
	}

	IOReturn cpuComplete(const TegraGM20BAbiCpuComplete *, TegraGM20BAbiCpuCompleteReply *);
};

#include "drivers/TegraGM20B/SwitchGpuUserCpuComplete.h"

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t)
{
}
#endif

namespace
{
constexpr uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t write = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;

class CpuComplete : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_visibility operations = {external_status,
									  external_wait};
			ASSERT_EQ(switch_gpu_backing_register_visibility(SWITCH_GPU_ENGINE_GM20B,
									 &operations),
				  0);
			registered = true;
		}
		external_completed.clear();
		failZeroAllocation() = false;
	}

	void TearDown() override
	{
		failZeroAllocation() = false;
		cache_callback = {};
		Backing::TearDown();
	}

	TegraGM20BAbiCpuCompleteReply acknowledge(switch_gpu_backing *b, switch_gpu_backing_token t,
						  uint64_t offset = 64, uint64_t bytes = 64)
	{
		const SwitchGpuCpuRange range{b, offset, bytes};
		TegraGM20BAbiCpuCompleteReply result{};
		EXPECT_EQ(switch_gpu_cpu_complete(t, &range, 1, &result), 0);
		return result;
	}
};

TEST_F(CpuComplete, AcknowledgesOnlyMaintainedCachelines)
{
	auto *b = backing();
	const auto t = token(1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	EXPECT_EQ(acknowledge(b, t, 65, 1).current_count, 1u);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 64u);
	EXPECT_EQ(cache[0].bytes, 64u);
	EXPECT_EQ(cache[0].invalidate, SWITCH_GPU_CACHE_CLEAN_INVALIDATE);
	cache.clear();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, read), 0);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, read), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 128u);
	EXPECT_EQ(cache[0].invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, RetainedPointerWritesSurviveRepeatedCompletionAndCpuAcquire)
{
	auto *b = backing();
	const auto t = token(2);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	ASSERT_EQ(acknowledge(b, t).current_count, 1u);
	cache.clear();
	int cpu = 77;
	cache_callback = [&](uint64_t, uint64_t, int operation) {
		if (operation == SWITCH_GPU_CACHE_INVALIDATE)
			cpu = 0;
	};
	EXPECT_EQ(acknowledge(b, t).current_count, 1u);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, write), 0);
	EXPECT_EQ(cpu, 77);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, write), 0);
	cache.clear();
	EXPECT_EQ(acknowledge(b, t).stale, 1u);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, read), 0);
	EXPECT_EQ(cpu, 77);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, NewGpuContentInvalidatesAcknowledgmentWithoutWalkingBitmap)
{
	auto *b = backing();
	const auto first = token(3), next = token(3, 2);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, first), 0);
	wait(&first);
	ASSERT_EQ(acknowledge(b, first).current_count, 1u);
	const auto stamp = b->cpu_current_content;
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, next), 0);
	EXPECT_EQ(b->cpu_current_content, stamp);
	EXPECT_NE(b->content, stamp);
	wait(&next);
	cache.clear();
	EXPECT_EQ(acknowledge(b, first).stale, 1u);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, ChannelIncarnationAndForeignProducerCannotAcknowledge)
{
	auto *b = backing();
	const auto first = token(4, 1, SWITCH_GPU_ENGINE_GM20B, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, first), 0);
	wait(&first);
	cache.clear();
	EXPECT_EQ(acknowledge(b, token(4, 1, SWITCH_GPU_ENGINE_GM20B, 2)).stale, 1u);
	EXPECT_EQ(acknowledge(b, token(5)).stale, 1u);
	EXPECT_TRUE(cache.empty());
	const auto foreign = token(5, 2, SWITCH_GPU_ENGINE_NVDEC);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, foreign), 0);
	wait(&foreign);
	cache.clear();
	EXPECT_EQ(acknowledge(b, first).stale, 1u);
	EXPECT_TRUE(cache.empty());
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, PendingExternalVisibilityDoesNotAcknowledgeOrWait)
{
	auto *b = backing();
	const auto t = token(6);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	external_completed[key(&t)] = 0;
	const auto waits = external_waits;
	cache.clear();
	auto result = acknowledge(b, t);
	EXPECT_EQ(result.pending, 1u);
	EXPECT_EQ(result.first_error, -16);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(external_waits, waits);
	external_completed[key(&t)] = 1;
	EXPECT_EQ(acknowledge(b, t).current_count, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, FailedSourcePreservesErrorAndDirtyState)
{
	auto *b = backing();
	const auto t = token(7);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	timelines[key(&t)].error = -5;
	cache.clear();
	const auto result = acknowledge(b, t);
	EXPECT_EQ(result.failed, 1u);
	EXPECT_EQ(result.first_error, -5);
	EXPECT_TRUE(cache.empty());
	EXPECT_TRUE(b->dirty & SWITCH_GPU_BACKING_GPU_DIRTY);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, EntireBatchValidatedBeforeAnyMaintenance)
{
	auto *b = backing();
	const auto t = token(8);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	const SwitchGpuCpuRange ranges[] = {{b, 64, 64}, {b, 4090, 64}};
	TegraGM20BAbiCpuCompleteReply reply{};
	EXPECT_EQ(switch_gpu_cpu_complete(t, ranges, 2, &reply), -22);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(b->cpu_current, nullptr);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, AllocationFailureLeavesVisibilityUnacknowledged)
{
	auto *b = backing();
	const auto t = token(9);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	failZeroAllocation() = true;
	const auto result = acknowledge(b, t);
	EXPECT_EQ(result.failed, 1u);
	EXPECT_EQ(result.first_error, -12);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(b->cpu_current, nullptr);
	EXPECT_TRUE(b->dirty & SWITCH_GPU_BACKING_GPU_DIRTY);
	failZeroAllocation() = false;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, WrappedZeroPrefixCoversPreviousWriter)
{
	auto *b = backing();
	const auto writer = token(10, UINT32_MAX), prefix = token(10, 0);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, writer), 0);
	wait(&writer);
	cache.clear();
	EXPECT_EQ(acknowledge(b, prefix).current_count, 1u);
	EXPECT_EQ(acknowledge(b, token(10, UINT32_MAX - 1)).stale, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, ActiveCpuWriterPreventsMaintenance)
{
	auto *b = backing();
	const auto t = token(11);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, write), 0);
	cache.clear();
	int cpu = 77;
	cache_callback = [&](uint64_t offset, uint64_t, int operation) {
		if (offset == 128 && operation == SWITCH_GPU_CACHE_INVALIDATE)
			cpu = 0;
	};
	EXPECT_EQ(acknowledge(b, t).stale, 1u);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, write), 0);
	cache.clear();
	EXPECT_EQ(acknowledge(b, t).stale, 1u);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, read), 0);
	EXPECT_EQ(cpu, 77);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, read), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 256, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 256u);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 256, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, MaintenanceAndWriterPublicationShareOwnershipLock)
{
	auto *b = backing();
	const auto first = token(12), next = token(12, 2);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, first), 0);
	wait(&first);
	cache.clear();
	std::promise<void> entered, resume;
	auto resumed = resume.get_future().share();
	cache_callback = [&](uint64_t, uint64_t, int) {
		entered.set_value();
		resumed.wait();
	};
	auto acknowledgment = std::async(std::launch::async, [&] {
		return acknowledge(b, first);
	});
	entered.get_future().wait();
	std::promise<void> attempting;
	auto writer = std::async(std::launch::async, [&] {
		attempting.set_value();
		return publish(b, SWITCH_GPU_BACKING_WRITE, next);
	});
	attempting.get_future().wait();
	EXPECT_EQ(writer.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	resume.set_value();
	EXPECT_EQ(acknowledgment.get().current_count, 1u);
	EXPECT_EQ(writer.get(), 0);
	cache_callback = {};
	wait(&next);
	cache.clear();
	EXPECT_EQ(acknowledge(b, first).stale, 1u);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(acknowledge(b, next).current_count, 1u);
	EXPECT_EQ(cache.size(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, CpuOnlyBackingHasNoAttestedRawProducer)
{
	auto *b = backing();
	cache.clear();
	EXPECT_EQ(acknowledge(b, token(13)).stale, 1u);
	EXPECT_TRUE(cache.empty());
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, AdapterValidatesEveryHandleAndExtentBeforeMaintenance)
{
	auto *b = backing();
	const auto t = token(14);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	tgm_chan channel{14, 1};
	AGXDeviceUserClient client;
	client.chans_[0] = &channel;
	client.bos.push_back({17, 4096, b});
	TegraGM20BAbiCpuRange ranges[] = {{17, 64, 64}, {18, 128, 64}};
	TegraGM20BAbiCpuComplete request{14, reinterpret_cast<uint64_t>(ranges), 1, 1, 2, 0};
	TegraGM20BAbiCpuCompleteReply reply{};
	EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnBadArgument);
	EXPECT_TRUE(cache.empty());
	ranges[1] = {17, 4090, 64};
	EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnBadArgument);
	EXPECT_TRUE(cache.empty());
	ranges[1] = {17, 192, 64};
	EXPECT_EQ(client.cpuComplete(&request, &reply), 0);
	EXPECT_EQ(reply.current_count, 2u);
	EXPECT_EQ(cache.size(), 2u);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, AdapterRejectsRecycledIncarnationAndFutureSequence)
{
	auto *b = backing();
	const auto t = token(15, 1, SWITCH_GPU_ENGINE_GM20B, 2);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	tgm_chan channel{(uint64_t(1) << 32) | 15, 1};
	AGXDeviceUserClient client;
	client.chans_[0] = &channel;
	client.bos.push_back({18, 4096, b});
	TegraGM20BAbiCpuRange range{18, 64, 64};
	TegraGM20BAbiCpuComplete request{15, reinterpret_cast<uint64_t>(&range), 1, 1, 1, 0};
	TegraGM20BAbiCpuCompleteReply reply{};
	EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnNotReady);
	EXPECT_TRUE(cache.empty());
	request.channel_uid = channel.uid;
	request.seqno = 2;
	EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnNotReady);
	EXPECT_TRUE(cache.empty());
	request.seqno = 1;
	EXPECT_EQ(client.cpuComplete(&request, &reply), 0);
	EXPECT_EQ(reply.current_count, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, AdapterRetainsBackingAndTokenAcrossConnectionClose)
{
	auto *b = backing();
	const auto t = token(16, 0);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	cache.clear();
	tgm_chan channel{16, 0};
	AGXDeviceUserClient client;
	client.chans_[0] = &channel;
	client.bos.push_back({19, 4096, b});
	TegraGM20BAbiCpuRange range{19, 64, 64};
	TegraGM20BAbiCpuComplete request{16, reinterpret_cast<uint64_t>(&range), 1, 0, 1, 0};
	TegraGM20BAbiCpuCompleteReply reply{};
	cache_callback = [&](uint64_t, uint64_t, int) {
		IOLockLock(client.lock_);
		client.chans_[0] = nullptr;
		client.submitGate_.stopped = true;
		client.bos.clear();
		IOLockUnlock(client.lock_);
		EXPECT_GE(timelines[key(&t)].refs, 2);
	};
	EXPECT_EQ(client.cpuComplete(&request, &reply), 0);
	EXPECT_EQ(reply.current_count, 1u);
	cache_callback = {};
	EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnNotOpen);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, AdapterRefusesUnmaintainableViews)
{
	auto *b = backing();
	const auto t = token(17);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	tgm_chan channel{17, 1};
	AGXDeviceUserClient client;
	client.chans_[0] = &channel;
	client.bos.push_back({20, 4096, b});
	TegraGM20BAbiCpuRange range{20, 64, 64};
	TegraGM20BAbiCpuComplete request{17, reinterpret_cast<uint64_t>(&range), 1, 1, 1, 0};
	TegraGM20BAbiCpuCompleteReply reply{};
	for (unsigned flag = 0; flag < 3; ++flag) {
		client.bos[0].chunked = flag == 0;
		client.bos[0].nativeView = flag == 1;
		client.bos[0].representation = flag == 2;
		EXPECT_EQ(client.cpuComplete(&request, &reply), kIOReturnBadArgument);
	}
	EXPECT_TRUE(cache.empty());
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, FailedCpuHandoffDoesNotExposeOrRetainCpuOwnership)
{
	auto *b = backing();
	const auto t = token(18);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	failZeroAllocation() = true;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, write), -12);
	EXPECT_EQ(b->cpu_writers, 0u);
	EXPECT_TRUE(cache.empty());
	EXPECT_TRUE(b->dirty & SWITCH_GPU_BACKING_GPU_DIRTY);
	failZeroAllocation() = false;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, write), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, write), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, DiscardedCpuRangeRemainsCurrentWhileOtherGpuLinesStayDirty)
{
	auto *b = backing();
	const auto t = token(19);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	constexpr auto discard = write | SWITCH_GPU_BACKING_CPU_DISCARD;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, discard), 0);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, discard), 0);
	cache.clear();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 128, 64, read), 0);
	EXPECT_TRUE(cache.empty());
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 128, 64, read), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 256, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 256u);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 256, 64, read), 0);
	switch_gpu_backing_release(b);
}

TEST_F(CpuComplete, OwnedTransferAllocationFailureKeepsOriginalLease)
{
	auto *b = backing();
	const auto t = token(20);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	cache.clear();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(
			  b, 0, 4096, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE,
			  &lease),
		  0);
	auto *original = lease;
	failZeroAllocation() = true;
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(b, &lease, read), -12);
	EXPECT_EQ(lease, original);
	EXPECT_EQ(b->cpu_readers, 1u);
	EXPECT_TRUE(cache.empty());
	failZeroAllocation() = false;
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(b, &lease, read), 0);
	EXPECT_EQ(lease, nullptr);
	EXPECT_EQ(b->cpu_readers, 1u);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, read), 0);
	switch_gpu_backing_release(b);
}
}  // namespace
