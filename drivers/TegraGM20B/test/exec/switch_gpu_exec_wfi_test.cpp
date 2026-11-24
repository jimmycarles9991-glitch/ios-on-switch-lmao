// SPDX-License-Identifier: MIT
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_fixture.h"

namespace
{
const tgm_push kPush = {0x40000000, 16, 0, 0, 0, 0};

struct ExecWfi : public testing::Test {
	switch_exec_chan *chan = nullptr;

	void SetUp() override
	{
		const switch_exec_hooks hooks = {};
		switch_exec_reset(&hooks);
		chan = switch_exec_chan_new(0x100, 0);
		switch_exec_chan_enable_visible(chan);
	}

	switch_exec_wfi_counts submit(uint32_t visibility, const switch_gpu_release *releases,
				      uint32_t release_count, uint32_t *seqno_out = nullptr)
	{
		switch_exec_chan_pointers before = {};
		switch_exec_chan_pointers after = {};
		switch_exec_wfi_counts counts = {};
		uint32_t seqno = 0;

		switch_exec_chan_read(chan, &before);
		EXPECT_EQ(switch_gpu_chan_exec_events(switch_exec_chan_tgm(chan), &kPush, 1,
						      nullptr, 0, nullptr, 0, nullptr, 0, &seqno,
						      nullptr, visibility, releases, release_count,
						      nullptr),
			  0);
		switch_exec_chan_read(chan, &after);
		switch_exec_chan_count_wfis(chan, before.push_cur, after.push_cur, &counts);
		if (seqno_out)
			*seqno_out = seqno;
		return counts;
	}
};

TEST_F(ExecWfi, ExternalFenceEmitsExactlyOneHostWfiAcrossAllReleases)
{
	const switch_gpu_release releases[2] = {
		{0x3000000800ull, 7},
		{0x3000000804ull, 8},
	};
	const auto counts = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					   SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY,
				   releases, 2);
	EXPECT_EQ(counts.method_wfis, 1u);
	EXPECT_EQ(counts.fb_flushes, 2u);
	EXPECT_EQ(counts.l2_flushes, 1u);
	EXPECT_EQ(counts.visible_wfi_en, 0u);
	EXPECT_EQ(counts.visible_wfi_dis, 1u);
	EXPECT_EQ(counts.fence_wfi_en, 0u);
	EXPECT_EQ(counts.fence_wfi_dis, 1u);
	EXPECT_EQ(counts.extra_wfi_en, 0u);
	EXPECT_EQ(counts.extra_wfi_dis, 2u);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(counts.start_stamps_en, 0u);
	EXPECT_EQ(counts.start_stamps_dis, 0u);
	EXPECT_EQ(counts.end_stamps_en, 0u);
	EXPECT_EQ(counts.end_stamps_dis, 0u);
	EXPECT_EQ(counts.release_wfi_en, 0u);
#else
	EXPECT_EQ(counts.start_stamps_en, 1u);
	EXPECT_EQ(counts.end_stamps_en, 0u);
	EXPECT_EQ(counts.end_stamps_dis, 1u);
	EXPECT_EQ(counts.release_wfi_en, 1u);
#endif
}

TEST_F(ExecWfi, InternalFenceDrainsOnceThenDisablesWfiOnTrailingReleases)
{
	const switch_gpu_release releases[2] = {
		{0x3000000800ull, 7},
		{0x3000000804ull, 8},
	};
	const auto counts = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT, releases, 2);
	EXPECT_EQ(counts.method_wfis, 0u);
	EXPECT_EQ(counts.fb_flushes, 0u);
	EXPECT_EQ(counts.l2_flushes, 0u);
	EXPECT_EQ(counts.visible_wfi_en, 0u);
	EXPECT_EQ(counts.visible_wfi_dis, 0u);
	EXPECT_EQ(counts.extra_wfi_en, 0u);
	EXPECT_EQ(counts.extra_wfi_dis, 2u);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(counts.start_stamps_en, 0u);
	EXPECT_EQ(counts.start_stamps_dis, 0u);
	EXPECT_EQ(counts.end_stamps_en, 0u);
	EXPECT_EQ(counts.end_stamps_dis, 0u);
	EXPECT_EQ(counts.fence_wfi_en, 1u);
	EXPECT_EQ(counts.fence_wfi_dis, 0u);
	EXPECT_EQ(counts.release_wfi_en, 1u);
#else
	EXPECT_EQ(counts.start_stamps_en, 1u);
	EXPECT_EQ(counts.end_stamps_en, 1u);
	EXPECT_EQ(counts.end_stamps_dis, 0u);
	EXPECT_EQ(counts.fence_wfi_en, 0u);
	EXPECT_EQ(counts.fence_wfi_dis, 1u);
	EXPECT_EQ(counts.release_wfi_en, 2u);
#endif
}

TEST_F(ExecWfi, VolatileFenceEmitsWfiAndFbFlushWithoutL2FlushOrVisibleAdvance)
{
	const switch_gpu_release releases[2] = {
		{0x3000000800ull, 7},
		{0x3000000804ull, 8},
	};
	const auto counts = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					   SWITCH_GPU_SUBMIT_VOLATILE_VISIBILITY,
				   releases, 2);
	EXPECT_EQ(counts.method_wfis, 1u);
	EXPECT_EQ(counts.fb_flushes, 1u);
	EXPECT_EQ(counts.l2_flushes, 0u);
	EXPECT_EQ(counts.visible_wfi_en, 0u);
	EXPECT_EQ(counts.visible_wfi_dis, 0u);
	EXPECT_EQ(counts.fence_wfi_en, 0u);
	EXPECT_EQ(counts.fence_wfi_dis, 1u);
	EXPECT_EQ(counts.extra_wfi_en, 0u);
	EXPECT_EQ(counts.extra_wfi_dis, 2u);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(counts.start_stamps_en, 0u);
	EXPECT_EQ(counts.start_stamps_dis, 0u);
	EXPECT_EQ(counts.end_stamps_en, 0u);
	EXPECT_EQ(counts.end_stamps_dis, 0u);
	EXPECT_EQ(counts.release_wfi_en, 0u);
#else
	EXPECT_EQ(counts.start_stamps_en, 1u);
	EXPECT_EQ(counts.end_stamps_en, 0u);
	EXPECT_EQ(counts.end_stamps_dis, 1u);
	EXPECT_EQ(counts.release_wfi_en, 1u);
#endif
}

TEST_F(ExecWfi, HighChannelStartStampUsesWfiOnlyWhenLowBusyAndSkipsWhenIdle)
{
	switch_exec_chan_high(chan, 1);
	uint32_t seqno = 0;

	const auto idle = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					 SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY,
				 nullptr, 0, &seqno);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(idle.start_stamps_en, 0u);
	EXPECT_EQ(idle.start_stamps_dis, 0u);
	EXPECT_EQ(idle.method_wfis, 1u);
	EXPECT_EQ(idle.release_wfi_en, 0u);
	EXPECT_EQ(switch_exec_chan_job_waits(chan, seqno), 1);
#else
	EXPECT_EQ(idle.start_stamps_en, 1u);
	EXPECT_EQ(switch_exec_chan_job_waits(chan, seqno), 0);
#endif

	switch_exec_set_low_busy(7, 1);
	const auto busy = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					 SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY,
				 nullptr, 0, &seqno);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(busy.start_stamps_en, 1u);
	EXPECT_EQ(busy.start_stamps_dis, 0u);
	EXPECT_EQ(busy.method_wfis, 1u);
	EXPECT_EQ(busy.release_wfi_en, 1u);
	EXPECT_EQ(switch_exec_chan_job_waits(chan, seqno), 0);
#else
	EXPECT_EQ(busy.start_stamps_en, 1u);
	EXPECT_EQ(switch_exec_chan_job_waits(chan, seqno), 0);
#endif

	switch_exec_chan_kernel(chan, 1);
	const auto kernel = submit(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					   SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY,
				   nullptr, 0, &seqno);
#if SWITCH_EXEC_WFI_RELEASE
	EXPECT_EQ(kernel.start_stamps_en, 0u);
	EXPECT_EQ(kernel.start_stamps_dis, 0u);
	EXPECT_EQ(kernel.method_wfis, 1u);
	EXPECT_EQ(kernel.release_wfi_en, 0u);
#else
	EXPECT_EQ(kernel.start_stamps_en, 1u);
#endif
}
}  // namespace
