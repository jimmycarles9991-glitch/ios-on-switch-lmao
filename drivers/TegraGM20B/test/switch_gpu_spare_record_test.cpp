// SwitchGpuSpareRecord.h: one boot's demand recorded, read back at the next boot, and the seed it gives.
#include <gtest/gtest.h>

#include <vector>

#include "drivers/TegraGM20B/SwitchGpuSpareRecord.h"

namespace
{
constexpr uint64_t kCap = SWITCH_GPU_SPARE_CAP_BYTES, kMiB = 1ULL << 20;

uint32_t slotOf(uint64_t bytes, switch_gpu_spare_kind kind)
{
	return switch_gpu_spare_slot(kind, switch_gpu_spare_class_up(bytes));
}

std::vector<uint8_t> encode(const switch_gpu_spare_books &b)
{
	std::vector<uint8_t> out(SWITCH_GPU_SPARE_RECORD_BYTES);
	out.resize(switch_gpu_spare_record_encode(b, out.data()));
	return out;
}

void missOnce(switch_gpu_spare_books &b, uint32_t s)
{
	switch_gpu_spare_books_asked(b, s, SWITCH_GPU_SPARE_NONE);
	switch_gpu_spare_books_learn(b, s, SWITCH_GPU_SPARE_FIRST, 32 * kMiB, kCap);
}

TEST(SpareRecord, ABootsAskedDepthsSeedTheNextBoot)
{
	switch_gpu_spare_books boot{};
	switch_gpu_spare_books_init(boot);
	const uint32_t wc = slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC);
	missOnce(boot, wc);
	const std::vector<uint8_t> record = encode(boot);
	ASSERT_EQ(record.size(), SWITCH_GPU_SPARE_RECORD_BYTES);

	switch_gpu_spare_books next{};
	switch_gpu_spare_books_init(next);
	EXPECT_EQ(switch_gpu_spare_record_seed(next, record.data(), uint32_t(record.size()), kCap),
		  1u);
	EXPECT_EQ(next.depth[wc], 1);
	EXPECT_EQ(next.used[wc], SWITCH_GPU_SPARE_SEEDED);
	EXPECT_EQ(switch_gpu_spare_books_refill(next, kCap), wc)
		<< "a seeded class refills before the warm classes nobody has asked for";
}

// review-memory3: the record kept 32 entries in slot order, so a boot that asked more than 32 cached classes lost every
// write-combined one, fresh_spare's 24 MiB among them; one byte a slot keeps them all
TEST(SpareRecord, EveryClassABootAskedIsKeptHoweverManyItAsked)
{
	switch_gpu_spare_books boot{};
	switch_gpu_spare_books_init(boot);
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
		missOnce(boot, switch_gpu_spare_slot(SWITCH_GPU_SPARE_CACHED, c));
	const uint32_t wc = slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC);
	missOnce(boot, wc);
	uint32_t learned = 0;
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; s++)
		learned += switch_gpu_spare_books_target(boot, s) != 0;
	ASSERT_GT(learned, 32u) << "more classes than the old record's 32 entries";
	const std::vector<uint8_t> record = encode(boot);
	switch_gpu_spare_books next{};
	switch_gpu_spare_books_init(next);
	EXPECT_EQ(switch_gpu_spare_record_seed(next, record.data(), uint32_t(record.size()), kCap),
		  learned);
	EXPECT_EQ(next.used[wc], SWITCH_GPU_SPARE_SEEDED);
	EXPECT_EQ(next.depth[wc], 1);
}

// A seed this boot never asked for again is not recorded: depths do not grow from boot to boot on their own
TEST(SpareRecord, ASeedNoBootAsksForEndsAfterOneBoot)
{
	switch_gpu_spare_books b{};
	switch_gpu_spare_books_init(b);
	const uint32_t idle = slotOf(16 * kMiB, SWITCH_GPU_SPARE_WC);
	switch_gpu_spare_books_seed(b, idle, 1, kCap);
	const uint32_t asked = slotOf(20 * kMiB, SWITCH_GPU_SPARE_WC);
	switch_gpu_spare_books_seed(b, asked, 1, kCap);
	switch_gpu_spare_books_asked(b, asked, asked);
	const std::vector<uint8_t> record = encode(b);
	EXPECT_EQ(record[SWITCH_GPU_SPARE_RECORD_HEADER + idle], 0);
	EXPECT_EQ(record[SWITCH_GPU_SPARE_RECORD_HEADER + asked], 1)
		<< "the seeded depth, unchanged by a hit";
}

// A record of another version, length or class table seeds nothing, and a depth past the most is skipped
TEST(SpareRecord, ARecordOfAnotherBuildSeedsNothingWrong)
{
	switch_gpu_spare_books b{};
	switch_gpu_spare_books_init(b);
	const uint32_t wc = slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC);
	missOnce(b, wc);
	const std::vector<uint8_t> record = encode(b);
	const auto seeds = [](std::vector<uint8_t> r) {
		switch_gpu_spare_books n{};
		switch_gpu_spare_books_init(n);
		return switch_gpu_spare_record_seed(n, r.data(), uint32_t(r.size()), kCap);
	};
	EXPECT_EQ(seeds(record), 1u);
	for (uint32_t at = 0; at < SWITCH_GPU_SPARE_RECORD_HEADER; at++) {
		std::vector<uint8_t> other = record;
		other[at] ^= 1;
		EXPECT_EQ(seeds(other), 0u) << "header byte " << at;
	}
	std::vector<uint8_t> other = record;
	other.pop_back();
	EXPECT_EQ(seeds(other), 0u);
	other = record;
	other[SWITCH_GPU_SPARE_RECORD_HEADER + wc] = SWITCH_GPU_SPARE_DEPTH_MAX + 1;
	EXPECT_EQ(seeds(other), 0u);
	EXPECT_EQ(seeds({}), 0u);
}

TEST(SpareRecord, TheRecordFitsItsBudget)
{
	static_assert(SWITCH_GPU_SPARE_KINDS <= 255 && SWITCH_GPU_SPARE_CLASSES <= 255);
	EXPECT_EQ(SWITCH_GPU_SPARE_RECORD_BYTES,
		  SWITCH_GPU_SPARE_RECORD_HEADER + SWITCH_GPU_SPARE_SLOTS);
	EXPECT_LE(SWITCH_GPU_SPARE_RECORD_BYTES, 128u)
		<< "of the 2 KiB common partition boot-args shares";
}
}  // namespace
