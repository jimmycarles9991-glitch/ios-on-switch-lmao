// SPDX-License-Identifier: MIT
// The push scan's thread-call read (TegraGM20BPushScanRun.cpp deferredWork) over a known submission: a record queued as
// deferScan fills it, taken and read through switch_gpu_scan_record and tgm_scan::readSubmission. Its words are the
// submission's only until the submission completes; boot 22's off-thread reads of words a process had written again
// gave targets of two method headers (iconservicesagent 2147484889x2147484890, 0x800004d9 x 0x800004da) and no
// interlocks.
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "drivers/TegraGM20B/SwitchGpuPushScanQueue.h"
#include "drivers/TegraGM20B/TegraGM20BPushScan.h"

namespace
{
using Queue = SwitchGpuPushScanQueue<4, 8>;
using Words = std::vector<uint32_t>;

constexpr uint64_t kBudget = 512 << 10;
constexpr uint32_t kSeqno = 41;

uint32_t inc(uint32_t subc, uint32_t mthd, uint32_t count)
{
	return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}

uint32_t immd(uint32_t subc, uint32_t mthd, uint32_t data)
{
	return (4u << 29) | (data << 16) | (subc << 13) | (mthd >> 2);
}

// Colour target 0's address, then a pass into a 1280x720 block-linear target that turns the pixel interlock on and
// draws once
struct Submission {
	Words state = {inc(0, 0x0800, 2), 0x3e, 0xf7e0c000};
	Words pass = {inc(0, 0x0808, 4), 1280, 720, 0xcf, 0, immd(0, 0x1538, 0), immd(0, 0x1224, 2),
		      immd(0, 0x1618, 1)};
};

// The process's next frame written over the same words: the first push now ends inside SET_COLOR_TARGET_A..MEMORY, so
// read with the old pushes' lengths the second push's first two headers land in WIDTH and HEIGHT
void writeNextFrame(Submission &s)
{
	s.state = {inc(0, 0x0800, 6), 0x3e, 0xf7e0c000};
	s.pass = {immd(0, 0x1364, 0), immd(0, 0x1368, 0), 0xcf, 0, immd(0, 0x1618, 1), 0, 0, 0};
}

struct Fence {
	uint32_t completed = kSeqno - 1;
};

Queue::Record *queued(Queue &q, Submission &s)
{
	Queue::Record *r = q.reserve();
	EXPECT_NE(r, nullptr);
	const Words *pushes[] = {&s.state, &s.pass};
	switch_gpu_scan_fill(*r, "iconservicesagent", true, 2, 8, [&](uint32_t i) {
		return switch_gpu_scan_words{pushes[i]->data(),
					     static_cast<uint32_t>(pushes[i]->size())};
	});
	r->seqno = kSeqno;
	q.publish();
	return r;
}

tgm_scan::SubmissionRead readRecord(const Queue::Record &record)
{
	return tgm_scan::readSubmission(record.push, record.count, nullptr, record.rawMacros,
					kBudget);
}

TEST(PushScanDeferred, TheThreadCallReadsAKnownSubmissionsTargetAndInterlock)
{
	Queue q;
	Submission s;
	Fence fence;
	queued(q, s);
	const Queue::Record *r = q.take();
	ASSERT_NE(r, nullptr);
	tgm_scan::SubmissionRead read = {};
	const bool kept = switch_gpu_scan_record(
		*r,
		[&](const Queue::Record &x) {
			return static_cast<int32_t>(fence.completed - x.seqno) >= 0;
		},
		[&](const Queue::Record &x) {
			read = readRecord(x);
		});
	q.done();
	EXPECT_TRUE(kept);
	EXPECT_EQ(read.largest.width, 1280u);
	EXPECT_EQ(read.largest.height, 720u);
	EXPECT_EQ(read.largest.samples, 1u);
	EXPECT_EQ(read.c.interlocks, 1u);
	EXPECT_EQ(read.c.passes, 1u);
	EXPECT_EQ(read.c.draws, 1u);
	EXPECT_EQ(read.c.readBytes, 4u * (3 + 8));
}

// What the read finds once the process has written its next frame over the words: boot 22's target
TEST(PushScanDeferred, WordsWrittenAgainReadAsAnotherFramesHeaders)
{
	Queue q;
	Submission s;
	queued(q, s);
	writeNextFrame(s);
	const tgm_scan::SubmissionRead read = readRecord(*q.take());
	EXPECT_EQ(read.largest.width, 2147484889u);
	EXPECT_EQ(read.largest.height, 2147484890u);
	EXPECT_EQ(read.c.interlocks, 0u);
}

TEST(PushScanDeferred, AReadAfterTheSubmissionCompletedIsNotCounted)
{
	Queue q;
	Submission s;
	Fence fence;
	queued(q, s);
	fence.completed = kSeqno;
	writeNextFrame(s);
	unsigned reads = 0;
	tgm_scan::SubmissionRead read = {};
	const bool kept = switch_gpu_scan_record(
		*q.take(),
		[&](const Queue::Record &x) {
			return static_cast<int32_t>(fence.completed - x.seqno) >= 0;
		},
		[&](const Queue::Record &x) {
			read = readRecord(x);
			reads++;
		});
	EXPECT_FALSE(kept) << "a target of " << read.largest.width << "x" << read.largest.height
			   << " would go into the peaks";
	EXPECT_EQ(reads, 0u);
}

TEST(PushScanDeferred, AReadTheSubmissionsCompletionOvertookIsNotCounted)
{
	Queue q;
	Submission s;
	Fence fence;
	queued(q, s);
	tgm_scan::SubmissionRead read = {};
	const bool kept = switch_gpu_scan_record(
		*q.take(),
		[&](const Queue::Record &x) {
			return static_cast<int32_t>(fence.completed - x.seqno) >= 0;
		},
		[&](const Queue::Record &x) {
			fence.completed = kSeqno + 3;
			writeNextFrame(s);
			read = readRecord(x);
		});
	EXPECT_FALSE(kept) << "a target of " << read.largest.width << "x" << read.largest.height
			   << " would go into the peaks";
}

// The queue's one-reader rule (the thread call is THREAD_CALL_OPTIONS_ONCE): a second reader is handed the record the
// first is reading.
TEST(PushScanDeferred, TakeHandsTheOldestRecordToEveryReaderUntilItsDone)
{
	Queue q;
	Submission s;
	queued(q, s);
	queued(q, s);
	const Queue::Record *first = q.take();
	EXPECT_EQ(q.take(), first);
	q.done();
	EXPECT_NE(q.take(), first);
}
}  // namespace
