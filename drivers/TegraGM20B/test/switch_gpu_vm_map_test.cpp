// SPDX-License-Identifier: MIT
#include "switch_gpu_vm_map_stubs.h"

class VmMap : public testing::Test
{
protected:
	tgm_vm a{}, b{};
	tgm_mem memory{};
	std::vector<u64> pages;

	void SetUp() override
	{
		for (unsigned i = 0; i < 256; ++i)
			tgm_gpu.mmu.kind[i] = u8(i);
		a.owner = &a;
		b.owner = &b;
		a.vmm.id = 1;
		b.vmm.id = 2;
		tgm_tlb_space_init(&a.tlb);
		tgm_tlb_space_init(&b.tlb);
		pages.resize(35);
		for (unsigned i = 0; i < pages.size(); ++i)
			pages[i] = 0x80000000 + i * PAGE_SIZE;
		memory = {{12, pages.size() * PAGE_SIZE},
			  pages.size() * PAGE_SIZE,
			  12,
			  pages.data(),
			  u32(pages.size()),
			  nullptr,
			  false};
		calls.clear();
		mapHook = {};
		failGet = failMap = -1;
		getCount = mapCount = invalidations = constructors = flushes = 0;
		gateInside = gateWaiting = 0;
		gateShut = false;
	}

	void TearDown() override
	{
		EXPECT_EQ(gateInside, 0u);
		EXPECT_FALSE(ordered.active(owner()));
		while (memory.big_views) {
			auto *view = memory.big_views;
			memory.big_views = view->next;
			free(view);
		}
		for (void *node : a.kmaps.nodes)
			free(node);
	}

	int map(tgm_vm &vm, switch_gpu_mapping **result)
	{
		return switch_gpu_vm_map(&vm, 0x40000000, memory.bytes, &memory, 0, 0, 0, result);
	}
};

TEST_F(VmMap, MixedPagesUsePreparedViewsAndRetireSynchronously)
{
	mem_prepare_big_views(&memory);
	ASSERT_EQ(constructors, 1);
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(map(a, &mapping), 0);
	ASSERT_EQ(mapping->count, 2u);
	EXPECT_EQ(mapping->entries[0].shift, 17);
	EXPECT_EQ(mapping->entries[1].shift, 12);
	EXPECT_EQ(constructors, 1);
	EXPECT_EQ(a.tlb.gen, 2u);
	for (const auto &call : calls)
		EXPECT_TRUE(call.deferred);
	switch_gpu_vm_unmap(&a, &mapping);
	EXPECT_EQ(flushes, 1u);
	EXPECT_EQ(mapping, nullptr);
	ASSERT_EQ(calls.size(), 8u);
	for (size_t i = 4; i < calls.size(); ++i)
		EXPECT_FALSE(calls[i].deferred);
}

TEST_F(VmMap, MissingBigViewFallsBackWithoutConstructorInMap)
{
	memory.big_views_ready = true;
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(map(a, &mapping), 0);
	ASSERT_EQ(mapping->count, 2u);
	EXPECT_EQ(mapping->entries[0].shift, 12);
	EXPECT_EQ(constructors, 0);
	switch_gpu_vm_unmap(&a, &mapping);
}

TEST_F(VmMap, FailedMapRollsBackEveryReservationOutsideDeferredScope)
{
	mem_prepare_big_views(&memory);
	failMap = 1;
	switch_gpu_mapping *mapping = nullptr;
	EXPECT_EQ(map(a, &mapping), -5);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_EQ(a.tlb.gen, 1u);
	ASSERT_EQ(calls.size(), 8u);
	for (size_t i = 4; i < calls.size(); ++i)
		EXPECT_FALSE(calls[i].deferred);
	EXPECT_EQ(calls[4].operation, 'u');
	EXPECT_EQ(calls[6].operation, 'u');
}

TEST_F(VmMap, FailedGetRetiresPrefixAndInvalidatesDroppedPageTables)
{
	mem_prepare_big_views(&memory);
	failGet = 1;
	switch_gpu_mapping *mapping = nullptr;
	EXPECT_EQ(map(a, &mapping), -12);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_EQ(invalidations, 1);
	EXPECT_EQ(a.tlb.gen, 1u);
	ASSERT_EQ(calls.size(), 5u);
	EXPECT_EQ(calls[3].operation, 'u');
	EXPECT_EQ(calls[4].operation, 'p');
}

TEST_F(VmMap, IndependentAddressSpacesMapWhileOneIsBlocked)
{
	memory.big_views_ready = true;
	std::promise<void> entered, leave;
	auto leaveFuture = leave.get_future().share();
	std::atomic<bool> first = true;
	mapHook = [&](unsigned vm) {
		if (vm == 1 && first.exchange(false)) {
			entered.set_value();
			leaveFuture.wait();
		}
	};
	switch_gpu_mapping *firstMap = nullptr, *secondMap = nullptr;
	auto one = std::async(std::launch::async, [&] {
		return map(a, &firstMap);
	});
	entered.get_future().wait();
	auto two = std::async(std::launch::async, [&] {
		return map(b, &secondMap);
	});
	EXPECT_EQ(two.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	leave.set_value();
	EXPECT_EQ(one.get(), 0);
	EXPECT_EQ(two.get(), 0);
	for (const auto &call : calls)
		EXPECT_TRUE(call.deferred);
	switch_gpu_vm_unmap(&a, &firstMap);
	switch_gpu_vm_unmap(&b, &secondMap);
}

TEST_F(VmMap, SameAddressSpaceSerializesWholeMappingBatch)
{
	memory.big_views_ready = true;
	std::promise<void> entered, leave;
	auto leaveFuture = leave.get_future().share();
	std::atomic<bool> first = true;
	mapHook = [&](unsigned) {
		if (first.exchange(false)) {
			entered.set_value();
			leaveFuture.wait();
		}
	};
	switch_gpu_mapping *firstMap = nullptr, *secondMap = nullptr;
	auto one = std::async(std::launch::async, [&] {
		return map(a, &firstMap);
	});
	entered.get_future().wait();
	auto two = std::async(std::launch::async, [&] {
		return map(a, &secondMap);
	});
	EXPECT_EQ(two.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	leave.set_value();
	EXPECT_EQ(one.get(), 0);
	EXPECT_EQ(two.get(), 0);
	switch_gpu_vm_unmap(&a, &firstMap);
	switch_gpu_vm_unmap(&a, &secondMap);
}

TEST_F(VmMap, ClosedSleepGateDoesNotHoldAddressSpaceMutex)
{
	gateShut = true;
	std::promise<void> launched;
	auto one = std::async(std::launch::async, [&] {
		launched.set_value();
		return tgm_vm_unmap(&a, 0x40000000, 4096);
	});
	launched.get_future().wait();
	EXPECT_EQ(one.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	EXPECT_TRUE(mutex_available(&a.map_lock));
	{
		std::lock_guard held(gateMutex);
		gateShut = false;
		gateCv.notify_all();
	}
	EXPECT_EQ(one.get(), 0);
}

TEST_F(VmMap, OrdinaryAndKernelMappingsKeepSynchronousVisibility)
{
	u8 shift = 0;
	ASSERT_EQ(tgm_vm_map(&a, 0x40000000, 4096, &memory, 0, 0, 0), 0);
	EXPECT_FALSE(calls.back().deferred);
	ASSERT_EQ(tgm_vm_map_ordered(&a, 0x40001000, 4096, &memory, 4096, 0, 0, &shift), 0);
	EXPECT_EQ(shift, 12);
	EXPECT_TRUE(calls.back().deferred);
	u64 va = 0;
	ASSERT_EQ(tgm_vm_kmap(&a, &memory, 4096, &va), 0);
	EXPECT_EQ(va, TGM_KERNEL_VA);
	EXPECT_FALSE(calls.back().deferred);
	EXPECT_EQ(a.kmaps.nodes.size(), 1u);
}

TEST_F(VmMap, PhysicalInteriorUsesItsOwnBigViewOffset)
{
	pages.resize(64);
	for (unsigned i = 0; i < pages.size(); ++i)
		pages[i] = 0x80010000 + i * PAGE_SIZE;
	memory.pages = pages.data();
	memory.npages = u32(pages.size());
	memory.bytes = pages.size() * PAGE_SIZE;
	memory.mem.bytes = memory.bytes;
	mem_prepare_big_views(&memory);
	ASSERT_NE(memory.big_views, nullptr);
	EXPECT_EQ(memory.big_views->offset, 16 * PAGE_SIZE);
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_map(&a, 0x40000000, 48 * PAGE_SIZE, &memory, 16 * PAGE_SIZE, 0, 0,
				    &mapping),
		  0);
	ASSERT_EQ(mapping->count, 2u);
	EXPECT_EQ(mapping->entries[0].shift, 17);
	ASSERT_EQ(calls.size(), 4u);
	EXPECT_EQ(calls[1].offset, 0u);
	EXPECT_EQ(calls[3].offset, 48 * PAGE_SIZE);
	switch_gpu_vm_unmap(&a, &mapping);
}

TEST_F(VmMap, CompressedAliasesInDifferentSpacesUseOnlyCanonicalMemory)
{
	memory.page = memory.mem.shift = 17;
	memory.bytes = memory.mem.bytes = 32 * PAGE_SIZE;
	memory.npages = 32;
	tgm_gpu.mmu.kind[0x42] = 0xfe;
	mem_prepare_big_views(&memory);
	EXPECT_EQ(memory.big_views, nullptr);
	EXPECT_EQ(constructors, 0);
	switch_gpu_mapping *one = nullptr, *two = nullptr;
	ASSERT_EQ(switch_gpu_vm_map(&a, 0x40000000, memory.bytes, &memory, 0, 0x42, 0, &one), 0);
	ASSERT_EQ(switch_gpu_vm_map(&b, 0x60000000, memory.bytes, &memory, 0, 0x42, 0, &two), 0);
	unsigned maps = 0;
	for (const auto &call : calls) {
		if (call.operation == 'm') {
			EXPECT_EQ(call.memory, &memory.mem);
			EXPECT_EQ(call.shift, 17);
			++maps;
		}
	}
	EXPECT_EQ(maps, 2u);
	switch_gpu_vm_unmap(&a, &one);
	switch_gpu_vm_unmap(&b, &two);
}

TEST_F(VmMap, PreparedUncompressedBigViewCannotBecomeIndependentCompressionAuthority)
{
	mem_prepare_big_views(&memory);
	ASSERT_NE(memory.big_views, nullptr);
	ASSERT_EQ(memory.big_views->mem.shift, 17);
	tgm_gpu.mmu.kind[0x42] = 0xfe;
	switch_gpu_mapping *mapping = nullptr;
	EXPECT_EQ(switch_gpu_vm_map(&a, 0x40000000, 32 * PAGE_SIZE, &memory, 0, 0x42, 0, &mapping),
		  -EINVAL);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_TRUE(calls.empty());
}

// A sub-range Unmap or Map of one binding takes only that range: PTEs outside it keep their values.
TEST_F(VmMap, SplitOfA4KRunKeepsEveryPteOutsideTheRange)
{
	switch_gpu_mapping *mapping = nullptr, *below = nullptr, *taken = nullptr, *above = nullptr;
	ASSERT_EQ(switch_gpu_vm_map(&a, 0x40000000, 8 * PAGE_SIZE, &memory, 0, 0, 0, &mapping), 0);
	ASSERT_EQ(mapping->count, 1u);
	calls.clear();
	ASSERT_EQ(switch_gpu_vm_split(&a, &mapping, &memory, 0, 0, 0, 0x40002000, 0x2000, &below,
				      &taken, &above),
		  0);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_TRUE(calls.empty());
	u64 va = 0, bytes = 0;
	ASSERT_EQ(switch_gpu_mapping_span(below, &va, &bytes), 0);
	EXPECT_EQ(va, 0x40000000u);
	EXPECT_EQ(bytes, 0x2000u);
	ASSERT_EQ(switch_gpu_mapping_span(above, &va, &bytes), 0);
	EXPECT_EQ(va, 0x40004000u);
	EXPECT_EQ(bytes, 0x4000u);
	EXPECT_EQ(above->entries[0].offset, 0x4000u);
	switch_gpu_vm_unmap(&a, &taken);
	ASSERT_EQ(calls.size(), 2u);
	EXPECT_EQ(calls[0].operation, 'u');
	EXPECT_EQ(calls[0].va, 0x40002000u);
	EXPECT_EQ(calls[0].bytes, 0x2000u);
	EXPECT_EQ(calls[1].operation, 'p');
	EXPECT_EQ(calls[1].bytes, 0x2000u);
	switch_gpu_vm_unmap(&a, &below);
	switch_gpu_vm_unmap(&a, &above);
}

// A cut 128 KiB page keeps its PTE, whole, with the range's part; what stays outside the range comes back pending as
// 4 KiB entries, mapped (same pages, same kind) only by switch_gpu_vm_install once that PTE is gone.
TEST_F(VmMap, CutBigPageKeepsItsPteUntilRetirementAndItsRestWaitsForInstall)
{
	mem_prepare_big_views(&memory);
	switch_gpu_mapping *mapping = nullptr, *below = nullptr, *taken = nullptr, *above = nullptr;
	ASSERT_EQ(map(a, &mapping), 0);
	ASSERT_EQ(mapping->entries[0].shift, 17);
	calls.clear();
	ASSERT_EQ(switch_gpu_vm_split(&a, &mapping, &memory, 0, 0, 0, 0x40008000, 0x1000, &below,
				      &taken, &above),
		  0);
	EXPECT_TRUE(calls.empty());
	ASSERT_NE(taken, nullptr);
	ASSERT_EQ(taken->count, 1u);
	EXPECT_EQ(taken->entries[0].shift, 17);
	EXPECT_EQ(taken->entries[0].bytes, 0x20000u);
	ASSERT_EQ(below->count, 1u);
	EXPECT_TRUE(below->entries[0].pending);
	EXPECT_EQ(below->entries[0].shift, 12);
	ASSERT_EQ(above->count, 2u);
	EXPECT_TRUE(above->entries[0].pending);
	EXPECT_FALSE(above->entries[1].pending);
	switch_gpu_vm_unmap(&a, &taken);
	ASSERT_EQ(calls.size(), 2u);
	EXPECT_EQ(calls[0].operation, 'u');
	EXPECT_EQ(calls[0].shift, 17);
	calls.clear();
	ASSERT_EQ(switch_gpu_vm_install(&a, below, &memory, 0, 0, 0), 0);
	ASSERT_EQ(switch_gpu_vm_install(&a, above, &memory, 0, 0, 0), 0);
	const std::vector<std::pair<u64, u64>> remapped = {{0x40000000, 0x8000},
							   {0x40009000, 0x17000}};
	ASSERT_EQ(calls.size(), 4u);
	for (size_t i = 0; i < remapped.size(); ++i) {
		EXPECT_EQ(calls[2 * i].operation, 'g');
		EXPECT_EQ(calls[1 + 2 * i].operation, 'm');
		EXPECT_EQ(calls[1 + 2 * i].va, remapped[i].first);
		EXPECT_EQ(calls[1 + 2 * i].bytes, remapped[i].second);
		EXPECT_EQ(calls[1 + 2 * i].shift, 12);
		EXPECT_EQ(calls[1 + 2 * i].offset, remapped[i].first - 0x40000000);
		EXPECT_EQ(calls[1 + 2 * i].memory, &memory.mem);
		EXPECT_TRUE(calls[1 + 2 * i].deferred);
	}
	EXPECT_FALSE(switch_gpu_mapping_pending(below));
	char text[128];
	switch_gpu_mapping_describe(below, text, sizeof(text));
	EXPECT_STREQ(text, "0x40000000+0x8000/12");
	switch_gpu_vm_unmap(&a, &below);
	switch_gpu_vm_unmap(&a, &above);
}

TEST_F(VmMap, CompressedMappingCutInsideABigPageIsRefusedUntouched)
{
	memory.page = memory.mem.shift = 17;
	memory.bytes = memory.mem.bytes = 32 * PAGE_SIZE;
	memory.npages = 32;
	tgm_gpu.mmu.kind[0x42] = 0xfe;
	switch_gpu_mapping *mapping = nullptr, *below = nullptr, *taken = nullptr, *above = nullptr;
	ASSERT_EQ(switch_gpu_vm_map(&a, 0x40000000, memory.bytes, &memory, 0, 0x42, 0, &mapping),
		  0);
	calls.clear();
	EXPECT_EQ(switch_gpu_vm_split(&a, &mapping, &memory, 0x42, 0, 0, 0x40001000, 0x1000, &below,
				      &taken, &above),
		  -EINVAL);
	EXPECT_NE(mapping, nullptr);
	EXPECT_TRUE(calls.empty());
	switch_gpu_vm_unmap(&a, &mapping);
}

TEST_F(VmMap, PendingEntryThatCannotBeInstalledLeavesItsMappingOutsideTheOrderedScope)
{
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_prepare(&a, 0x40000000, 8 * PAGE_SIZE, &memory, 0, 0, &mapping), 0);
	EXPECT_TRUE(calls.empty());
	EXPECT_TRUE(switch_gpu_mapping_pending(mapping));
	EXPECT_FALSE(switch_gpu_mapping_installed(mapping));
	failMap = mapCount;
	EXPECT_EQ(switch_gpu_vm_install(&a, mapping, &memory, 0, 0, 0), -ENOMEM);
	EXPECT_EQ(mapping->count, 0u);
	EXPECT_EQ(calls.back().operation, 'p');
	EXPECT_FALSE(calls.back().deferred);
	switch_gpu_vm_unmap(&a, &mapping);
}
