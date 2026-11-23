// SPDX-License-Identifier: MIT
// A killed channel's records against later users of the same allocation (SwitchGpuOwnership.cpp): no later use fails, a
// failed write is logged once per producer, and no record or timeline reference outlives its use. A writer's record
// stays while its write is the allocation's current content (the producer a snapshot or export names), as a completed
// one does; the next write retires it.
// A test that counts log lines kills a producer (queue) of its own: the log names each producer once per binary.
// What a failed or refused writer leaves is never shown: the display's snapshot of it, an export and a present that
// waits for it end with why (SwitchGpuBacking.h WRITER_GONE, WRITER_FAILED, WRITE_REFUSED) and no lease.
#include <array>
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuPresentTokens.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;

// why each timeline's failed tokens failed, as SwitchGpuTimeline.cpp's failure tells it; none: it cannot tell
std::map<Key, int> whys;

int failure(const switch_gpu_backing_token *token)
{
	const auto at = whys.find(key(token));
	return at == whys.end() ? 0 : at->second;
}

class Unshown : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		whys.clear();
		ASSERT_EQ(switch_gpu_backing_register_failure(SWITCH_GPU_ENGINE_GM20B, failure), 0);
	}
};

// The present's snapshot (SwitchGpuScanoutSnapshot::add): its status, and whether the backing was handed over (a lease)
int presentSnapshot(switch_gpu_backing *allocation, switch_gpu_backing_token &producer)
{
	switch_gpu_backing_token promises[4] = {};
	switch_gpu_cpu_lease *lease = nullptr;
	const int result = switch_gpu_backing_snapshot_read_promises(allocation, &producer, &lease,
								     promises, 4);
	if (result >= 0)
		EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	else
		EXPECT_EQ(lease, nullptr);
	return result;
}

uint32_t cpuLocks(switch_gpu_backing *allocation)
{
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	return info.cpu_locks;
}

int cpu_read(switch_gpu_backing *allocation)
{
	const int result = switch_gpu_backing_cpu_acquire(allocation, 0, 4096, kRead);
	if (!result)
		EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, kRead), 0);
	return result;
}

int cpu_write(switch_gpu_backing *allocation, uint64_t offset, uint64_t bytes)
{
	const int result = switch_gpu_backing_cpu_acquire(allocation, offset, bytes, kWrite);
	if (!result)
		EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, offset, bytes, kWrite), 0);
	return result;
}
}  // namespace

TEST_F(Backing, FailedReaderLetsTheNextWriterAndReaderRun)
{
	auto *allocation = backing();
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, kRead, reader), 0);
	kill_channel(reader);
	const int logged = ioLogLines();
	EXPECT_EQ(publish(allocation, kWrite, token(2)), 0);
	EXPECT_EQ(refs(reader), 0);
	EXPECT_EQ(publish(allocation, kRead, token(3)), 0);
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(ioLogLines(), logged);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedWriterFailsNoReaderAndIsLoggedOnce)
{
	auto *allocation = backing();
	const auto writer = token(21);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	const int logged = ioLogLines();
	EXPECT_EQ(publish(allocation, kRead, token(2)), 0);
	EXPECT_EQ(refs(writer), 1);
	EXPECT_EQ(ioLogLines(), logged + 1);
	EXPECT_EQ(publish(allocation, kRead, token(3)), 0);
	EXPECT_EQ(publish(allocation, kRead | kWrite, token(4)), 0);
	complete(token(2));
	complete(token(3));
	complete(token(4));
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(refs(writer), 0);
	EXPECT_EQ(ioLogLines(), logged + 1);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedWriterFailsNoCpuReader)
{
	auto *allocation = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(refs(writer), 1);
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(publish(allocation, kRead, token(2)), 0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, WritesAfterAFailedWriterRun)
{
	auto *gpu = backing(), *cpu = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(gpu, kWrite, writer), 0);
	ASSERT_EQ(publish(cpu, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(publish(gpu, kWrite, token(2)), 0);
	EXPECT_EQ(cpu_write(cpu, 64, 64), 0);
	EXPECT_EQ(publish(gpu, kRead, token(3)), 0);
	EXPECT_EQ(publish(cpu, kRead, token(3)), 0);
	complete(token(2));
	complete(token(3));
	EXPECT_EQ(publish(gpu, kRead, token(3, 2)), 0);
	EXPECT_EQ(refs(writer), 0);
	switch_gpu_backing_release(gpu);
	switch_gpu_backing_release(cpu);
}

TEST_F(Backing, OneKilledProducerOfSeveralInputsIsLoggedOnceAndFailsNoReader)
{
	std::array<switch_gpu_backing *, 3> inputs = {backing(), backing(), backing()};
	const auto writer = token(22);
	for (auto *input : inputs)
		ASSERT_EQ(publish(input, kWrite, writer), 0);
	kill_channel(writer);
	const int logged = ioLogLines();
	std::array<switch_gpu_backing_use, 3> uses = {};
	for (size_t i = 0; i < uses.size(); ++i)
		uses[i] = {inputs[i], kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto reader = token(2);
	ASSERT_EQ(
		switch_gpu_backing_begin(uses.data(), uint32_t(uses.size()), &reader, &transaction),
		0);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(refs(writer), int(inputs.size()));
	EXPECT_EQ(ioLogLines(), logged + 1);
	for (auto *input : inputs)
		switch_gpu_backing_release(input);
}

TEST_F(Backing, ProducerKilledAfterBeginIsNothingToWaitFor)
{
	auto *allocation = backing();
	const auto writer = token(23);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	switch_gpu_backing_use use = {allocation, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto reader = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &reader, &transaction), 0);
	kill_channel(writer);
	const int logged = ioLogLines();
	std::array<switch_gpu_backing_token, 4> dependencies = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 4, &needed),
		  0);
	EXPECT_EQ(needed, 0u);
	EXPECT_EQ(ioLogLines(), logged + 1);
	switch_gpu_backing_commit(&transaction);
	complete(reader);
	EXPECT_EQ(publish(allocation, kRead, token(2, 2)), 0);
	EXPECT_EQ(refs(writer), 1);
	complete(token(2, 2));
	EXPECT_EQ(publish(allocation, kWrite, token(2, 3)), 0);
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(refs(writer), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, WriterDoesNotWaitForOrFailOnAKilledReader)
{
	auto *allocation = backing();
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, kRead, reader), 0);
	switch_gpu_backing_use use = {allocation, kWrite, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto writer = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &writer, &transaction), 0);
	kill_channel(reader);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 0u);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
}

// Process A writes `drawn` and samples `shared`; process B samples `shared` too. A's channel dies with both pending.
TEST_F(Backing, KillInOneProcessFailsNothingInAnother)
{
	auto *drawn = backing(), *shared = backing();
	const auto a = token(1), b = token(2);
	ASSERT_EQ(publish(drawn, kWrite, a), 0);
	ASSERT_EQ(publish(shared, kRead, a), 0);
	ASSERT_EQ(publish(shared, kRead, b), 0);
	kill_channel(a);
	EXPECT_EQ(publish(shared, kRead, token(2, 2)), 0);
	EXPECT_EQ(publish(drawn, kRead, token(2, 3)), 0);
	EXPECT_EQ(refs(a), 2);
	EXPECT_EQ(cpu_read(drawn), 0);
	complete(token(2, 3));
	EXPECT_EQ(publish(shared, kWrite, token(2, 5)), 0);
	EXPECT_EQ(publish(drawn, kWrite, token(2, 5)), 0);
	complete(token(2, 5));
	EXPECT_EQ(cpu_read(drawn), 0);
	EXPECT_EQ(refs(a), 0);
	switch_gpu_backing_release(drawn);
	switch_gpu_backing_release(shared);
}

// (c) A writer that failed on the GPU as the kernel's own failure (a compositor channel's kill, an engine that cannot
// say more): the present's snapshot is refused with it and takes no reference to it.
TEST_F(Unshown, ASnapshotOverAWriterThatFailedOnTheGpuIsRefusedFailed)
{
	auto *allocation = backing();
	const auto writer = token(41);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	const int held = refs(writer);
	switch_gpu_backing_token producer = token(5);
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITER_FAILED);
	EXPECT_EQ(producer.engine, 0u);
	EXPECT_EQ(cpuLocks(allocation), 0u);
	EXPECT_EQ(refs(writer), held);
	switch_gpu_backing_release(allocation);
}

// (a) A writer whose client is gone (its channel torn down, an app's channel killed): its frame is not shown, and the
// next one its client writes is.
TEST_F(Unshown, ASnapshotOverAGoneClientsWriterIsRefusedGone)
{
	auto *allocation = backing();
	const auto writer = token(42);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	whys[key(&writer)] = SWITCH_GPU_BACKING_WRITER_GONE;
	kill_channel(writer);
	switch_gpu_backing_token producer = {};
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITER_GONE);
	EXPECT_EQ(cpuLocks(allocation), 0u);
	const auto next = token(43);
	ASSERT_EQ(publish(allocation, kWrite, next), 0);
	EXPECT_EQ(presentSnapshot(allocation, producer), 0);
	EXPECT_EQ(producer.queue, next.queue);
	switch_gpu_backing_release(allocation);
}

// (b) A write whose submission was refused never came: the frame before it is not shown in its place, until the next
// write. A refused submission's reads mark nothing.
TEST_F(Unshown, ASnapshotAfterARefusedWriteIsRefusedUntilTheNextWrite)
{
	auto *allocation = backing(), *read = backing();
	const auto shown = token(44);
	ASSERT_EQ(publish(allocation, kWrite, shown), 0);
	complete(shown);
	switch_gpu_backing_token producer = {};
	ASSERT_EQ(presentSnapshot(allocation, producer), 0);
	const std::array<switch_gpu_backing_use, 2> refused = {
		{{allocation, kWrite, 0}, {read, kRead, 0}}};
	switch_gpu_backing_writes_refused(refused.data(), uint32_t(refused.size()));
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITE_REFUSED);
	EXPECT_EQ(cpuLocks(allocation), 0u);
	EXPECT_EQ(presentSnapshot(read, producer), 0);
	ASSERT_EQ(publish(allocation, kWrite, token(44, 2)), 0);
	EXPECT_EQ(presentSnapshot(allocation, producer), 0);
	EXPECT_EQ(producer.value, 2u);
	switch_gpu_backing_release(read);
	switch_gpu_backing_release(allocation);
}

// A writer that fails after the snapshot: the present's admission refuses it with why, and once admitted the present
// ends with why, which its completion hands the display.
TEST_F(Unshown, APresentWhoseWriterFailsAfterItsSnapshotEndsWithWhy)
{
	auto *allocation = backing();
	switch_gpu_backing_token producer = {}, promises[4] = {};
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_present_tokens held = {};
	const auto first = token(45);
	ASSERT_EQ(publish(allocation, kWrite, first), 0);
	ASSERT_EQ(switch_gpu_backing_snapshot_read_promises(allocation, &producer, &lease, promises,
							    4),
		  0);
	kill_channel(first);
	EXPECT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1),
		  SWITCH_GPU_BACKING_WRITER_FAILED);
	EXPECT_EQ(held.count, 0u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	const auto second = token(46);
	ASSERT_EQ(publish(allocation, kWrite, second), 0);
	ASSERT_EQ(switch_gpu_backing_snapshot_read_promises(allocation, &producer, &lease, promises,
							    4),
		  0);
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&held, &producer, 1), 0);
	whys[key(&second)] = SWITCH_GPU_BACKING_WRITER_GONE;
	kill_channel(second);
	EXPECT_EQ(switch_gpu_present_tokens_finish_visible(&held, 0, nullptr),
		  SWITCH_GPU_BACKING_WRITER_GONE);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

// An engine that names no cause (VIC here): a job cancelled (DEPENDENCY_FAILED: its client's close, an input that
// failed) ends its frame GONE; any other failure of its own is the kernel's.
TEST_F(Unshown, AnEngineJobCancelledEndsItsFrameGoneAndItsOwnFailureFailed)
{
	auto *allocation = backing();
	const auto vic = token(160, 3, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(publish(allocation, kWrite, vic), 0);
	timelines[key(&vic)].error = SWITCH_GPU_BACKING_DEPENDENCY_FAILED;
	switch_gpu_backing_token producer = {};
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITER_GONE);
	timelines[key(&vic)].error = -5;
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITER_FAILED);
	switch_gpu_backing_release(allocation);
}

namespace
{
std::map<Key, int> physicals;  // NVDEC jobs' physical results, ahead of their final status

int nvdecPhysical(const switch_gpu_backing_token *token, int)
{
	const auto at = physicals.find(key(token));
	return at == physicals.end() ? status(token) : at->second;
}

int noSnapshot(const switch_gpu_backing_token *, switch_gpu_event_waits **waits)
{
	*waits = nullptr;
	return 0;
}

int nonePending()
{
	return 0;
}

// A VIC job's why as its engine keeps it with the job's result (SwitchHost1xResults.h failure)
int vicFailure(const switch_gpu_backing_token *token)
{
	return failure(token);
}
}  // namespace

// A host1x job fails physically before its final status lands: why it failed reads from its physical result too,
// cancelled (-125) GONE, any other failure of an engine with no answer FAILED.
TEST_F(Unshown, AnEngineJobsWhyReadsItsPhysicalResultAheadOfItsStatus)
{
	const switch_gpu_backing_semantics semantics = {noSnapshot, nvdecPhysical, nonePending};
	const int registered =
		switch_gpu_backing_register_semantics(SWITCH_GPU_ENGINE_NVDEC, &semantics);
	ASSERT_TRUE(registered == 0 || registered == -16);
	physicals.clear();
	const auto decode = token(170, 4, SWITCH_GPU_ENGINE_NVDEC);
	ASSERT_EQ(switch_gpu_backing_token_status(&decode), 0);
	physicals[key(&decode)] = SWITCH_GPU_BACKING_DEPENDENCY_FAILED;
	EXPECT_EQ(switch_gpu_backing_token_failure(&decode), SWITCH_GPU_BACKING_WRITER_GONE);
	physicals[key(&decode)] = -5;
	EXPECT_EQ(switch_gpu_backing_token_failure(&decode), SWITCH_GPU_BACKING_WRITER_FAILED);
	physicals.clear();
}

// A VIC job that failed for its input (an app's GM20B job, whose channel was killed) keeps that input's why, as its
// engine decides it where its physical result fails; a present of what it wrote is refused GONE, not FAILED. An input
// that is the kernel's own failure (a compositor kill) makes it FAILED; no failed input, none.
TEST_F(Unshown, AnEngineJobThatFailedForItsInputAnswersTheInputsWhy)
{
	ASSERT_EQ(switch_gpu_backing_register_failure(SWITCH_GPU_ENGINE_VIC, vicFailure), 0);
	const auto app = token(47), compositor = token(48), done = token(49);
	complete(done);
	whys[key(&app)] = SWITCH_GPU_BACKING_WRITER_GONE;
	kill_channel(app);
	kill_channel(compositor);
	const std::array<switch_gpu_backing_token, 2> fromApp = {done, app},
						      fromCompositor = {done, compositor};
	EXPECT_EQ(switch_gpu_backing_tokens_failure(&done, 1), 0);
	EXPECT_EQ(switch_gpu_backing_tokens_failure(fromCompositor.data(), 2),
		  SWITCH_GPU_BACKING_WRITER_FAILED);
	auto *allocation = backing();
	const auto vic = token(161, 3, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(publish(allocation, kWrite, vic), 0);
	whys[key(&vic)] = switch_gpu_backing_tokens_failure(fromApp.data(), 2);
	timelines[key(&vic)].error = -5;
	switch_gpu_backing_token producer = {};
	EXPECT_EQ(presentSnapshot(allocation, producer), SWITCH_GPU_BACKING_WRITER_GONE);
	switch_gpu_backing_release(allocation);
}
