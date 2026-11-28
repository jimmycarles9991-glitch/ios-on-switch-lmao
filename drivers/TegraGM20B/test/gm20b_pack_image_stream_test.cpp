// SPDX-License-Identifier: MIT
// The tier-0 image opens once its tables are read, before its heap has streamed in: a device's open does not wait for the
// card. Each job of a connection that mapped it acquires on the GPU, until the stream's last chunk lands, on the word the
// stream raises. A stream that stops fails that work and refuses what follows: the GPU never runs a page it did not
// write.
#include "gm20b_pack_image_model.h"
#include "drivers/TegraGM20B/SwitchGpuPackTransfer.h"

namespace
{

constexpr uint64_t kChunks = 3;

uint32_t frontier()
{
	std::lock_guard<std::mutex> g(gModel.mu);
	return gModel.fenceWords[TGM_FENCE_KERNEL_PACK * 4];
}

TEST_F(PackImageTest, TheImageOpensBeforeItsStreamEndsAndJobsWaitOnTheGpuForIt)
{
	gModel.fileBytes = makePackFile(kChunks * kSwitchGpuPackTransferBytes, 0);
	gModel.pauseHeapPagein = 2;
	SwitchGpuPackImageView view = {};
	uint64_t waited = 0;
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, &waited), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.callPaused;
		});
	}
	EXPECT_EQ(frontier(), 1u) << "the first chunk landed, the second is being read";
	uint32_t pool = 0, payload = 0;
	EXPECT_EQ(switch_gpu_pack_job_wait(&pool, &payload), 1);
	EXPECT_EQ(payload, kChunks);
	EXPECT_EQ(pool, tgm_fence_offset(&gModel.fenceWords[TGM_FENCE_KERNEL_PACK * 4]));
	EXPECT_FALSE(switch_gpu_pack_failed_word(pool));
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.releaseCall = true;
		gModel.cv.notify_all();
	}
	gModel.joinThreads();
	EXPECT_EQ(frontier(), kChunks);
	EXPECT_EQ(switch_gpu_pack_job_wait(&pool, &payload), 0)
		<< "a job after the stream's end waits on nothing";
	EXPECT_TRUE(gModel.recovered.empty());
	for (uint32_t p = 0; p < view.mem->npages; ++p) {
		const uint8_t *dst = gModel.physToHost.at(view.mem->pages[p]);
		const uint8_t *exp =
			gModel.fileBytes.data() + kSwitchGpuPackTierAlign + size_t(p) * 4096;
		ASSERT_EQ(std::memcmp(dst, exp, 4096), 0) << "page " << p;
	}
}

TEST_F(PackImageTest, AStreamThatStopsAfterTheOpenFailsItsWaitersAndRefusesTheRest)
{
	gModel.fileBytes = makePackFile(kChunks * kSwitchGpuPackTransferBytes, 0);
	gModel.failCopyOnCall = 1;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	gModel.joinThreads();
	EXPECT_EQ(frontier(), 1u) << "the word stays where the stream stopped";
	uint32_t pool = 0, payload = 0;
	EXPECT_EQ(switch_gpu_pack_job_wait(&pool, &payload), -1);
	EXPECT_TRUE(switch_gpu_pack_failed_word(
		tgm_fence_offset(&gModel.fenceWords[TGM_FENCE_KERNEL_PACK * 4])));
	ASSERT_EQ(gModel.recovered.size(), 1u);
	EXPECT_THAT(gModel.recovered[0], HasSubstr("pack image"));
	EXPECT_EQ(switch_gpu_pack_image_refused_count_for_test(), 1u);
	SwitchGpuPackImageView later = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("SpringBoard", &later, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(later.ready);
}

TEST_F(PackImageTest, TheRenderServersTablesAreReadBeforeTheImageOpens)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.pauseHeapPagein = 1;
	SwitchGpuPackImageView view = {};
	auto open = std::async(std::launch::async, [&view] {
		return switch_gpu_pack_image_acquire("backboardd", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.callPaused;
		});
		EXPECT_GE(gModel.pageinCalls - gModel.heapPageins, 3u)
			<< "the fast keys, programs and variants";
	}
	ASSERT_EQ(open.get(), kIOReturnSuccess) << "the open does not wait for the heap";
	EXPECT_TRUE(view.ready);
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.releaseCall = true;
		gModel.cv.notify_all();
	}
}

// Before launchd, launchd's and its daemons' demand page-ins go first: the build reads in the boot replay's class
TEST_F(PackImageTest, TheBuildReadsInTheBootReplaysClass)
{
	gModel.fileBytes = makePackFile(kChunks * kSwitchGpuPackTransferBytes, 0);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("boot", &view, nullptr), kIOReturnSuccess);
	gModel.joinThreads();
	EXPECT_GT(gModel.pageinCalls, 0u);
	EXPECT_EQ(gModel.pageinsOutsideReplay, 0u) << "page-ins outside the replay's class";
	EXPECT_EQ(gModel.replayMarks, 2u);
	EXPECT_FALSE(gModel.replaying) << "the build left its thread in the replay's class";
}

// Shutdown's wait lasts one read: the table pre-read stops at the read after the flag, as the heap stream does
TEST_F(PackImageTest, AShutdownDuringTheTableReadsStopsAtTheNextRead)
{
	gModel.fileBytes = makePackFile(kChunks * kSwitchGpuPackTransferBytes, 0);
	gModel.pauseFirstPagein = true;
	SwitchGpuPackImageView view = {};
	auto open = std::async(std::launch::async, [&view] {
		return switch_gpu_pack_image_acquire("boot", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered;
		});
	}
	auto shutdown = std::async(std::launch::async, [] {
		switch_gpu_pack_shutdown();
	});
	while (!switch_gpu_pack_shutdown_requested())
		std::this_thread::yield();
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}
	shutdown.get();
	ASSERT_EQ(open.get(), kIOReturnSuccess);
	EXPECT_FALSE(view.ready);
	gModel.joinThreads();
	EXPECT_EQ(gModel.pageinCalls, 1u) << "table reads after the shutdown flag";
	EXPECT_EQ(gModel.heapPageins, 0u);
}

// A stream that shutdown stops recovers its parked channels inside shutdown's wait, never after it returned
TEST_F(PackImageTest, AShutdownMidStreamRecoversItsWaitersBeforeShutdownReturns)
{
	gModel.fileBytes = makePackFile(kChunks * kSwitchGpuPackTransferBytes, 0);
	gModel.pauseHeapPagein = 2;
	gModel.holdRecovery = true;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.callPaused;
		});
	}
	auto shutdown = std::async(std::launch::async, [] {
		switch_gpu_pack_shutdown();
	});
	while (!switch_gpu_pack_shutdown_requested())
		std::this_thread::yield();
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.releaseCall = true;
		gModel.cv.notify_all();
	}
	shutdown.get();
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		gModel.shutdownReturned = true;
		gModel.cv.notify_all();
	}
	gModel.joinThreads();
	ASSERT_EQ(gModel.recovered.size(), 1u);
	EXPECT_FALSE(gModel.recoveredAfterShutdown)
		<< "channels recovered after shutdown's wait returned";
	EXPECT_LT(frontier(), kChunks) << "the stream went on after the shutdown flag";
}

TEST_F(PackImageTest, JobsOfAMappedConnectionWaitAndTheHangRuleFailsAStoppedWord)
{
	const std::string submit = SourceText("../TegraGM20BUserClientJobSubmit.cpp");
	EXPECT_THAT(submit, HasSubstr("switch_gpu_pack_job_wait(&packPool, &packChunks)"));
	EXPECT_THAT(submit, HasSubstr("SwitchGpuRefusal::PackImage"));
	EXPECT_THAT(submit, HasSubstr("words[wordCount++] = {packPool, packChunks, 0};"));
	const std::string acquire = SourceText("../gpu_acquire.c");
	const std::string source =
		Between(acquire, "static enum tgm_wait_source semaphore_source(", "\n}\n");
	EXPECT_THAT(source, HasSubstr("switch_gpu_pack_failed_word(tgm_fence_offset(word))"));
	EXPECT_THAT(source, HasSubstr("return TGM_WAITS_FAILED;"));
	const std::string exec = SourceText("../gpu_exec.c");
	EXPECT_THAT(exec, HasSubstr("count > TegraGM20BMaxJobEvents + 1"));
}

}  // namespace
