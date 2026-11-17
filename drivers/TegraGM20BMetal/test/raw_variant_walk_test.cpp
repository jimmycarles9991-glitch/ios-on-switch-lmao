// SPDX-License-Identifier: MIT
// Every program a record's stage names is one some draw takes (raw_variants.h chooseVariant): a variant the draw whose
// facts are its own assumptions and the pack's chip facts, the generic program the draw holding the chip facts alone. A
// program no draw takes is pack and heap spent for nothing, and the generic one is still placed with its record.
// RAW_VARIANT_WALK_PACK: the built pack to walk (raw_variant_walk_pack_test).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_variants.h"

namespace {

// The first of a stage's `count` variants (`count`: its generic program) that no draw takes; `count` + 1 where every
// one is taken
uint32_t firstUntaken(const raw::PackVariant *variants, uint32_t count, uint32_t chipFacts)
{
    for (uint32_t i = 0; i <= count; i++) {
        const uint32_t own = i < count ? variants[i].assume & ~chipFacts : 0;
        if (raw::chooseVariant(variants, count, own | chipFacts) != i)
            return i;
    }
    return count + 1;
}

constexpr uint32_t kChip = raw::AssumeTexsF16, kResidual = raw::AssumeUniformAligned;

}  // namespace

TEST(RawVariantWalk, ARecordOfVariantsAndAGenericSomeDrawTakesWalksClean)
{
    const raw::PackVariant variants[2] = {{raw::AssumeTurned, 1}, {kResidual, 2}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 3u);
    EXPECT_EQ(firstUntaken(nullptr, 0, kChip), 1u);
}

TEST(RawVariantWalk, AVariantBehindItsTwinWithAChipFactIsFound)
{
    const raw::PackVariant variants[2] = {{kResidual | kChip, 1}, {kResidual, 2}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 1u);
}

TEST(RawVariantWalk, AGenericBehindAVariantOfChipFactsAloneIsFound)
{
    const raw::PackVariant variants[2] = {{kResidual, 1}, {kChip, 2}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 2u);
}

TEST(RawVariantWalk, AVariantAssumingNothingHidesTheGeneric)
{
    const raw::PackVariant variants[1] = {{0, 1}};
    EXPECT_EQ(firstUntaken(variants, 1, kChip), 1u);
}

#ifdef RAW_VARIANT_WALK_PACK
TEST(RawVariantWalk, EveryProgramOfThePackIsOneADrawTakes)
{
    const char *path = getenv("RAW_VARIANT_WALK_PACK");
    ASSERT_NE(path, nullptr);
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(path, 0, err)) << err;
    const uint32_t chip = pack.header()->chipFacts;
    const raw::PackPipeline *pipelines = pack.section<raw::PackPipeline>(raw::PackPipelines);
    uint32_t untaken = 0, walked = 0;
    for (uint32_t r = 0; r < pack.header()->pipelines; r++)
        for (uint32_t stage = 0; stage < 2; stage++) {
            uint32_t count = pipelines[r].variantCount[stage];
            const raw::PackVariant *variants = pack.variants(pipelines[r], stage, count);
            const uint32_t at = firstUntaken(variants, count, chip);
            walked += count + 1;
            if (at <= count && untaken++ < 8)
                ADD_FAILURE() << "stage " << stage << (at < count ? " variant " + std::to_string(at) : " generic")
                              << " of " << count << " variants no draw takes: " << pack.string(pipelines[r].keyString);
        }
    EXPECT_EQ(untaken, 0u) << "of " << walked << " programs walked";
}
#endif
