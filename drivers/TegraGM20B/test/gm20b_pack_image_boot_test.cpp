// SPDX-License-Identifier: MIT
// The tier-0 image is built at boot, once pack.bin is reachable (kern_exec.c, after the owner overlay) and the GPU is
// published (the bring-up), whichever happens second, so the render server's first acquire does not start it. Boot
// 2693043C: cameracaptured started it and backboardd's window server waited 7,138 ms of its 7,442.
#include "gm20b_pack_image_model.h"

namespace
{

using Condition = void (*)();

void gpuUsable()
{
	switch_gpu_pack_gpu_usable();
}

uint32_t opens()
{
	std::lock_guard<std::mutex> g(gModel.mu);
	return gModel.openCount;
}

void expectOneBootBuild(Condition first, Condition second)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	first();
	first();
	gModel.joinThreads();
	EXPECT_EQ(opens(), 0u) << "one condition alone starts nothing";
	second();
	gModel.joinThreads();
	second();
	first();
	gModel.joinThreads();
	EXPECT_EQ(opens(), 1u);
	SwitchGpuPackImageView view = {};
	uint64_t waited = 1;
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, &waited), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	EXPECT_EQ(waited, 0u);
	EXPECT_EQ(opens(), 1u);
	EXPECT_EQ(gModel.copyCalls, 1u);
}

TEST_F(PackImageTest, VolumesThenGpuStartsOneBuildBeforeAnyClient)
{
	expectOneBootBuild(switch_gpu_pack_volumes_mounted, gpuUsable);
}

TEST_F(PackImageTest, GpuThenVolumesStartsOneBuildBeforeAnyClient)
{
	expectOneBootBuild(gpuUsable, switch_gpu_pack_volumes_mounted);
}

TEST_F(PackImageTest, AClientsAcquireDuringTheBootBuildWaitsForIt)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.pauseFirstPagein = true;
	switch_gpu_pack_volumes_mounted();
	switch_gpu_pack_gpu_usable();
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered;
		});
	}
	SwitchGpuPackImageView view = {};
	auto client = std::async(std::launch::async, [&view] {
		return switch_gpu_pack_image_acquire("backboardd", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.sleepers >= 1;
		});
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}
	ASSERT_EQ(client.get(), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	gModel.joinThreads();
	EXPECT_EQ(opens(), 1u);
}

TEST_F(PackImageTest, AClientBuildBeforeBootLeavesTheConditionsNoSecondBuild)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("cameracaptured", &view, nullptr),
		  kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	switch_gpu_pack_volumes_mounted();
	switch_gpu_pack_gpu_usable();
	gModel.joinThreads();
	EXPECT_EQ(opens(), 1u);
}

TEST_F(PackImageTest, AShutdownBeforeTheSecondConditionStartsNoBuild)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	switch_gpu_pack_volumes_mounted();
	switch_gpu_pack_shutdown();
	switch_gpu_pack_gpu_usable();
	gModel.joinThreads();
	EXPECT_EQ(opens(), 0u);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(view.ready);
}

TEST_F(PackImageTest, BothKernelsMarkTheVolumesAfterTheOverlayAndTheBringUpMarksTheGpu)
{
	for (const char *path : {"../../../kernel/xnu-10063.141.1/bsd/kern/kern_exec.c",
				 "../../../kernel/xnu-12377.121.6/bsd/kern/kern_exec.c"}) {
		const std::string exec = SourceText(path);
		const size_t overlay =
			exec.find("native_switch_raw_overlay(p, (user_addr_t)scratch_addr);");
		const size_t marked = exec.find("switch_gpu_pack_volumes_mounted();");
		ASSERT_NE(overlay, std::string::npos) << path;
		ASSERT_NE(marked, std::string::npos) << path;
		EXPECT_LT(overlay, marked) << path;
		EXPECT_LT(marked, exec.find("switch_boot_steps_log(&steps, \"before launchd\");"))
			<< path;
		EXPECT_THAT(exec, HasSubstr("extern void switch_gpu_pack_volumes_mounted(void) "
					    "__attribute__((weak));"))
			<< path;
	}
	const std::string gpu = SourceText("../TegraGM20B.cpp");
	const size_t published = gpu.find("M4 ok published AGXAccelerator");
	ASSERT_NE(published, std::string::npos);
	EXPECT_LT(published, gpu.find("switch_gpu_pack_gpu_usable();"));
	EXPECT_EQ(Count(gpu, "switch_gpu_pack_gpu_usable();"), 1u);
}

}  // namespace
