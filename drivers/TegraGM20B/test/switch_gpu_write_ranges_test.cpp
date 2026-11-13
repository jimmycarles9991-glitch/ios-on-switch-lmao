// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuCpuComplete.h"
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"

namespace
{
class WriteRanges : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_visibility ops = {external_status, external_wait};
			ASSERT_EQ(switch_gpu_backing_register_visibility(SWITCH_GPU_ENGINE_GM20B,
									 &ops),
				  0);
			registered = true;
		}
		external_completed.clear();
	}

	int submit(switch_gpu_backing *b, const switch_gpu_backing_token &t,
		   std::initializer_list<switch_gpu_backing_write_range> writes)
	{
		switch_gpu_backing_use use{b, SWITCH_GPU_BACKING_WRITE, 0};
		switch_gpu_backing_transaction transaction{};
		int result = switch_gpu_backing_begin(&use, 1, &t, &transaction);
		std::vector<switch_gpu_backing_write_range> ranges(writes);
		if (!result)
			result = switch_gpu_backing_write_ranges(&transaction, ranges.data(),
								 ranges.size());
		if (result)
			switch_gpu_backing_abort(&transaction);
		else
			switch_gpu_backing_commit(&transaction);
		return result;
	}

	TegraGM20BAbiCpuCompleteReply complete(switch_gpu_backing *b,
					       const switch_gpu_backing_token &t, uint64_t offset,
					       uint64_t bytes = 64)
	{
		const SwitchGpuCpuRange range{b, offset, bytes};
		TegraGM20BAbiCpuCompleteReply reply{};
		EXPECT_EQ(switch_gpu_cpu_complete(t, &range, 1, &reply), 0);
		return reply;
	}
};

TEST_F(WriteRanges, FirstGpuWritePreservesNeverGpuWrittenCpuSpan)
{
	auto *b = backing();
	const auto t = token(1);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, t, {{b, 128, 64}}), 0);
	wait(&t);
	cache.clear();
	EXPECT_EQ(complete(b, t, 0, 256).current_count, 1u);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 128u);
	EXPECT_EQ(cache[0].bytes, 64u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, DisjointGpuWritePreservesPreviouslyAcknowledgedCpuDirtySpan)
{
	auto *b = backing();
	const auto a = token(1), other = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 64, 64}}), 0);
	wait(&a);
	ASSERT_EQ(complete(b, a, 64).current_count, 1u);
	int dirty = 77;
	cache_callback = [&](uint64_t offset, uint64_t bytes, int op) {
		if (op && offset < 128 && offset + bytes > 64)
			dirty = 0;
	};
	ASSERT_EQ(submit(b, other, {{b, 256, 64}}), 0);
	wait(&other);
	cache.clear();
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	EXPECT_EQ(dirty, 77);
	EXPECT_TRUE(cache.empty());
	const uint32_t access = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, access), 0);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(dirty, 77);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, access), 0);
	cache_callback = {};
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, LateCompletionKeepsExactProducerAfterCoarseWriterPrunes)
{
	auto *b = backing();
	const auto a = token(1), other = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 64, 64}}), 0);
	wait(&a);
	ASSERT_EQ(submit(b, other, {{b, 256, 64}}), 0);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_READ, token(3)), 0);
	EXPECT_EQ(timelines[key(&a)].refs, 2);
	cache.clear();
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 64u);
	EXPECT_EQ(complete(b, other, 256).pending, 1u);
	EXPECT_EQ(complete(b, a, 256).stale, 1u);
	wait(&other);
	const auto reader = token(3);
	wait(&reader);
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	EXPECT_EQ(timelines[key(&a)].refs, 1);
	EXPECT_EQ(complete(b, token(1, 1, SWITCH_GPU_ENGINE_GM20B, 2), 64).stale, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, NewOverlappingWriterSupersedesOnlyItsCachelines)
{
	auto *b = backing();
	const auto a = token(1), next = token(1, 2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 0, 256}}), 0);
	wait(&a);
	ASSERT_EQ(submit(b, next, {{b, 65, 1}}), 0);
	wait(&next);
	cache.clear();
	EXPECT_EQ(complete(b, a, 0).current_count, 1u);
	EXPECT_EQ(complete(b, a, 64).stale, 1u);
	EXPECT_EQ(complete(b, a, 128).current_count, 1u);
	EXPECT_EQ(complete(b, next, 65, 1).current_count, 1u);
	ASSERT_EQ(cache.size(), 3u);
	EXPECT_EQ(cache[2].offset, 64u);
	EXPECT_EQ(cache[2].bytes, 64u);
	EXPECT_EQ(complete(b, token(1, 2, SWITCH_GPU_ENGINE_GM20B, 2), 64).stale, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, DisjointRangesWithinSameSubmissionDoNotWidenHole)
{
	auto *b = backing();
	const auto t = token(1, 0);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, t, {{b, 193, 1}, {b, 1, 1}, {b, 193, 20}, {b, 2, 4}}), 0);
	cache.clear();
	EXPECT_EQ(complete(b, t, 0, 256).current_count, 1u);
	ASSERT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache[0].offset, 0u);
	EXPECT_EQ(cache[0].bytes, 64u);
	EXPECT_EQ(cache[1].offset, 192u);
	EXPECT_EQ(cache[1].bytes, 64u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, UnknownWriteReplacesPreciseAuthorityConservatively)
{
	auto *b = backing();
	const auto a = token(1), unknown = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 64, 64}}), 0);
	wait(&a);
	ASSERT_EQ(complete(b, a, 64).current_count, 1u);
	ASSERT_EQ(submit(b, unknown, {{b, 256, 64}, {b, 0, 0}}), 0);
	wait(&unknown);
	cache.clear();
	EXPECT_EQ(b->writer_count, 1u);
	EXPECT_EQ(b->writers[0].begin, 0u);
	EXPECT_EQ(b->writers[0].end, 4096u);
	EXPECT_EQ(complete(b, a, 64).stale, 1u);
	EXPECT_EQ(complete(b, unknown, 64).current_count, 1u);
	EXPECT_EQ(cache.size(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, AbortAndInvalidTrailingRangeLeavePublishedMapAndCacheUntouched)
{
	auto *b = backing();
	const auto a = token(1), next = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 64, 64}}), 0);
	wait(&a);
	ASSERT_EQ(complete(b, a, 64).current_count, 1u);
	const auto content = b->content;
	const auto refs = timelines[key(&a)].refs;
	cache.clear();
	EXPECT_EQ(submit(b, next, {{b, 256, 64}, {b, 4095, 2}}), -22);
	EXPECT_EQ(submit(b, next, {}), -22);
	EXPECT_EQ(submit(b, next, {{b, 64, 0}}), -22);
	EXPECT_EQ(b->content, content);
	EXPECT_EQ(timelines[key(&a)].refs, refs);
	EXPECT_EQ(timelines[key(&next)].refs, 0);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	EXPECT_TRUE(cache.empty());
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, RetryCompactionPreservesMultipleBackingsAndUnknownWrites)
{
	auto *a = backing(), *b = backing();
	const auto t = token(1);
	ASSERT_EQ(switch_gpu_backing_cpu_track(a), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	switch_gpu_backing_use uses[] = {{b, SWITCH_GPU_BACKING_WRITE, 0},
					 {a, SWITCH_GPU_BACKING_WRITE, 0}};
	switch_gpu_backing_write_range ranges[] = {
		{b, 128, 64}, {a, 256, 64}, {a, 256, 64}, {b, 0, 0}, {a, 64, 64}};
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		switch_gpu_backing_transaction transaction{};
		ASSERT_EQ(switch_gpu_backing_begin(uses, 2, &t, &transaction), 0);
		ASSERT_EQ(switch_gpu_backing_write_ranges(&transaction, ranges, 5), 0);
		if (attempt < 2)
			switch_gpu_backing_abort(&transaction);
		else
			switch_gpu_backing_commit(&transaction);
	}
	EXPECT_EQ(a->writer_count, 2u);
	EXPECT_EQ(a->writers[0].begin, 64u);
	EXPECT_EQ(a->writers[1].begin, 256u);
	EXPECT_EQ(b->writer_count, 1u);
	EXPECT_EQ(b->writers[0].end, 4096u);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, IntervalLimitFailsBeforePublishingAndRetainsOldAuthority)
{
	auto *b = backing(128);
	const auto a = token(1), next = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	std::vector<switch_gpu_backing_write_range> ranges;
	for (unsigned i = 0; i < 4096; ++i)
		ranges.push_back({b, uint64_t(i) * 128, 64});
	switch_gpu_backing_use use{b, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction{};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &a, &transaction), 0);
	ASSERT_EQ(switch_gpu_backing_write_ranges(&transaction, ranges.data(), ranges.size()), 0);
	switch_gpu_backing_commit(&transaction);
	wait(&a);
	EXPECT_EQ(b->writer_count, 4096u);
	const auto content = b->content;
	EXPECT_EQ(submit(b, next, {{b, 64, 64}}), -28);
	EXPECT_EQ(b->content, content);
	EXPECT_EQ(b->writer_count, 4096u);
	EXPECT_EQ(timelines[key(&next)].refs, 0);
	EXPECT_EQ(complete(b, a, 0).current_count, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, WarmWritesReuseMetadataAndGpuOnlyBackingDoesNotAllocateIt)
{
	auto *b = backing(), *gpu = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	const auto *first = b->writers, *second = b->writer_next;
	for (unsigned value = 1; value <= 16; ++value) {
		const auto t = token(1, value);
		ASSERT_EQ(submit(b, t, {{b, 64, 64}, {b, 256, 64}}), 0);
		wait(&t);
		EXPECT_TRUE(b->writers == first || b->writers == second);
		EXPECT_TRUE(b->writer_next == first || b->writer_next == second);
	}
	ASSERT_EQ(submit(gpu, token(2), {{gpu, 64, 64}}), 0);
	EXPECT_EQ(gpu->writers, nullptr);
	EXPECT_EQ(gpu->writer_next, nullptr);
	switch_gpu_backing_release(b);
	switch_gpu_backing_release(gpu);
}

TEST_F(WriteRanges, EnablingTrackingAfterUnknownWriterKeepsWholeAuthority)
{
	auto *b = backing();
	const auto a = token(1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, a), 0);
	wait(&a);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	EXPECT_EQ(b->writer_count, 1u);
	EXPECT_EQ(b->writers[0].end, 4096u);
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, ExtendingBackingPreservesOldBitmapAndTracksNewPages)
{
	const uint64_t pages[] = {next_page += 4096, next_page += 4096};
	auto *b = switch_gpu_backing_register(pages, 1, 0, nullptr, nullptr);
	const auto a = token(1), next = token(2);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	ASSERT_EQ(submit(b, a, {{b, 64, 64}}), 0);
	wait(&a);
	ASSERT_EQ(complete(b, a, 64).current_count, 1u);
	auto *alias = switch_gpu_backing_register(pages, 2, 0, nullptr, nullptr);
	ASSERT_EQ(alias, b);
	ASSERT_EQ(submit(b, next, {{b, 4096, 64}}), 0);
	wait(&next);
	cache.clear();
	EXPECT_EQ(complete(b, a, 64).current_count, 1u);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(complete(b, next, 4096).current_count, 1u);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 4096u);
	switch_gpu_backing_release(alias);
	switch_gpu_backing_release(b);
}

TEST_F(WriteRanges, ImportedHistoryWithoutCurrentProducerRetainsConservativeMaintenance)
{
	auto *b = backing();
	const auto t = token(1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, t), 0);
	wait(&t);
	const uint32_t write = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 256, 64, write), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 256, 64, write), 0);
	ASSERT_TRUE(b->dirty & SWITCH_GPU_BACKING_GPU_DIRTY);
	ASSERT_EQ(switch_gpu_backing_cpu_track(b), 0);
	EXPECT_FALSE(b->cpu_tracking);
	cache.clear();
	EXPECT_EQ(complete(b, t, 64).stale, 1u);
	const uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 64, 64, read), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].offset, 64u);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 64, 64, read), 0);
	switch_gpu_backing_release(b);
}
}  // namespace
