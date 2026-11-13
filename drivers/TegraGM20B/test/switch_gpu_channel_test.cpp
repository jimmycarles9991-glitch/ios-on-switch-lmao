// SPDX-License-Identifier: MIT
#include <array>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuChannelPolicy.h"

TEST(ChannelPolicy, ExistingEngineMasksPreserveFullContextBehavior)
{
	for (uint32_t engines = 0; engines < 32; ++engines)
		EXPECT_EQ(switch_gpu_channel_copy_only(engines), 0);
}

TEST(ChannelPolicy, StrictCopyRejectsMixedOrMissingEngineRequests)
{
	for (uint32_t engines = 0; engines < 32; ++engines)
		EXPECT_EQ(switch_gpu_channel_copy_only(engines | TegraGM20BEngineCopyOnly),
			  engines == TegraGM20BEngineCopy ? 1 : -22);
	EXPECT_EQ(switch_gpu_channel_copy_only(TegraGM20BEngineCopyOnly | TegraGM20BEngineCopy |
					       (1u << 20)),
		  -22);
}

TEST(ChannelPolicy, CopyOnlyBindsExactlyOneCeObjectOnSubchannelFour)
{
	const uint32_t classes[] = {0xb197, 0xb1c0, 0xa140, 0x902d, 0xb0b5};
	std::array<uint32_t, 4> words = {0xdeadbeef, 0, 0, 0xdeadbeef};
	ASSERT_EQ(switch_gpu_channel_bind_words(words.data() + 1, classes, 1), 2u);
	EXPECT_EQ(switch_gpu_channel_first_object(1), 4u);
	EXPECT_EQ(words[0], 0xdeadbeef);
	EXPECT_EQ(words[3], 0xdeadbeef);
	EXPECT_EQ(words[1] >> 29, 1u);
	EXPECT_EQ((words[1] >> 16) & 0x1fff, 1u);
	EXPECT_EQ((words[1] >> 13) & 7, 4u);
	EXPECT_EQ(words[1] & 0x1fff, 0u);
	EXPECT_EQ(words[2], 0xb0b5u);
}

TEST(ChannelPolicy, LegacyBindingPreservesAllSubchannelsAndClasses)
{
	const uint32_t classes[] = {0xb197, 0xb1c0, 0xa140, 0x902d, 0xb0b5};
	uint32_t words[10] = {};
	ASSERT_EQ(switch_gpu_channel_bind_words(words, classes, 0), 10u);
	EXPECT_EQ(switch_gpu_channel_first_object(0), 0u);
	for (uint32_t i = 0; i < 5; ++i) {
		EXPECT_EQ((words[2 * i] >> 13) & 7, i);
		EXPECT_EQ(words[2 * i + 1], classes[i]);
	}
}
