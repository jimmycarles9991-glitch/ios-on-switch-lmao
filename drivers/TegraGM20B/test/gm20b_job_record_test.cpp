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
