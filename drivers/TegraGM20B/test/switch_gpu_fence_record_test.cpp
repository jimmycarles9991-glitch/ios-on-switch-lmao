// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include "drivers/TegraGM20B/SwitchGpuFenceRecord.h"

namespace
{
struct Slot {
	alignas(8) uint32_t words[16] = {};

	void identify(uint64_t uid)
	{
		memcpy(&words[8], &uid, sizeof(uid));
	}
};
}  // namespace

TEST(FenceRecord, PassedOnlyByTheLiveChannelTheWaitBeganOn)
{
	Slot slot;
	slot.identify(41);
	slot.words[0] = 9;
	EXPECT_TRUE(switch_gpu_fence_passed(slot.words, 9, 41));
	EXPECT_TRUE(switch_gpu_fence_passed(slot.words, 3, 41));
	EXPECT_FALSE(switch_gpu_fence_passed(slot.words, 10, 41));
	EXPECT_FALSE(switch_gpu_fence_passed(slot.words, 9, 42));
	slot.words[0] = 0x80000001u;
	EXPECT_TRUE(switch_gpu_fence_passed(slot.words, 0x7ffffff0u, 41));
}

// A kill moves the fence past every seqno it gave out after writing the error: that is not the job completing.
TEST(FenceRecord, AKilledChannelsFenceIsNotItsWork)
{
	Slot slot;
	slot.identify(7);
	slot.words[3] = 4;
	slot.words[0] = 100;
	EXPECT_FALSE(switch_gpu_fence_passed(slot.words, 50, 7));
}

// The slot's next channel continues its seqnos: a wait that began on the old one is not passed by the new one's fence.
TEST(FenceRecord, ANewChannelInTheSlotIsAnotherChannel)
{
	Slot slot;
	slot.identify(7);
	slot.words[0] = 5;
	slot.identify(8);
	slot.words[0] = 6;
	EXPECT_FALSE(switch_gpu_fence_passed(slot.words, 6, 7));
	EXPECT_EQ(switch_gpu_fence_identity(slot.words), 8u);
}
