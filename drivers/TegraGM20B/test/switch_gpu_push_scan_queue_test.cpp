// SwitchGpuPushScanQueue.h: the push scan's submissions waiting for the thread call that reads them.
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/SwitchGpuPushScanQueue.h"

namespace
{

using Queue = SwitchGpuPushScanQueue<2, 4>;

TEST(PushScanQueue, ARecordTakenIsNotHandedOutAgainUntilItIsRead)
{
	Queue q;
	Queue::Record *a = q.reserve();
	ASSERT_NE(a, nullptr);
	q.publish();
	ASSERT_NE(q.reserve(), nullptr);
	q.publish();
	const Queue::Record *taken = q.take();
	EXPECT_EQ(taken, a);
	EXPECT_EQ(q.reserve(), nullptr);
	EXPECT_EQ(q.dropped, 1u);
	q.done();
	EXPECT_EQ(q.reserve(), a);
}

TEST(PushScanQueue, AFilledRecordKeepsWhatItCannotReadAsUnread)
{
	Queue q;
	Queue::Record *r = q.reserve();
	ASSERT_NE(r, nullptr);
	static const uint32_t words[4] = {1, 2, 3, 4};
	switch_gpu_scan_fill(*r, "a-process-name-longer-than-sixteen", true, 3, 4, [&](uint32_t i) {
		return switch_gpu_scan_words{i == 1 ? nullptr : words, i + 1};
	});
	EXPECT_STREQ(r->owner, "a-process-name-l");
	EXPECT_TRUE(r->rawMacros);
	EXPECT_EQ(r->count, 3u);
	EXPECT_EQ(r->push[0].words, words);
	EXPECT_EQ(r->push[1].words, nullptr);
	EXPECT_EQ(r->push[1].dwords, 2u);
	EXPECT_EQ(q.take(), nullptr);
}

}  // namespace
