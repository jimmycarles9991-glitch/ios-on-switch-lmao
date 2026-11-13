// SPDX-License-Identifier: MIT
// SwitchGpuJobVisibility.h over gm20b_gpu_memory.h: a job's stores sit dirty in the L2, and its fence writes them to DRAM
// only when the job ends External (gpu_exec.c tgm_chan_fence_job: WFI, FB_FLUSH, L2_FLUSH_DIRTY). A job that writes an IOSurface
// over its creator's memory ends External whatever the runtime asked, so the creator's lockless read after completion
// sees the GPU's bytes; a job that writes only what a lock or a visibility wait guards stays Internal.
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuJobVisibility.h"
#include "gm20b_gpu_memory.h"

namespace
{
constexpr uint64_t kSurfaceVa = 1ull << 32, kDrawn = 4000, kDrawnBytes = 256;
constexpr uint32_t kInternal = TegraGM20BSubmitVisibilityExplicit;

// gpu_exec.c chan_exec's reading of the flags: without explicit visibility every job is External.
bool flushes(uint32_t visibility)
{
	return !(visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) ||
	       (visibility & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
}

struct Creator {
	std::vector<uint8_t> pages = std::vector<uint8_t>(2 * Memory::kPage, 0x11);
	Memory memory;

	Creator()
	{
		memory.map(kSurfaceVa, pages, false, true);
	}

	void draw(uint32_t submit, bool signals, bool unlockedWrites)
	{
		const std::vector<uint8_t> drawn(kDrawnBytes, 0xa7);
		ASSERT_TRUE(memory.store(kSurfaceVa + kDrawn, drawn.data(), drawn.size()));
		if (flushes(switch_gpu_job_visibility(submit, signals, unlockedWrites, false)))
			memory.flushL2();
	}

	uint64_t staleBytes() const
	{
		uint64_t stale = 0;
		for (uint64_t i = kDrawn; i < kDrawn + kDrawnBytes; ++i)
			stale += pages[i] != 0xa7;
		return stale;
	}
};
}  // namespace

TEST(JobVisibility, AWriteIntoCreatorMemoryIsInDramAtCompletionThoughTheRuntimeAskedInternal)
{
	Creator creator;
	creator.draw(kInternal, false, true);
	EXPECT_EQ(creator.staleBytes(), 0u);
}

TEST(JobVisibility, AnInternalJobsWritesStayInTheL2PastItsCompletion)
{
	Creator creator;
	creator.draw(kInternal, false, false);
	EXPECT_EQ(creator.staleBytes(), kDrawnBytes);
	creator.memory.flushL2();
	EXPECT_EQ(creator.staleBytes(), 0u);
}

TEST(JobVisibility, OnlyCreatorMemoryEventSignalsOrTheRuntimeMakeAJobExternal)
{
	EXPECT_FALSE(flushes(switch_gpu_job_visibility(kInternal, false, false, true)));
	EXPECT_TRUE(flushes(switch_gpu_job_visibility(kInternal, false, true, false)));
	EXPECT_TRUE(flushes(switch_gpu_job_visibility(kInternal, true, false, false)));
	EXPECT_TRUE(flushes(switch_gpu_job_visibility(
		kInternal | TegraGM20BSubmitExternalVisibility, false, false, false)));
}

TEST(JobVisibility, ASubmitWithoutExplicitVisibilityIsExternal)
{
	Creator creator;
	creator.draw(0, false, false);
	EXPECT_EQ(creator.staleBytes(), 0u);
}

TEST(JobVisibility, EventsAndHostWritesCarryOverToTheChannel)
{
	const uint32_t flags = switch_gpu_job_visibility(kInternal | TegraGM20BSubmitHasEvents,
							 false, false, true);
	EXPECT_TRUE(flags & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT);
	EXPECT_TRUE(flags & SWITCH_GPU_SUBMIT_FORCE_FENCE);
	EXPECT_TRUE(flags & SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	EXPECT_FALSE(flags & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
}
