// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuScanoutSnapshot.h"

struct switch_gpu_backing {
	switch_gpu_backing_token producer;
	int error = 0;
	unsigned users = 0;
	std::vector<switch_gpu_backing_token> promises =
		{};  // pending writer promises (SwitchGpuWriterPromise.h)
};

struct switch_gpu_cpu_lease {
	switch_gpu_backing *backing;
};

extern "C" int switch_gpu_backing_snapshot_read(switch_gpu_backing *backing,
						switch_gpu_backing_token *producer,
						switch_gpu_cpu_lease **lease)
{
	if (backing->error)
		return backing->error;
	*producer = backing->producer;
	*lease = new switch_gpu_cpu_lease{backing};
	++backing->users;
	return 0;
}

extern "C" int switch_gpu_backing_snapshot_read_promises(switch_gpu_backing *backing,
							 switch_gpu_backing_token *producer,
							 switch_gpu_cpu_lease **lease,
							 switch_gpu_backing_token *tokens,
							 uint32_t capacity)
{
	if (backing->promises.size() > capacity)
		return -28;
	const int result = switch_gpu_backing_snapshot_read(backing, producer, lease);
	if (result)
		return result;
	for (size_t i = 0; i < backing->promises.size(); ++i)
		tokens[i] = backing->promises[i];
	return int(backing->promises.size());
}

#if SWITCH_GPU_PATH_STATS
unsigned promiseWaits;

extern "C" void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count)
{
	if (path == SWITCH_GPU_PATH_PROMISE_WAIT)
		promiseWaits += count;
}
#endif

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **lease, int wrote)
{
	EXPECT_EQ(wrote, 0);
	--(*lease)->backing->users;
	delete *lease;
	*lease = nullptr;
	return 0;
}

TEST(ScanoutSnapshot, CombinesUiAndVideoProducersWithoutChangingTheCallerList)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing_token video{10, 2, SWITCH_GPU_ENGINE_NVDEC, 170};
	switch_gpu_backing ui{{20, 3, SWITCH_GPU_ENGINE_GM20B, 4}};
	ASSERT_EQ(snapshot.begin(&video, 1), 0);
	ASSERT_EQ(snapshot.add(&ui), 0);
	EXPECT_EQ(snapshot.count, 2u);
	EXPECT_EQ(snapshot.tokens[0].value, video.value);
	EXPECT_EQ(snapshot.tokens[1].queue, ui.producer.queue);
	EXPECT_EQ(ui.users, 1u);
	EXPECT_EQ(snapshot.begin(nullptr, 0), -16);
	snapshot.cancel();
	EXPECT_EQ(ui.users, 0u);
}

TEST(ScanoutSnapshot, CpuAuthoredFrameIsExactWithNoGpuTokens)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing ui{{}};
	ASSERT_EQ(snapshot.begin(nullptr, 0), 0);
	ASSERT_EQ(snapshot.add(&ui), 0);
	EXPECT_EQ(snapshot.count, 0u);
	EXPECT_EQ(snapshot.leaseCount, 1u);
	snapshot.cancel();
	EXPECT_EQ(ui.users, 0u);
}

TEST(ScanoutSnapshot, FailureRetainsEarlierSnapshotUntilAbort)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing ui{{}}, video{{}, -5};
	ASSERT_EQ(snapshot.begin(nullptr, 0), 0);
	ASSERT_EQ(snapshot.add(&ui), 0);
	EXPECT_EQ(snapshot.add(&video), -5);
	EXPECT_EQ(ui.users, 1u);
	snapshot.cancel();
	EXPECT_EQ(ui.users, 0u);
	EXPECT_EQ(video.users, 0u);
}

TEST(ScanoutSnapshot, OverflowNeverDropsAProducerAndUnwindsItsReservation)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_MAX_DEPENDENCIES] = {};
	switch_gpu_backing ui{{1, 1, SWITCH_GPU_ENGINE_GM20B, 1}};
	ASSERT_EQ(snapshot.begin(tokens, SWITCH_GPU_PRESENT_MAX_DEPENDENCIES), 0);
	EXPECT_EQ(snapshot.add(&ui), -28);
	EXPECT_EQ(ui.users, 1u);
	snapshot.cancel();
	EXPECT_EQ(ui.users, 0u);
}

TEST(ScanoutSnapshot, CompletionReleasesOwnershipBeforeCallingDisplayAndAllowsNextFrame)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing ui{{}};

	struct Context {
		SwitchGpuScanoutSnapshot *snapshot;
		switch_gpu_backing *ui;
		unsigned calls;
	} context{&snapshot, &ui, 0};

	ASSERT_EQ(snapshot.begin(nullptr, 0), 0);
	ASSERT_EQ(snapshot.add(&ui), 0);
	snapshot.display = &context;
	snapshot.done = [](void *arg, int status) {
		auto *context = static_cast<Context *>(arg);
		EXPECT_EQ(status, -5);
		EXPECT_EQ(context->ui->users, 0u);
		EXPECT_EQ(context->snapshot->begin(nullptr, 0), 0);
		++context->calls;
	};
	SwitchGpuScanoutSnapshot::complete(&snapshot, -5);
	EXPECT_EQ(context.calls, 1u);
	EXPECT_EQ(snapshot.active, 1u);
	snapshot.cancel();
}

// A surface a held command buffer promised: the present acquires the promises beside the producer.
TEST(ScanoutSnapshot, APromisedSurfaceAddsItsPromises)
{
	SwitchGpuScanoutSnapshot snapshot = {};
	switch_gpu_backing drawable{{20, 3, SWITCH_GPU_ENGINE_GM20B, 4}};
	drawable.promises = {{8, 0, SWITCH_GPU_ENGINE_PROMISE, 2048},
			     {9, 0, SWITCH_GPU_ENGINE_PROMISE, 2052}};
	ASSERT_EQ(snapshot.begin(nullptr, 0), 0);
	ASSERT_EQ(snapshot.add(&drawable), 0);
	ASSERT_EQ(snapshot.count, 3u);
	EXPECT_EQ(snapshot.tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
	EXPECT_EQ(snapshot.tokens[1].queue, 2052u);
	EXPECT_EQ(snapshot.tokens[2].engine, uint32_t(SWITCH_GPU_ENGINE_GM20B));
	snapshot.cancel();
	EXPECT_EQ(drawable.users, 0u);
}
