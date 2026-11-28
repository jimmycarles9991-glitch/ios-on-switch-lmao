// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuJobRecord.h"

namespace
{
struct Record {
	std::vector<uint64_t> words;
	size_t bytes = 0;

	Record(TegraGM20BAbiJob head, uint32_t waits = 0, uint32_t signals = 0)
	{
		bytes = sizeof(head) + head.push_count * sizeof(TegraGM20BAbiPush) +
			head.shared_count * sizeof(TegraGM20BAbiSurfaceUse);
		if (head.flags & TegraGM20BJobHasEvents)
			bytes += sizeof(TegraGM20BAbiSubmitEvents) +
				 (waits + signals) * sizeof(TegraGM20BAbiEventUse);
		words.assign((bytes + 7) / 8, 0);
		memcpy(words.data(), &head, sizeof(head));
		if (head.flags & TegraGM20BJobHasEvents) {
			auto *base = reinterpret_cast<uint8_t *>(words.data());
			auto *events = reinterpret_cast<TegraGM20BAbiSubmitEvents *>(
				base + bytes - sizeof(TegraGM20BAbiSubmitEvents) -
				(waits + signals) * sizeof(TegraGM20BAbiEventUse));
			*events = {waits, signals, 0};
			auto *uses = reinterpret_cast<TegraGM20BAbiEventUse *>(events + 1);
			for (uint32_t i = 0; i < waits + signals; ++i)
				uses[i] = {i + 1, 1};
		}
	}

	SwitchGpuJobRecord parse(size_t size = 0)
	{
		return SwitchGpuJobRecord::parse(words.data(), size ? size : bytes);
	}
};
}  // namespace

TEST(JobRecord, PartsFollowTheHeader)
{
	Record record({1, 2, 3, 7, 4, TegraGM20BJobExternal | TegraGM20BJobHasEvents}, 1, 2);
	const auto parsed = record.parse();
	ASSERT_TRUE(parsed.valid());
	EXPECT_EQ(reinterpret_cast<uint8_t *>(parsed.pushes) -
			  reinterpret_cast<uint8_t *>(parsed.job),
		  24);
	EXPECT_EQ(reinterpret_cast<const uint8_t *>(parsed.uses) -
			  reinterpret_cast<uint8_t *>(parsed.job),
		  24 + 32);
	ASSERT_NE(parsed.events, nullptr);
	EXPECT_EQ(parsed.events->signal_count, 2u);
	EXPECT_EQ(Record({1, 0, 0, 0, 0, 0}).parse().events, nullptr);
	EXPECT_TRUE(Record({1, 1, 0, 0, 0, TegraGM20BJobVolatileWrites}).parse().valid());
	const uint32_t volatileQuiet = uint32_t(TegraGM20BJobVolatileWrites) | TegraGM20BJobQuiet;
	EXPECT_TRUE(Record({1, 1, 0, 0, 0, volatileQuiet}).parse().valid());
}

TEST(JobRecord, MalformedRecordsAreRefused)
{
	EXPECT_FALSE(Record({0, 0, 0, 0, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 0, 0, 0, 1u << 7}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 1, 0, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 0, 3, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, TegraGM20BMaxResourceSets + 1, 1, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, TegraGM20BMaxJobPushes + 1, 0, 0, 0, 0}).parse().valid());
	Record ok({1, 1, 0, 0, 1, 0});
	EXPECT_TRUE(ok.parse().valid());
	EXPECT_FALSE(ok.parse(ok.bytes - 8).valid());
	EXPECT_FALSE(Record({1, 0, 0, 0, 0, TegraGM20BJobHasEvents}, TegraGM20BMaxJobEvents, 1)
			     .parse()
			     .valid());
	Record events({1, 0, 0, 0, 0, TegraGM20BJobHasEvents}, 1, 0);
	EXPECT_TRUE(events.parse().valid());
	EXPECT_FALSE(events.parse(events.bytes - sizeof(TegraGM20BAbiEventUse)).valid());
	EXPECT_LE(SwitchGpuJobRecord::kMaxBytes, size_t(1) << 16);
}

// A command buffer's admission: its cookie follows the shared uses, before the events; Cancel carries no work
TEST(JobRecord, AdmissionCookieFollowsTheSharedUses)
{
	const TegraGM20BAbiJob head = {3, 1, 0,
				       0, 2, TegraGM20BJobAdmissionBegin | TegraGM20BJobHasEvents};
	std::vector<uint8_t> bytes(
		sizeof(head) + sizeof(TegraGM20BAbiPush) + 2 * sizeof(TegraGM20BAbiSurfaceUse) +
		sizeof(TegraGM20BAbiSubmitAdmission) + sizeof(TegraGM20BAbiSubmitEvents) +
		sizeof(TegraGM20BAbiEventUse));
	uint8_t *at = bytes.data();
	memcpy(at, &head, sizeof(head));
	at += sizeof(head) + sizeof(TegraGM20BAbiPush) + 2 * sizeof(TegraGM20BAbiSurfaceUse);
	const TegraGM20BAbiSubmitAdmission lease = {41, 0};
	memcpy(at, &lease, sizeof(lease));
	at += sizeof(lease);
	const TegraGM20BAbiSubmitEvents events = {0, 1, 0};
	memcpy(at, &events, sizeof(events));
	const TegraGM20BAbiEventUse signal = {9, 1};
	memcpy(at + sizeof(events), &signal, sizeof(signal));
	const auto record = SwitchGpuJobRecord::parse(bytes.data(), bytes.size());
	ASSERT_TRUE(record.valid());
	EXPECT_EQ(record.admission, 41u);
	ASSERT_NE(record.events, nullptr);
	EXPECT_EQ(record.events->signal_count, 1u);
	EXPECT_FALSE(
		SwitchGpuJobRecord::parse(bytes.data(), bytes.size() - sizeof(signal)).valid());

	const TegraGM20BAbiJob both = {
		3, 0, 0, 0, 0, TegraGM20BJobAdmissionBegin | TegraGM20BJobAdmissionContinue};
	const TegraGM20BAbiJob endAlone = {3, 0, 0, 0, 0, TegraGM20BJobAdmissionEnd};
	const TegraGM20BAbiJob cancelWork = {3, 1, 0, 0, 0, TegraGM20BJobAdmissionCancel};
	for (const TegraGM20BAbiJob &bad : {both, endAlone, cancelWork}) {
		std::vector<uint8_t> record(sizeof(bad) +
					    bad.push_count * sizeof(TegraGM20BAbiPush) +
					    sizeof(TegraGM20BAbiSubmitAdmission));
		memcpy(record.data(), &bad, sizeof(bad));
		memcpy(record.data() + record.size() - sizeof(lease), &lease, sizeof(lease));
		EXPECT_FALSE(SwitchGpuJobRecord::parse(record.data(), record.size()).valid())
			<< bad.flags;
	}
	const TegraGM20BAbiJob cancel = {3, 0, 0, 0, 0, TegraGM20BJobAdmissionCancel};
	std::vector<uint8_t> zero(sizeof(cancel) + sizeof(lease));
	memcpy(zero.data(), &cancel, sizeof(cancel));
	EXPECT_FALSE(SwitchGpuJobRecord::parse(zero.data(), zero.size()).valid())
		<< "a zero cookie was taken";
	memcpy(zero.data() + sizeof(cancel), &lease, sizeof(lease));
	EXPECT_TRUE(SwitchGpuJobRecord::parse(zero.data(), zero.size()).valid());
}

TEST(JobRecord, SameUsesAcceptsZeroSharedUsesAndRejectsListedUsesOrCancel)
{
	EXPECT_TRUE(Record({1, 2, 0, 0, 0, TegraGM20BJobSameUses}).parse().valid());
	const uint32_t sameQuiet = uint32_t(TegraGM20BJobSameUses) | TegraGM20BJobQuiet;
	EXPECT_TRUE(Record({1, 2, 1, 7, 0, sameQuiet}).parse().valid());
	EXPECT_FALSE(Record({1, 2, 0, 0, 1, TegraGM20BJobSameUses}).parse().valid());
	const TegraGM20BAbiJob cancelSame = {
		3, 0, 0, 0, 0, uint32_t(TegraGM20BJobAdmissionCancel) | TegraGM20BJobSameUses};
	const TegraGM20BAbiSubmitAdmission lease = {41, 0};
	std::vector<uint8_t> bytes(sizeof(cancelSame) + sizeof(lease));
	memcpy(bytes.data(), &cancelSame, sizeof(cancelSame));
	memcpy(bytes.data() + sizeof(cancelSame), &lease, sizeof(lease));
	EXPECT_FALSE(SwitchGpuJobRecord::parse(bytes.data(), bytes.size()).valid());
}

TEST(JobRecord, StageCopiesHeaderAndTailAtExactByteOffsets)
{
	const TegraGM20BAbiJob head = {
		1,
		2,
		3,
		7,
		1,
		TegraGM20BJobExternal | TegraGM20BJobAdmissionBegin | TegraGM20BJobFulfills |
			TegraGM20BJobHasEvents | TegraGM20BJobEventReleases};
	const TegraGM20BAbiPush pushes[2] = {
		{0x10000000ull, 42, 0},
		{0x10001000ull, 16, TegraGM20BPushNoPrefetch},
	};
	const TegraGM20BAbiSurfaceUse use = {99, TegraGM20BSurfaceWrite, 0};
	const TegraGM20BAbiSubmitAdmission admission = {0x123456789abcdef0ull, 0};
	const TegraGM20BAbiJobPromises promises = {1, 0, {5}};
	const TegraGM20BAbiSubmitEvents events = {1, 1, 0};
	const TegraGM20BAbiEventUse eventUses[2] = {{11, 22}, {33, 44}};
	const TegraGM20BAbiSignalRelease release = {7, TegraGM20BSignalReleased};

	std::vector<uint8_t> user(sizeof(head) + sizeof(pushes) + sizeof(use) + sizeof(admission) +
				  sizeof(promises) + sizeof(events) + sizeof(eventUses) +
				  sizeof(release));
	uint8_t *at = user.data();
	auto put = [&](const void *src, size_t n) {
		memcpy(at, src, n);
		at += n;
	};
	put(&head, sizeof(head));
	put(pushes, sizeof(pushes));
	put(&use, sizeof(use));
	put(&admission, sizeof(admission));
	put(&promises, sizeof(promises));
	put(&events, sizeof(events));
	put(eventUses, sizeof(eventUses));
	put(&release, sizeof(release));

	SwitchGpuJobBuffer scratch;
	memset(&scratch, 0xaa, sizeof(scratch));
	const auto parsed = scratch.stage(head, user.size(), [&](uint8_t *dst, size_t tail) {
		if (tail != user.size() - sizeof(head))
			return false;
		memcpy(dst, user.data() + sizeof(head), tail);
		return true;
	});
	ASSERT_TRUE(parsed.valid());
	ASSERT_EQ(parsed.job->push_count, 2u);
	EXPECT_EQ(parsed.pushes[0].va, pushes[0].va);
	EXPECT_EQ(parsed.pushes[0].dwords, pushes[0].dwords);
	EXPECT_EQ(parsed.pushes[0].flags, pushes[0].flags);
	EXPECT_EQ(parsed.pushes[1].va, pushes[1].va);
	EXPECT_EQ(parsed.pushes[1].dwords, pushes[1].dwords);
	EXPECT_EQ(parsed.pushes[1].flags, pushes[1].flags);
	EXPECT_EQ(parsed.uses[0].handle, use.handle);
	EXPECT_EQ(parsed.uses[0].access, use.access);
	EXPECT_EQ(parsed.admission, admission.cookie);
	ASSERT_NE(parsed.promises, nullptr);
	EXPECT_EQ(parsed.promises->promise[0], 5u);
	ASSERT_NE(parsed.events, nullptr);
	EXPECT_EQ(parsed.events->wait_count, 1u);
	EXPECT_EQ(parsed.events->signal_count, 1u);
	ASSERT_NE(parsed.releases, nullptr);
	EXPECT_EQ(parsed.releases[0].payload, 7u);
}

TEST(JobRecord, StageHandlesMaxSizeHeaderOnlyAndErrors)
{
	struct Guarded {
		SwitchGpuJobBuffer buffer;
		uint64_t guard = 0x55aa55aa55aa55aaull;
	} guarded;
	memset(&guarded.buffer, 0, sizeof(guarded.buffer));

	const TegraGM20BAbiJob maxHead = {
		1,
		TegraGM20BMaxJobPushes,
		0,
		0,
		TegraGM20BMaxJobUses,
		TegraGM20BJobAdmissionBegin | TegraGM20BJobFulfills | TegraGM20BJobHasEvents |
			TegraGM20BJobEventReleases};
	std::vector<uint8_t> maxUser(SwitchGpuJobRecord::kMaxBytes, 0);
	uint8_t *at = maxUser.data();
	memcpy(at, &maxHead, sizeof(maxHead));
	at += sizeof(maxHead);
	for (uint32_t i = 0; i < TegraGM20BMaxJobPushes; ++i) {
		const TegraGM20BAbiPush push = {0x20000000ull + uint64_t(i) * 64, 4 + i, 0};
		memcpy(at, &push, sizeof(push));
		at += sizeof(push);
	}
	for (uint32_t i = 0; i < TegraGM20BMaxJobUses; ++i) {
		const TegraGM20BAbiSurfaceUse use = {i + 1, TegraGM20BSurfaceRead, 0};
		memcpy(at, &use, sizeof(use));
		at += sizeof(use);
	}
	const TegraGM20BAbiSubmitAdmission admission = {77, 0};
	memcpy(at, &admission, sizeof(admission));
	at += sizeof(admission);
	const TegraGM20BAbiJobPromises promises = {1, 0, {1}};
	memcpy(at, &promises, sizeof(promises));
	at += sizeof(promises);
	const TegraGM20BAbiSubmitEvents events = {0, TegraGM20BMaxJobEvents, 0};
	memcpy(at, &events, sizeof(events));
	at += sizeof(events);
	for (uint32_t i = 0; i < TegraGM20BMaxJobEvents; ++i) {
		const TegraGM20BAbiEventUse ev = {i + 1, i + 10};
		memcpy(at, &ev, sizeof(ev));
		at += sizeof(ev);
	}
	for (uint32_t i = 0; i < TegraGM20BMaxJobEvents; ++i) {
		const TegraGM20BAbiSignalRelease rel = {i + 1, TegraGM20BSignalReleased};
		memcpy(at, &rel, sizeof(rel));
		at += sizeof(rel);
	}
	ASSERT_EQ(size_t(at - maxUser.data()), SwitchGpuJobRecord::kMaxBytes);

	const auto maxParsed = guarded.buffer.stage(
		maxHead, maxUser.size(), [&](uint8_t *dst, size_t tail) {
			memcpy(dst, maxUser.data() + sizeof(maxHead), tail);
			return true;
		});
	ASSERT_TRUE(maxParsed.valid());
	EXPECT_EQ(guarded.guard, 0x55aa55aa55aa55aaull);
	EXPECT_EQ(maxParsed.pushes[0].va, 0x20000000ull);
	EXPECT_EQ(maxParsed.pushes[TegraGM20BMaxJobPushes - 1].dwords,
		  4u + TegraGM20BMaxJobPushes - 1);
	EXPECT_EQ(maxParsed.releases[TegraGM20BMaxJobEvents - 1].payload, TegraGM20BMaxJobEvents);

	bool called = false;
	const TegraGM20BAbiJob headerOnly = {1, 0, 0, 0, 0, 0};
	const auto empty = guarded.buffer.stage(
		headerOnly, sizeof(headerOnly), [&](uint8_t *, size_t) {
			called = true;
			return true;
		});
	EXPECT_TRUE(empty.valid());
	EXPECT_FALSE(called);

	EXPECT_FALSE(guarded.buffer
			     .stage(headerOnly, sizeof(headerOnly) - 1,
				    [](uint8_t *, size_t) { return true; })
			     .valid());
	EXPECT_FALSE(guarded.buffer
			     .stage(maxHead, SwitchGpuJobRecord::kMaxBytes + 1,
				    [](uint8_t *, size_t) { return true; })
			     .valid());
	EXPECT_FALSE(guarded.buffer
			     .stage(maxHead, maxUser.size(),
				    [](uint8_t *, size_t) { return false; })
			     .valid());
}
