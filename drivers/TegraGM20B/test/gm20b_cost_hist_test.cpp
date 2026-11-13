// TegraGM20BCostHist.h: the debug lines' quarter-octave buckets and the percentile read from them.
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BCostHist.h"

namespace
{
TEST(CostHist, EverySampleLandsInABucketWhoseTopIsAtLeastIt)
{
	for (uint64_t us = 0; us < 2000000; us = us * 9 / 8 + 1) {
		const uint32_t b = tgm_cost_bucket(us * 1000);
		ASSERT_LT(b, TGM_COST_BUCKETS);
		EXPECT_GE(tgm_cost_bucket_top_us(b), us) << us;
		const uint32_t below = b == 8 ? 3 : b - 1;  // buckets 4-7 are never used
		if (b > 0)
			EXPECT_LT(tgm_cost_bucket_top_us(below), us) << us;
	}
}

TEST(CostHist, P95OfAWindowWithOneSlowFrameIsTheCommonFrame)
{
	uint32_t hist[TGM_COST_BUCKETS] = {};
	for (int i = 0; i < 299; i++)
		hist[tgm_cost_bucket(2441000)]++;
	hist[tgm_cost_bucket(9182000)]++;
	const uint64_t p95 = tgm_cost_percentile(hist, 300, 950);
	EXPECT_GE(p95, 2441u);
	EXPECT_LT(p95, 2441u * 120 / 100);
	EXPECT_GE(tgm_cost_percentile(hist, 300, 1000), 9182u);
}

TEST(CostHist, AnEmptyWindowReadsZero)
{
	const uint32_t hist[TGM_COST_BUCKETS] = {};
	EXPECT_EQ(tgm_cost_percentile(hist, 0, 950), 0u);
}
}  // namespace
