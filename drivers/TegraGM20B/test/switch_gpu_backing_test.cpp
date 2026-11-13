// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"

TEST_F(Backing, TracksEveryWriterAndFailsClosedWhenDependencyArrayIsTooSmall)
{
	auto *allocation = backing();
	ASSERT_NE(allocation, nullptr);
	for (uint32_t i = 1; i <= 40; ++i)
		ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(i)), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(41);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	std::array<switch_gpu_backing_token, 40> dependencies;
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 16, &needed),
		  -28);
	EXPECT_EQ(needed, 40u);
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 40, &needed),
		  0);
	EXPECT_EQ(needed, 40u);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
	EXPECT_TRUE(retirement_scheduled);
}

TEST_F(Backing, WritersWaitForReadersButReadersDoNotWaitForEachOther)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), 0);
	for (uint32_t access : {SWITCH_GPU_BACKING_READ, SWITCH_GPU_BACKING_WRITE}) {
		switch_gpu_backing_use use = {allocation, access, 0};
		switch_gpu_backing_transaction transaction = {};
		const auto completion = token(2);
		ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
		switch_gpu_backing_token dependency;
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed),
			  0);
		EXPECT_EQ(needed, access == SWITCH_GPU_BACKING_WRITE ? 1u : 0u);
		switch_gpu_backing_abort(&transaction);
	}
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PartialCpuWriteInvalidatesBeforeCleanAndKeepsNeighboringGpuData)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].invalidate, 0);
	std::array<uint8_t, 128> memory{}, cpu{};
	for (size_t i = 0; i < memory.size(); ++i)
		memory[i] = uint8_t(i + 1);
	cpu.fill(0xee);
	cache_callback = [&](uint64_t offset, uint64_t bytes, int operation) {
		const uint64_t begin = offset & ~uint64_t(63),
			       end = (offset + bytes + 63) & ~uint64_t(63);
		ASSERT_LE(end, memory.size());
		for (uint64_t at = begin; at < end; ++at) {
			if (operation == SWITCH_GPU_CACHE_INVALIDATE)
				cpu[at] = memory[at];
			else
				memory[at] = cpu[at];
		}
	};
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 7, 2, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache[1].invalidate, 1);
	EXPECT_EQ(cache[1].offset, 0u);
	EXPECT_EQ(cache[1].bytes, 64u);
	cpu[7] = 0xa1;
	cpu[8] = 0xb2;
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 7, 2, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(cache.size(), 3u);
	EXPECT_EQ(cache[2].invalidate, 0);
	for (size_t i = 0; i < memory.size(); ++i)
		EXPECT_EQ(memory[i], i == 7 ? 0xa1 : i == 8 ? 0xb2 : uint8_t(i + 1));
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	EXPECT_EQ(cache.size(), 3u);
	cache_callback = {};
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuAvoidSyncAndActiveCpuOwnershipRefuseConflictingEngines)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
						 SWITCH_GPU_BACKING_READ |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuReadLeaseHandsOwnershipToThePublishedGpuReader)
{
	auto *allocation = backing();
	const uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	const uint32_t write = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, read), 0);
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, write), -16);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, read), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto writer = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &writer, &transaction), 0);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 1u);
	EXPECT_EQ(dependency.queue, reader.queue);
	EXPECT_EQ(dependency.value, reader.value);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedAvoidSyncReadLeavesNoCpuReservation)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
						 SWITCH_GPU_BACKING_READ |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PreserveDirtyOnlyCleansAfterTheNonblockingProducerCheck)
{
	auto *allocation = backing();
	const auto producer = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, producer), 0);
	const uint32_t access = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC |
				SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY;
	const size_t before = cache.size();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, access), -16);
	EXPECT_EQ(cache.size(), before);
	timelines[key(&producer)].completed = producer.value;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, access), 0);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_CLEAN_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, access), 0);
	const auto next = token(2);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, next), 0);
	timelines[key(&next)].completed = next.value;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PreserveDirtyRejectsOtherOwnershipModes)
{
	auto *allocation = backing();
	for (uint32_t access :
	     {uint32_t(SWITCH_GPU_BACKING_READ), uint32_t(SWITCH_GPU_BACKING_WRITE),
	      uint32_t(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC |
		       SWITCH_GPU_BACKING_CPU_NO_CACHE),
	      uint32_t(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC |
		       SWITCH_GPU_BACKING_CPU_CONCURRENT)})
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(
				  allocation, 0, 4096,
				  access | SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY),
			  -22);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ConversionUsesOnlyItsOwnedNativeAndPitchLeases)
{
	auto *native = backing();
	auto *pitch = backing();
	const auto producer = token(1), initialized = token(2);
	ASSERT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, producer), 0);
	ASSERT_EQ(publish(pitch, SWITCH_GPU_BACKING_READ, initialized), 0);
	timelines[key(&producer)].completed = producer.value;
	timelines[key(&initialized)].completed = initialized.value;
	const size_t cacheBefore = cache.size();
	switch_gpu_cpu_lease *nativeLease = nullptr, *pitchLease = nullptr;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(
			  native, 0, 4096,
			  SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE, &nativeLease),
		  0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(
			  pitch, 0, 4096, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE,
			  &pitchLease),
		  0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(native, &info);
	EXPECT_NE(info.dirty & SWITCH_GPU_BACKING_GPU_DIRTY, 0u);
	EXPECT_EQ(info.dirty & SWITCH_GPU_BACKING_CPU_DIRTY, 0u);
	switch_gpu_backing_use uses[] = {{native, SWITCH_GPU_BACKING_READ, 0},
					 {pitch, SWITCH_GPU_BACKING_WRITE, 0}};
	switch_gpu_cpu_lease *leases[] = {nativeLease, pitchLease};
	switch_gpu_backing_transaction transaction = {};
	const auto conversion = token(3, 1, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(
		switch_gpu_backing_begin_conversion(uses, 2, leases, 2, &conversion, &transaction),
		0);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(cache.size(), cacheBefore);
	EXPECT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, token(4)), -16);
	EXPECT_EQ(publish(pitch, SWITCH_GPU_BACKING_WRITE, token(4)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&nativeLease, 0), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&pitchLease, 0), 0);
	EXPECT_EQ(nativeLease, nullptr);
	EXPECT_EQ(pitchLease, nullptr);
	for (auto *allocation : {native, pitch}) {
		switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
		const auto consumer = token(4);
		ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &consumer, &transaction), 0);
		switch_gpu_backing_token dependency = {};
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed),
			  0);
		EXPECT_EQ(needed, 1u);
		EXPECT_EQ(dependency.engine, conversion.engine);
		EXPECT_EQ(dependency.queue, conversion.queue);
		switch_gpu_backing_abort(&transaction);
	}
	switch_gpu_backing_release(native);
	switch_gpu_backing_release(pitch);
}

TEST_F(Backing, ConversionDoesNotIgnoreAnotherCpuReader)
{
	auto *allocation = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	const uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, read, &lease), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, read), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	EXPECT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction),
		-16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, read), 0);
	ASSERT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction),
		0);
	switch_gpu_backing_abort(&transaction);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ConversionRejectsForeignDuplicateAndPartialLeases)
{
	auto *allocation = backing();
	auto *other = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	const uint32_t access = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, access, &lease), 0);
	switch_gpu_backing_use use = {other, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	EXPECT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction),
		-22);
	use.backing = allocation;
	switch_gpu_cpu_lease *duplicates[] = {lease, lease};
	EXPECT_EQ(switch_gpu_backing_begin_conversion(&use, 1, duplicates, 2, &completion,
						      &transaction),
		  -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 64, 64, access, &lease), 0);
	EXPECT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction),
		-22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, SWITCH_GPU_BACKING_READ,
						       &lease),
		  -22);
	EXPECT_EQ(lease, nullptr);
	switch_gpu_backing_release(allocation);
	switch_gpu_backing_release(other);
}

TEST_F(Backing, AbortedConversionKeepsItsCpuExclusion)
{
	auto *allocation = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(
			  allocation, 0, 4096,
			  SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE, &lease),
		  0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	ASSERT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction),
		0);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedOwnedAvoidSyncAcquisitionReturnsNoLease)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096,
						       SWITCH_GPU_BACKING_READ |
							       SWITCH_GPU_BACKING_CPU_NO_CACHE |
							       SWITCH_GPU_BACKING_CPU_AVOID_SYNC,
						       &lease),
		  -16);
	EXPECT_EQ(lease, nullptr);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailureAndResetGenerationCannotBeReportedAsCompletion)
{
	auto *allocation = backing();
	const auto first = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, first), 0);
	timelines[key(&first)].error = -5;
	const auto reset = token(1, 99, SWITCH_GPU_ENGINE_GM20B, 2);
	timelines[key(&reset)].completed = 99;
	const int logged = ioLogLines();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(ioLogLines(), logged + 1);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedReaderDoesNotInvalidateUnchangedContents)
{
	auto *allocation = backing();
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	timelines[key(&reader)].error = -5;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(3)), 0);
	EXPECT_EQ(timelines[key(&reader)].refs, 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedReaderFenceSurvivesPendingAcquiresWithoutPoisoningNewWork)
{
	auto *allocation = backing();
	const auto reader = token(1), pending = token(2), later = token(3);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, pending), 0);
	timelines[key(&reader)].error = -5;
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &later, &transaction), 0);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 1u);
	EXPECT_EQ(dependency.queue, pending.queue);
	EXPECT_EQ(timelines[key(&reader)].refs, 1);
	switch_gpu_backing_abort(&transaction);
	timelines[key(&pending)].completed = pending.value;
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, later), 0);
	EXPECT_EQ(timelines[key(&reader)].refs, 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedWriterRejectsNoUseAndRetiresOnceReplaced)
{
	auto *allocation = backing();
	const auto reader = token(1), writer = token(2);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&reader)].error = -5;
	timelines[key(&writer)].error = -5;
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(3)), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(3, 2)), 0);
	EXPECT_EQ(timelines[key(&reader)].refs, 0);
	EXPECT_EQ(timelines[key(&writer)].refs, 1);
	complete(token(3, 2));
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(timelines[key(&writer)].refs, 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CompletionRecordsAndPinnedOwnerOutliveLastClientLease)
{
	const uint64_t page = next_page += 4096;
	int releases = 0;
	auto *allocation = switch_gpu_backing_register(&page, 1, 0, &releases, [](void *owner) {
		++*static_cast<int *>(owner);
	});
	const auto completion = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, completion), 0);
	switch_gpu_backing_release(allocation);
	EXPECT_EQ(releases, 0);
	EXPECT_EQ(timelines[key(&completion)].refs, 2);
	switch_gpu_backing_retire();
	EXPECT_EQ(releases, 0);
	timelines[key(&completion)].completed = completion.value;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(releases, 1);
	EXPECT_EQ(timelines[key(&completion)].refs, 0);
	EXPECT_EQ(switch_gpu_backing_find_pa(page), nullptr);
}

TEST_F(Backing, RegistrationRetainsIdentityAndExtendingPagesChangesGeneration)
{
	const uint64_t pages[] = {next_page += 4096, next_page += 4096};
	auto *first = switch_gpu_backing_register(pages, 1, 500, nullptr, nullptr);
	switch_gpu_backing_info before = {}, after = {};
	switch_gpu_backing_describe(first, &before);
	auto *second = switch_gpu_backing_register(pages, 2, 500, nullptr, nullptr);
	ASSERT_EQ(first, second);
	switch_gpu_backing_describe(second, &after);
	EXPECT_EQ(before.allocation_id, after.allocation_id);
	EXPECT_NE(before.allocation_generation, after.allocation_generation);
	EXPECT_EQ(before.pages[0], pages[0]);
	EXPECT_EQ(after.page_count, 2u);
	auto *by_surface = switch_gpu_backing_find_surface(500);
	EXPECT_EQ(by_surface, first);
	switch_gpu_backing_release(by_surface);
	switch_gpu_backing_release(first);
	switch_gpu_backing_release(second);
}

TEST_F(Backing, DuplicateUsesMergeAndAbortBalancesPreparedReferences)
{
	auto *one = backing(), *two = backing();
	switch_gpu_backing_use uses[] = {{two, SWITCH_GPU_BACKING_READ, 0},
					 {one, SWITCH_GPU_BACKING_READ, 0},
					 {two, SWITCH_GPU_BACKING_WRITE, 0}};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	ASSERT_EQ(switch_gpu_backing_begin(uses, 3, &completion, &transaction), 0);
	EXPECT_EQ(transaction.count, 2u);
	EXPECT_EQ(timelines[key(&completion)].refs, 2);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(timelines[key(&completion)].refs, 0);
	switch_gpu_backing_release(one);
	switch_gpu_backing_release(two);
}

TEST_F(Backing, Host1xWrapAdvancesOnTheSameTimeline)
{
	auto *allocation = backing();
	const auto before_wrap = token(1, 0xfffffff0u, SWITCH_GPU_ENGINE_VIC);
	timelines[key(&before_wrap)].completed = 0xffffff00u;
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, before_wrap), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1, 5, SWITCH_GPU_ENGINE_VIC)),
		  0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1, 4, SWITCH_GPU_ENGINE_VIC)),
		  -22);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, DiscardRequiresFullCacheLinesAndLayoutRequiresPaddedBlockExtent)
{
	auto *allocation = backing();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 1, 64,
						 SWITCH_GPU_BACKING_WRITE |
							 SWITCH_GPU_BACKING_CPU_DISCARD),
		  -22);
	switch_gpu_backing_layout layout = {};
	layout.plane_count = 1;
	layout.planes[0] = {0, 4096, 64, 9, 256, 4, SWITCH_GPU_BACKING_BLOCK_LINEAR, 2};
	EXPECT_EQ(switch_gpu_backing_set_layout(allocation, &layout), -22);
	layout.planes[0].block_height_log2 = 1;
	EXPECT_EQ(switch_gpu_backing_set_layout(allocation, &layout), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, SurfaceUnlockCleansBeforeSeedAndKeepsOwnershipUntilAppleSucceeds)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 700, nullptr, nullptr);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 700, 0), 0);
	switch_gpu_surface_lock_did(&client, 700, 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 700, 1), 0);
	switch_gpu_surface_lock_did(&client, 700, 1);
	const size_t before = cache.size();
	switch_gpu_surface_unlock_will(&client, 700, 0);
	EXPECT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	switch_gpu_surface_unlock_did(&client, 700, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	switch_gpu_surface_unlock_will(&client, 700, 1);
	switch_gpu_surface_unlock_did(&client, 700, 1);
	EXPECT_EQ(cache.size(), before + 1);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedAppleLockUnwindsOnlyItsPendingReservation)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 701, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 701, 0), 0);
	switch_gpu_surface_lock_did(&client, 701, 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 701, 1), 0);
	switch_gpu_surface_lock_failed(&client, 701, 1);
	switch_gpu_surface_lock_failed(&client, 701, 1);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), -16);
	switch_gpu_surface_unlock_will(&client, 701, 0);
	switch_gpu_surface_unlock_did(&client, 701, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, DeadSurfaceClientReleasesCpuReservationsBeforeNextEngineSubmission)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 702, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 702, 0), 0);
	switch_gpu_surface_lock_did(&client, 702, 0);
	client.inactive = true;
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

extern uint32_t switch_gpu_cpu_sleepers;
extern int switch_gpu_test_ready_waiting;
void switch_gpu_backing_sort_uses(switch_gpu_backing_use *uses, uint32_t count);

// Lock order: a job's uses by backing id, in place, whether they came sorted or not.
TEST_F(Backing, UsesAreOrderedByBackingWhetherOrNotTheyCameSorted)
{
	switch_gpu_backing *made[6];
	for (auto *&allocation : made)
		allocation = backing();
	switch_gpu_backing_use uses[6];
	for (uint32_t i = 0; i < 6; ++i)
		uses[i] = {made[i], SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_sort_uses(uses, 6);
	for (uint32_t i = 0; i < 6; ++i)
		EXPECT_EQ(uses[i].backing, made[i]);
	const uint32_t order[6] = {4, 0, 5, 2, 2, 1};
	for (uint32_t i = 0; i < 6; ++i)
		uses[i] = {made[order[i]], SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_sort_uses(uses, 6);
	const auto id = [](switch_gpu_backing *allocation) {
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(allocation, &info);
		return info.allocation_id;
	};
	for (uint32_t i = 1; i < 6; ++i)
		EXPECT_LE(id(uses[i - 1].backing), id(uses[i].backing));
	for (auto *allocation : made)
		switch_gpu_backing_release(allocation);
}

// The sweep a dead client's leases wait for runs only for a job they could refuse or for a waiter they could hold.
TEST_F(Backing, ADeadClientsLeaseIsSweptByAJobOnItsBackingNotByEveryJob)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *held = switch_gpu_backing_register(&page, 1, 704, nullptr, nullptr);
	auto *other = backing();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 704, 0), 0);
	switch_gpu_surface_lock_did(&client, 704, 0);
	client.inactive = true;
	EXPECT_EQ(publish(other, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(client.refs, 2u);
	EXPECT_EQ(publish(held, SWITCH_GPU_BACKING_WRITE, token(1, 2)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(other);
	switch_gpu_backing_release(held);
}

TEST_F(Backing, AReadinessWatchGetsADeadClientsLeaseSweptByAnyJob)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *held = switch_gpu_backing_register(&page, 1, 705, nullptr, nullptr);
	auto *other = backing();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 705, 0), 0);
	switch_gpu_surface_lock_did(&client, 705, 0);
	client.inactive = true;
	switch_gpu_test_ready_waiting = 1;
	EXPECT_EQ(publish(other, SWITCH_GPU_BACKING_READ, token(1)), 0);
	switch_gpu_test_ready_waiting = 0;
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(other);
	switch_gpu_backing_release(held);
}

TEST_F(Backing, ACpuWaiterBehindADeadClientsLeaseIsWokenByAnUnrelatedJob)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *held = switch_gpu_backing_register(&page, 1, 706, nullptr, nullptr);
	auto *other = backing();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 706, 0), 0);
	switch_gpu_surface_lock_did(&client, 706, 0);
	client.inactive = true;
	const uint64_t bytes = 4096;
	std::thread waiter([&] {
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(held, 0, bytes, SWITCH_GPU_BACKING_WRITE),
			  0);
		EXPECT_EQ(switch_gpu_backing_cpu_release(held, 0, bytes, SWITCH_GPU_BACKING_WRITE),
			  0);
	});
	while (!__atomic_load_n(&switch_gpu_cpu_sleepers, __ATOMIC_ACQUIRE))
		std::this_thread::yield();
	EXPECT_EQ(publish(other, SWITCH_GPU_BACKING_READ, token(1)), 0);
	waiter.join();
	EXPECT_EQ(client.refs, 1u);
	EXPECT_EQ(switch_gpu_cpu_sleepers, 0u);
	switch_gpu_backing_release(other);
	switch_gpu_backing_release(held);
}

TEST_F(Backing, ExtendingGpuDirtyBackingCleansOnlyTheNewPages)
{
	const uint64_t pages[] = {next_page += 4096, next_page += 4096};
	auto *allocation = switch_gpu_backing_register(pages, 1, 0, nullptr, nullptr);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	auto *extended = switch_gpu_backing_register(pages, 2, 0, nullptr, nullptr);
	ASSERT_EQ(allocation, extended);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	ASSERT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache[1].offset, 4096u);
	EXPECT_EQ(cache[1].bytes, 4096u);
	switch_gpu_backing_release(extended);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, RefusedTrapCannotCancelAnotherThreadsPendingLock)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 703, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 703, 0), 0);
	std::thread refused([&] {
		switch_gpu_surface_lock_failed(&client, 703, 0);
	});
	refused.join();
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), -16);
	switch_gpu_surface_lock_did(&client, 703, 0);
	switch_gpu_surface_unlock_will(&client, 703, 0);
	switch_gpu_surface_unlock_did(&client, 703, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ImmutableExportPreservesPendingProducerWithoutCpuWait)
{
	auto *allocation = backing();
	const auto writer = token(71, 9);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	ASSERT_EQ(switch_gpu_backing_export_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.value, writer.value);
	EXPECT_EQ(producer.queue, writer.queue);
	EXPECT_EQ(timelines[key(&writer)].completed, 0u);
	EXPECT_EQ(timelines[key(&writer)].refs, 3);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(72)), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
						 SWITCH_GPU_BACKING_WRITE |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto reader = token(73);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &reader, &transaction), 0);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 1u);
	EXPECT_EQ(dependency.queue, writer.queue);
	switch_gpu_backing_commit(&transaction);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(timelines[key(&writer)].refs, 2);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(74)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CompletedProducerRemainsIdentifiableAfterReaderPruning)
{
	auto *allocation = backing();
	const auto writer = token(75, 3);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].completed = writer.value;
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(76)), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	ASSERT_EQ(switch_gpu_backing_export_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.queue, writer.queue);
	EXPECT_EQ(producer.value, writer.value);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuWriteInvalidatesTheOldExportProducer)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(77)), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	EXPECT_EQ(switch_gpu_backing_export_read(allocation, &producer, &lease),
		  SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(lease, nullptr);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(78)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedProducerExportsUndefinedContentsWithNothingToWaitFor)
{
	auto *allocation = backing();
	const auto writer = token(79);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].error = -5;
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = token(5);
	EXPECT_EQ(switch_gpu_backing_export_read(allocation, &producer, &lease), 0);
	EXPECT_NE(lease, nullptr);
	EXPECT_EQ(producer.engine, 0u);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 1u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ImmutableExportCannotBeDiscountedToAuthorizeAConversionWrite)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(80)), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	ASSERT_EQ(switch_gpu_backing_export_read(allocation, &producer, &lease), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto conversion = token(81);
	EXPECT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &conversion, &transaction),
		-22);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, EngineConversionReservationPreservesPendingGpuDependenciesWithoutCpuWait)
{
	auto *allocation = backing();
	const auto writer = token(82), reader = token(83), conversion = token(84);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	const auto cleans = cache.size();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(allocation, SWITCH_GPU_BACKING_WRITE, 0,
							&lease),
		  0);
	EXPECT_EQ(timelines[key(&writer)].completed, 0u);
	EXPECT_EQ(timelines[key(&reader)].completed, 0u);
	EXPECT_EQ(cache.size(), cleans);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(85)), -16);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(
		switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &conversion, &transaction),
		0);
	switch_gpu_backing_token dependencies[2] = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies, 2, &needed), 0);
	EXPECT_EQ(needed, 2u);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(86)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, EngineConversionReservationCannotBypassCpuOwnership)
{
	auto *allocation = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	EXPECT_EQ(switch_gpu_backing_conversion_reserve(allocation, SWITCH_GPU_BACKING_WRITE, 1,
							&lease),
		  -16);
	EXPECT_EQ(lease, nullptr);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 1u);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(allocation, SWITCH_GPU_BACKING_WRITE, 1,
							&lease),
		  0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, EngineConversionReservationGoesOnOverFailedContents)
{
	auto *allocation = backing();
	const auto writer = token(87);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].error = -5;
	switch_gpu_cpu_lease *lease = nullptr;
	EXPECT_EQ(switch_gpu_backing_conversion_reserve(allocation, SWITCH_GPU_BACKING_WRITE, 0,
							&lease),
		  0);
	EXPECT_NE(lease, nullptr);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, EngineOnlyReservationStillWaitsAndInvalidatesBeforeCpuExposure)
{
	auto *allocation = backing();
	const auto writer = token(88);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(allocation, SWITCH_GPU_BACKING_READ, 0,
							&lease),
		  0);
	EXPECT_EQ(timelines[key(&writer)].completed, 0u);
	ASSERT_EQ(
		switch_gpu_backing_cpu_transfer_owned(allocation, &lease, SWITCH_GPU_BACKING_READ),
		0);
	EXPECT_EQ(lease, nullptr);
	EXPECT_EQ(timelines[key(&writer)].completed, writer.value);
	ASSERT_FALSE(cache.empty());
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PhysicalSnapshotCleansCpuContentAndExcludesWriters)
{
	auto *allocation = backing(2);
	cache.clear();
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = token(99);
	ASSERT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.engine, 0u);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].invalidate, SWITCH_GPU_CACHE_CLEAN);
	EXPECT_EQ(cache[0].bytes, 8192u);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(99)), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	cache.clear();
	ASSERT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), 0);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PhysicalSnapshotRetainsPendingWriterWithoutCpuWait)
{
	auto *allocation = backing();
	const auto writer = token(101, 22);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	cache.clear();
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	ASSERT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.value, writer.value);
	EXPECT_EQ(producer.queue, writer.queue);
	EXPECT_EQ(timelines[key(&writer)].completed, 0u);
	EXPECT_TRUE(cache.empty());
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PhysicalSnapshotRefusesCpuWritersAndGoesOnOverFailedGpuWriters)
{
	auto *allocation = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	EXPECT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), -16);
	EXPECT_EQ(lease, nullptr);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.engine, 0u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	const auto writer = token(102);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].error = -5;
	producer = writer;
	EXPECT_EQ(switch_gpu_backing_snapshot_read(allocation, &producer, &lease), 0);
	EXPECT_EQ(producer.engine, 0u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PhysicalSnapshotWaitSleepsOnlyForCpuOwner)
{
	auto *allocation = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	switch_gpu_backing_token producer = {};
	auto acquire = std::async(std::launch::async, [&] {
		return switch_gpu_backing_snapshot_read_wait(allocation, &producer, &lease);
	});
	EXPECT_EQ(acquire.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(acquire.get(), 0);
	EXPECT_EQ(producer.engine, 0u);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(103)), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, NativeViewReadPublishesBothAuthorityAndPhysicalLifetime)
{
	auto *logical = backing(), *native = backing();
	const auto producer = token(104, 7, SWITCH_GPU_ENGINE_NVDEC);
	ASSERT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, producer), 0);
	volatile uint64_t payload = 1, representation = 3;
	switch_gpu_native_surface_view view = {};
	view.logical = logical;
	view.native = native;
	view.payload = {logical, &payload, 1};
	view.representation = {logical, &representation, 3};
	switch_gpu_backing_use uses[2] = {};
	switch_gpu_backing_guard guards[2] = {};
	uint32_t useCount = 0, guardCount = 0;
	ASSERT_EQ(switch_gpu_native_surface_add_read(view, SWITCH_GPU_BACKING_READ, uses, guards, 2,
						     useCount, guardCount),
		  0);
	switch_gpu_backing_transaction transaction = {};
	const auto consumer = token(105);
	ASSERT_EQ(switch_gpu_backing_begin_checked(uses, useCount, guards, guardCount, &consumer,
						   &transaction),
		  0);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	ASSERT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 1u);
	EXPECT_EQ(dependency.queue, producer.queue);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(logical, 0, 4096,
						 SWITCH_GPU_BACKING_WRITE |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(native, 0, 4096,
						 SWITCH_GPU_BACKING_WRITE |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	representation = 4;
	const auto stale = token(106);
	EXPECT_EQ(switch_gpu_backing_begin_checked(uses, useCount, guards, guardCount, &stale,
						   &transaction),
		  SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(timelines[key(&stale)].refs, 0);
	switch_gpu_backing_release(logical);
	switch_gpu_backing_release(native);
}

TEST_F(Backing, PitchProxyRequiresItsGenerationAndNeverAuthorizesWrites)
{
	auto *logical = backing();
	volatile uint64_t representation = 4;
	switch_gpu_native_surface_view view = {};
	view.logical = view.native = logical;
	view.representation = {logical, &representation, 4};
	switch_gpu_backing_use uses[2] = {};
	switch_gpu_backing_guard guards[2] = {};
	uint32_t useCount = 0, guardCount = 0;
	EXPECT_EQ(switch_gpu_native_surface_add_read(view, SWITCH_GPU_BACKING_WRITE, uses, guards,
						     2, useCount, guardCount),
		  -22);
	EXPECT_EQ(useCount, 0u);
	EXPECT_EQ(guardCount, 0u);
	EXPECT_EQ(switch_gpu_native_surface_add_read(view, SWITCH_GPU_BACKING_READ, uses, guards, 1,
						     useCount, guardCount),
		  -28);
	EXPECT_EQ(useCount, 0u);
	EXPECT_EQ(guardCount, 0u);
	ASSERT_EQ(switch_gpu_native_surface_add_read(view, SWITCH_GPU_BACKING_READ, uses, guards, 2,
						     useCount, guardCount),
		  0);
	EXPECT_EQ(guardCount, 1u);
	representation = 5;
	switch_gpu_backing_transaction transaction = {};
	const auto consumer = token(107);
	EXPECT_EQ(switch_gpu_backing_begin_checked(uses, useCount, guards, guardCount, &consumer,
						   &transaction),
		  SWITCH_GPU_BACKING_STALE);
	switch_gpu_backing_release(logical);
}

namespace
{
bool hostAcquire(switch_gpu_backing *allocation, switch_gpu_backing_token completion)
{
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction{};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	const bool needed = transaction.host_written;
	switch_gpu_backing_commit(&transaction);
	return needed;
}

void installVisibility()
{
	const switch_gpu_backing_visibility ops = {external_status, external_wait};
	const int result = switch_gpu_backing_register_visibility(SWITCH_GPU_ENGINE_GM20B, &ops);
	ASSERT_TRUE(result == 0 || result == -16);
}
}

TEST_F(Backing, InitialHostEpochWaitsForTheFirstSuccessfulAcquireEvenWithNewerWorkQueued)
{
	auto *allocation = backing();
	const auto first = token(301, 1), second = token(301, 2), third = token(301, 3);
	EXPECT_TRUE(hostAcquire(allocation, first));
	EXPECT_TRUE(hostAcquire(allocation, second));
	timelines[key(&first)].completed = 1;
	EXPECT_FALSE(hostAcquire(allocation, third));
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuCleanPublicationStillRequiresANewGpuAcquire)
{
	auto *allocation = backing();
	const auto first = token(302, 1);
	EXPECT_TRUE(hostAcquire(allocation, first));
	timelines[key(&first)].completed = 1;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	EXPECT_TRUE(hostAcquire(allocation, token(302, 2)));
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ExternalEngineWriterRequiresGpuCacheAcquire)
{
	auto *allocation = backing();
	const auto first = token(303, 1), vic = token(304, 1, SWITCH_GPU_ENGINE_VIC);
	EXPECT_TRUE(hostAcquire(allocation, first));
	timelines[key(&first)].completed = 1;
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, vic), 0);
	timelines[key(&vic)].completed = 1;
	EXPECT_TRUE(hostAcquire(allocation, token(303, 2)));
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedHostAcquireDoesNotAcknowledgeTheMemoryEpoch)
{
	auto *allocation = backing();
	const auto first = token(305, 1);
	EXPECT_TRUE(hostAcquire(allocation, first));
	timelines[key(&first)].error = -5;
	EXPECT_TRUE(hostAcquire(allocation, token(306, 1)));
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuAvoidSyncRejectsExecutedButNotExternallyVisibleWritesWithoutLeaking)
{
	installVisibility();
	auto *allocation = backing();
	const auto writer = token(307, 1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].completed = 1;
	external_completed[key(&writer)] = 0;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
						 SWITCH_GPU_BACKING_READ |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(308)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuReadPromotesVisibilityBeforeInvalidating)
{
	installVisibility();
	auto *allocation = backing();
	const auto writer = token(309, 1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].completed = 1;
	external_completed[key(&writer)] = 0;
	const auto before = external_waits;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(external_waits, before + 1);
	ASSERT_FALSE(cache.empty());
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, AKernelCpuMoveWaitsForExecutedInternalWritesWithoutTakingOwnership)
{
	installVisibility();
	auto *allocation = backing();
	const auto writer = token(312, 1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].completed = 1;
	external_completed[key(&writer)] = 0;
	const auto before = external_waits;
	EXPECT_EQ(switch_gpu_backing_wait_visible(allocation), 0);
	EXPECT_EQ(external_waits, before + 1);
	EXPECT_EQ(switch_gpu_backing_wait_visible(allocation), 0);
	EXPECT_EQ(external_waits, before + 1);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(313)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, VicDependsOnExecutedInternalGpuWritesUntilExternalVisibility)
{
	installVisibility();
	auto *allocation = backing();
	const auto writer = token(310, 1), vic = token(311, 1, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, writer), 0);
	timelines[key(&writer)].completed = 1;
	external_completed[key(&writer)] = 0;
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction{};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &vic, &transaction), 0);
	switch_gpu_backing_token dependency{};
	uint32_t count = 0;
	ASSERT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &count), 0);
	ASSERT_EQ(count, 1u);
	EXPECT_EQ(dependency.queue, writer.queue);
	switch_gpu_backing_abort(&transaction);
	switch_gpu_backing_release(allocation);
}

// NVDEC's surface recycling (SwitchNVDECOutput.cpp) reuses a buffer in place when only engines its job dependencies
// order still use it, and replaces it while a CPU owner (an export's lease) holds it: this names both without waiting.
TEST_F(Backing, PendingNamesTheEnginesAUseWouldFollowAndTheCpuOwners)
{
	auto *allocation = backing();
	uint32_t engines = ~0u, owners = ~0u;
	ASSERT_EQ(
		switch_gpu_backing_pending(allocation, SWITCH_GPU_BACKING_WRITE, &engines, &owners),
		0);
	EXPECT_EQ(engines, 0u);
	EXPECT_EQ(owners, 0u);
	const auto decode = token(4, 1, SWITCH_GPU_ENGINE_NVDEC);
	const auto present = token(5, 1, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, decode), 0);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, present), 0);
	ASSERT_EQ(
		switch_gpu_backing_pending(allocation, SWITCH_GPU_BACKING_WRITE, &engines, &owners),
		0);
	EXPECT_EQ(engines, (1u << SWITCH_GPU_ENGINE_NVDEC) | (1u << SWITCH_GPU_ENGINE_VIC));
	ASSERT_EQ(
		switch_gpu_backing_pending(allocation, SWITCH_GPU_BACKING_READ, &engines, &owners),
		0);
	EXPECT_EQ(engines, 1u << SWITCH_GPU_ENGINE_NVDEC) << "a reader follows writers only";
	complete(decode);
	complete(present);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	ASSERT_EQ(
		switch_gpu_backing_pending(allocation, SWITCH_GPU_BACKING_WRITE, &engines, &owners),
		0);
	EXPECT_EQ(engines, 0u);
	EXPECT_EQ(owners, 1u);
	EXPECT_EQ(switch_gpu_backing_pending(allocation, SWITCH_GPU_BACKING_CPU_AVOID_SYNC,
					     &engines, &owners),
		  -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}
