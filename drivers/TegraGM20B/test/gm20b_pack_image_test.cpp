// SPDX-License-Identifier: MIT
#include "gm20b_pack_image_model.h"

TEST_F(PackImageTest, PackHeaderAndAbiLayoutMatchMetalPackH)
{
	static_assert(sizeof(SwitchGpuPackSection) == sizeof(gm20b::PackSection));
	static_assert(kSwitchGpuPackVersion == gm20b::kPackVersion);
	static_assert(uint32_t(kSwitchGpuPackSectionCount) == uint32_t(gm20b::PackSectionCount));
	static_assert(kSwitchGpuPackTierAlign == gm20b::kPackTierAlign);
	static_assert(uint32_t(kSwitchGpuPackPrograms) == uint32_t(gm20b::PackPrograms));
	static_assert(uint32_t(kSwitchGpuPackHeap) == uint32_t(gm20b::PackHeap));
	static_assert(uint32_t(kSwitchGpuPackVariants) == uint32_t(gm20b::PackVariants));
	static_assert(uint32_t(kSwitchGpuPackFastKeys) == uint32_t(gm20b::PackFastKeys));
	static_assert(uint32_t(kSwitchGpuPackSegments) == uint32_t(gm20b::PackSegments));
	static_assert(offsetof(SwitchGpuPackHeader, magic) == offsetof(gm20b::PackHeader, magic));
	static_assert(offsetof(SwitchGpuPackHeader, version) ==
		      offsetof(gm20b::PackHeader, version));
	static_assert(offsetof(SwitchGpuPackHeader, programs) ==
		      offsetof(gm20b::PackHeader, programs));
	static_assert(offsetof(SwitchGpuPackHeader, pipelines) ==
		      offsetof(gm20b::PackHeader, pipelines));
	static_assert(offsetof(SwitchGpuPackHeader, samplers) ==
		      offsetof(gm20b::PackHeader, samplers));
	static_assert(offsetof(SwitchGpuPackHeader, section) ==
		      offsetof(gm20b::PackHeader, section));
	static_assert(offsetof(SwitchGpuPackHeader, stack) == offsetof(gm20b::PackHeader, stack));
	static_assert(offsetof(SwitchGpuPackHeader, chipFacts) ==
		      offsetof(gm20b::PackHeader, chipFacts));
	static_assert(offsetof(SwitchGpuPackHeader, tier0SlmPerWarp) ==
		      offsetof(gm20b::PackHeader, tier0SlmPerWarp));
	static_assert(offsetof(SwitchGpuPackHeader, tier0) == offsetof(gm20b::PackHeader, tier0));
	static_assert(offsetof(SwitchGpuPackHeader, rules) == offsetof(gm20b::PackHeader, rules));

	EXPECT_EQ(TGM_PACK_WINDOW_VA, (1ULL << 39) - (4ULL << 30));
	EXPECT_EQ(TGM_PACK_WINDOW_BYTES, 4ULL << 30);
	EXPECT_STREQ(TGM_PACK_PATH, "/System/Library/Extensions/TegraGM20BMetal.pack/pack.bin");
	EXPECT_EQ(TegraGM20BMethodPackImageMap, 41u);
}

// The pack packfile.py writes, at the version it writes, is one the kernel maps: a version the writer moved alone would
// refuse every image at boot
TEST_F(PackImageTest, ThePackTheWriterMakesPassesTheKernelsHeaderCheck)
{
	const std::string pack = SourceText("../../TegraGM20BMetal/test/pack_v34_fixture.bin");
	ASSERT_GE(pack.size(), sizeof(SwitchGpuPackHeader));
	SwitchGpuPackHeader hdr = {};
	std::memcpy(&hdr, pack.data(), sizeof(hdr));
	EXPECT_EQ(hdr.version, kSwitchGpuPackVersion);
	const char *why = nullptr;
	EXPECT_TRUE(switch_gpu_pack_validate_header(&hdr, pack.size(), &why)) << (why ? why : "");
}

TEST_F(PackImageTest, HeaderValidationRefusesMalformedHeaders)
{
	auto bytes = makePackFile(256ULL << 10, 128ULL << 10);
	SwitchGpuPackHeader hdr = {};
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	const char *why = nullptr;
	EXPECT_TRUE(switch_gpu_pack_validate_header(&hdr, bytes.size(), &why));
	EXPECT_EQ(why, nullptr);

	auto bad = hdr;
	bad.magic[7] = '2';
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.version = 32;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.section[kSwitchGpuPackHeap].bytes = bytes.size();
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = bad.section[kSwitchGpuPackHeap].bytes + 4096;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = 64ULL << 10;  // not 128 KiB aligned
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = 0;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	EXPECT_FALSE(switch_gpu_pack_validate_header(&hdr, sizeof(hdr) - 1, &why));
}

TEST_F(PackImageTest, CpuRangeValidationRejectsUnalignedOverflowAndNonZeroTrailing)
{
	TegraGM20BAbiPackCpuRange ranges[TegraGM20BMaxPackCpuRanges] = {};
	uint64_t total = 0;
	ranges[0] = {0x10000000ULL, 0x4000ULL};
	ranges[1] = {0x10010000ULL, 0x8000ULL};
	EXPECT_TRUE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	EXPECT_EQ(total, 0xc000ULL);

	// Non-zero trailing entry past count
	ranges[2] = {0x10020000ULL, 0x4000ULL};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	ranges[2] = {};

	// Unaligned address
	ranges[0].address = 0x10001000ULL;
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	ranges[0].address = 0x10000000ULL;

	// Wraparound
	ranges[0] = {UINT64_MAX - PAGE_SIZE + 1ULL, 2ULL * PAGE_SIZE};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 1, PAGE_SIZE, &total));

	// Exceeds 64 MiB cap
	ranges[0] = {0x10000000ULL, kSwitchGpuPackMaxCpuWireBytes + PAGE_SIZE};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 1, PAGE_SIZE, &total));
}

TEST_F(PackImageTest, SingleBuildDeduplicationConcurrentCallersReceiveSameImageAndBytes)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 128ULL << 10, kSwitchGpuPackVersion,
					0xaabbccddeeff0011ULL);
	gModel.pauseFirstPagein = true;

	SwitchGpuPackImageView v1 = {}, v2 = {}, v3 = {};
	uint64_t w1 = 0, w2 = 0, w3 = 0;
	auto f1 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("cameracaptured", &v1, &w1);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered;
		});
	}
	auto f2 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("backboardd", &v2, &w2);
	});
	auto f3 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("SpringBoard", &v3, &w3);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.sleepers >= 3;
		});
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}

	ASSERT_EQ(f1.get(), kIOReturnSuccess);
	ASSERT_EQ(f2.get(), kIOReturnSuccess);
	ASSERT_EQ(f3.get(), kIOReturnSuccess);
	gModel.joinThreads();

	EXPECT_TRUE(v1.ready);
	EXPECT_EQ(v1.mem, v2.mem);
	EXPECT_EQ(v1.mem, v3.mem);
	EXPECT_EQ(v1.cpuMemory, v2.cpuMemory);
	EXPECT_EQ(v1.tier0HeapBytes, 256ULL << 10);
	EXPECT_EQ(v1.heapBytes, 384ULL << 10);
	EXPECT_EQ(v1.stack, 0xaabbccddeeff0011ULL);
	EXPECT_EQ(gModel.openCount, 1u);
	EXPECT_EQ(gModel.closeCount, 1u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.maxConcurrentUpls, 1);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_GT(gModel.committedUpls, 0u);
	EXPECT_EQ(gModel.abortedUpls, 0u);
	EXPECT_EQ(gModel.copyCalls, 1u);
	EXPECT_GT(gModel.srcCleanCalls, 0u);
	EXPECT_GT(gModel.dstCleanInvalOps, 0u);
	EXPECT_GT(gModel.dstInvalOps, 0u);

	// Verify copied tier-0 bytes match file byte-for-byte via CE copy (not CPU).
	for (uint32_t p = 0; p < v1.mem->npages; ++p) {
		const uint8_t *dst = gModel.physToHost.at(v1.mem->pages[p]);
		const uint8_t *exp =
			gModel.fileBytes.data() + kSwitchGpuPackTierAlign + size_t(p) * 4096;
		ASSERT_EQ(std::memcmp(dst, exp, 4096), 0) << "mismatch at 4 KiB page " << p;
	}
}

TEST_F(PackImageTest, BadVersionRefusesOnceNeverRetriesAndClosesVnode)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0, 32);
	SwitchGpuPackImageView v1 = {}, v2 = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc1", &v1, nullptr), kIOReturnSuccess);
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc2", &v2, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(v1.ready);
	EXPECT_FALSE(v2.ready);
	EXPECT_EQ(gModel.openCount, 1u);
	EXPECT_EQ(gModel.closeCount, 1u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(switch_gpu_pack_image_refused_count_for_test(), 1u);
	EXPECT_EQ(gModel.logs.size(), 1u);
	EXPECT_THAT(gModel.logs[0], HasSubstr("refused (unsupported version"));
}

TEST_F(PackImageTest, MixedBigPageAndFallbackMapsAtWindowVaInTwoVms)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.failContigChunkIndex = 1;  // chunk 0: 128 KiB contiguous; chunk 1: 4 KiB fallback

	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	gModel.joinThreads();

	struct switch_gpu_mapping_run run0 = {}, run1 = {};
	ASSERT_TRUE(switch_gpu_mapping_next(view.mem->pages, view.mem->npages, TGM_PACK_WINDOW_VA,
					    0, view.tier0HeapBytes, &run0));
	EXPECT_EQ(run0.shift, SWITCH_GPU_BIG_SHIFT);
	EXPECT_EQ(run0.bytes, kSwitchGpuPackTierAlign);
	ASSERT_TRUE(switch_gpu_mapping_next(view.mem->pages, view.mem->npages,
					    TGM_PACK_WINDOW_VA + run0.bytes, run0.bytes,
					    view.tier0HeapBytes - run0.bytes, &run1));
	EXPECT_EQ(run1.shift, SWITCH_GPU_SMALL_SHIFT);
	EXPECT_EQ(run1.bytes, kSwitchGpuPackTierAlign);

	tgm_vm vmA = {1, {}}, vmB = {2, {}};
	SwitchGpuPackClient clientA = {}, clientB = {};
	ASSERT_EQ(switch_gpu_vm_map_volatile(&vmA, TGM_PACK_WINDOW_VA, view.tier0HeapBytes,
					     view.mem, 0, 0, 1, 0, &clientA.gpuMapping),
		  0);
	ASSERT_EQ(switch_gpu_vm_map_volatile(&vmB, TGM_PACK_WINDOW_VA, view.tier0HeapBytes,
					     view.mem, 0, 0, 1, 0, &clientB.gpuMapping),
		  0);
	EXPECT_EQ(clientA.gpuMapping->va, TGM_PACK_WINDOW_VA);
	EXPECT_EQ(clientA.gpuMapping->readOnly, 1);
	EXPECT_EQ(clientA.gpuMapping->vol, 0);
	EXPECT_EQ(clientA.gpuMapping->bigBytes, kSwitchGpuPackTierAlign);
	EXPECT_EQ(clientB.gpuMapping->va, TGM_PACK_WINDOW_VA);
	EXPECT_EQ(clientB.gpuMapping->readOnly, 1);

	switch_gpu_pack_client_unmap_gpu(&vmA, &clientA);
	EXPECT_EQ(clientA.gpuMapping, nullptr);
	EXPECT_FALSE(view.mem->freed);
	EXPECT_EQ(vmB.mappings.count(TGM_PACK_WINDOW_VA), 1u);
	switch_gpu_pack_client_unmap_gpu(&vmB, &clientB);
}

TEST_F(PackImageTest, CompositorCpuRangeWiringDropsOnCloseWhileSharedImageSurvives)
{
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	gModel.joinThreads();

	switch_pack_task compositorTask = {42};
	SwitchGpuPackClient client = {};

	TegraGM20BAbiPackCpuRange ranges[2] = {
		{0x40000000ULL, 0x10000ULL},
		{0x40100000ULL, 0x20000ULL},
	};
	uint64_t wiredBytes = 0;
	ASSERT_EQ(switch_gpu_pack_client_wire_cpu(&compositorTask, ranges, 2, client.cpuWired,
						  &wiredBytes),
		  kIOReturnSuccess);
	client.cpuWiredCount = 2;
	EXPECT_EQ(wiredBytes, 0x30000ULL);
	EXPECT_EQ(client.cpuWired[0]->preparedDir, kIODirectionOut);
	EXPECT_EQ(client.cpuWired[1]->preparedDir, kIODirectionOut);

	const int sharedRefsBefore = view.cpuMemory->refs.load();
	switch_gpu_pack_client_release_cpu(&client);
	EXPECT_EQ(client.cpuWiredCount, 0u);
	EXPECT_EQ(gModel.liveMaps, 0);
	EXPECT_EQ(view.cpuMemory->refs.load(), sharedRefsBefore);

	// A restarted compositor still gets the same shared image, and partial wire failure rolls back cleanly.
	gModel.failWireRangeIndex = 3;  // 2nd range of next call fails prepare()
	SwitchGpuPackClient restarted = {};
	EXPECT_EQ(switch_gpu_pack_client_wire_cpu(&compositorTask, ranges, 2, restarted.cpuWired,
						  &wiredBytes),
		  kIOReturnBadArgument);
	EXPECT_EQ(restarted.cpuWired[0], nullptr);
	EXPECT_EQ(restarted.cpuWired[1], nullptr);
}

TEST_F(PackImageTest, WiredOrAbsentUplFallsBackToExactReadWithoutRefusingPack)
{
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	gModel.injectAlwaysAbsentUpl = true;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	gModel.joinThreads();
	EXPECT_EQ(gModel.copyCalls, 0u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	auto *multi = static_cast<IOMultiMemoryDescriptor *>(view.cpuMemory);
	ASSERT_EQ(multi->parts.size(), 1u);
	auto *bmd = static_cast<IOBufferMemoryDescriptor *>(multi->parts[0]);
	const uint8_t *expected = gModel.fileBytes.data() + kSwitchGpuPackTierAlign;
	EXPECT_EQ(std::memcmp(bmd->getBytesNoCopy(), expected, kSwitchGpuPackTierAlign), 0);
}

TEST_F(PackImageTest, PartialAbsentUplRetriesAndCopyFailureCleansUpAllState)
{
	// Case 1: transient partial-absent UPL commits cleanly and succeeds on retry.
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	gModel.injectPartialAbsentOnce = true;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &view, nullptr), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	gModel.joinThreads();
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(gModel.abortedUpls, 0u);
	EXPECT_GE(gModel.committedUpls, 2u);
	EXPECT_EQ(gModel.heapPageins, 2u);

	// Case 2: a copy failure after the image opened commits the UPL, closes the vnode and stops the stream: the jobs
	// that wait on it fail and the next open is refused. The image stays, its clients mapped it.
	switch_gpu_pack_image_reset_for_test();
	gModel.reset();
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.failCopyOnCall = 0;
	SwitchGpuPackImageView failed = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &failed, nullptr), kIOReturnSuccess);
	EXPECT_TRUE(failed.ready);
	gModel.joinThreads();
	uint32_t pool = 0, payload = 0;
	EXPECT_EQ(switch_gpu_pack_job_wait(&pool, &payload), -1);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(switch_gpu_pack_image_refused_count_for_test(), 1u);
	EXPECT_EQ(gModel.recovered.size(), 1u);
	SwitchGpuPackImageView later = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("later", &later, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(later.ready);
}

TEST_F(PackImageTest, ShutdownDrainsActiveBuildClosesVnodeAndWakesWaiter)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.pauseFirstPagein = true;

	SwitchGpuPackImageView view = {};
	auto caller = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("backboardd", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered && gModel.sleepers >= 1;
		});
	}
	auto shuttingDown = std::async(std::launch::async, [] {
		switch_gpu_pack_shutdown();
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.unintSleepers >= 1;
		});
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}
	shuttingDown.get();
	ASSERT_EQ(caller.get(), kIOReturnSuccess);
	EXPECT_FALSE(view.ready);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(gModel.liveDescriptors, 0);
}

TEST_F(PackImageTest, AStopThatNeverEndsPanicsNamingTheThreadInsteadOfHangingTheReboot)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.pauseFirstPagein = true;

	SwitchGpuPackImageView view = {};
	auto caller = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("backboardd", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered && gModel.sleepers >= 1;
		});
		gModel.shutdownDeadlinePassed = true;
	}
	auto stopping = std::async(std::launch::async, []() -> std::string {
		try {
			switch_gpu_pack_shutdown();
		} catch (const PackPanic &p) {
			return p.message;
		}
		return {};
	});
	const bool ended = stopping.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
	EXPECT_TRUE(ended) << "the stop waited past its deadline";
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.shutdownDeadlinePassed = false;
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}
	EXPECT_THAT(stopping.get(),
		    HasSubstr("the build thread is still running 20 s into shutdown"));
	ASSERT_EQ(caller.get(), kIOReturnSuccess);
	EXPECT_FALSE(view.ready);
}

// The pack reads nothing after boot: a tier-1 state's pages are read where it is first used. A read of the whole tier
// after boot reached the card at tier 0 whatever the thread's I/O policy, in the UI's class for 150 s.
TEST_F(PackImageTest, XnuShutdownHookWiredAndNoBootCompleteRead)
{
	const std::string pm10063 =
		SourceText("../../../kernel/xnu-10063.141.1/iokit/Kernel/IOPMrootDomain.cpp");
	const std::string pm12377 =
		SourceText("../../../kernel/xnu-12377.121.6/iokit/Kernel/IOPMrootDomain.cpp");
	for (const std::string &pm : {pm10063, pm12377})
		EXPECT_EQ(Count(pm, "switch_gpu_boot_complete"), 0u);
	const std::string shutdown = SourceText("../../../kernel/switch/shutdown/shutdown.c");
	EXPECT_THAT(shutdown,
		    HasSubstr("extern void switch_gpu_pack_shutdown(void) __attribute__((weak));"));
	EXPECT_THAT(shutdown, HasSubstr("extern void switch_gpu_pack_shutdown_request(void) "
					"__attribute__((weak));"));
	EXPECT_GE(Count(shutdown, "switch_gpu_pack_shutdown_request();"), 2u);
	EXPECT_GE(Count(shutdown, "switch_gpu_pack_shutdown()"), 2u);
}

TEST_F(PackImageTest, PackImageMapDoesNotMapKernelMemoryIntoClientTasks)
{
	const std::string uc = SourceText("../TegraGM20BUserClient.cpp");
	const std::string body = Between(uc, "IOReturn AGXDeviceUserClient::packImageMap(",
					 "\nvoid AGXDeviceUserClient::releaseMappings()");
	EXPECT_THAT(body, Not(HasSubstr("createMappingInTask")));
	EXPECT_THAT(body, HasSubstr("out->cpu_address = 0;"));
}

TEST_F(PackImageTest, CpuWireBudgetIsEnforcedSystemWideAcrossClients)
{
	TegraGM20BAbiPackCpuRange range = {0x10000000ULL, 40ULL << 20};
	SwitchGpuPackClient a = {}, b = {};
	uint64_t wiredA = 0, wiredB = 0;
	ASSERT_EQ(switch_gpu_pack_client_wire_cpu(task_t(0x11), &range, 1, a.cpuWired, &wiredA),
		  kIOReturnSuccess);
	a.cpuWiredCount = 1;
	a.cpuWiredBytes = wiredA;
	EXPECT_EQ(wiredA, 40ULL << 20);

	EXPECT_EQ(switch_gpu_pack_client_wire_cpu(task_t(0x22), &range, 1, b.cpuWired, &wiredB),
		  kIOReturnNoResources);

	switch_gpu_pack_client_release_cpu(&a);
	ASSERT_EQ(switch_gpu_pack_client_wire_cpu(task_t(0x22), &range, 1, b.cpuWired, &wiredB),
		  kIOReturnSuccess);
	b.cpuWiredCount = 1;
	b.cpuWiredBytes = wiredB;
	switch_gpu_pack_client_release_cpu(&b);
}
