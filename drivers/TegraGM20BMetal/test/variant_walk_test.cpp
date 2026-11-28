// SPDX-License-Identifier: MIT
// Every program a record's stage names is one some draw takes (variants.h chooseVariant): a variant the draw whose
// facts are its own assumptions and the pack's chip facts, the generic program the draw holding the chip facts alone. A
// program no draw takes is pack and heap spent for nothing, and the generic one is still placed with its record.
// SWITCH_METAL_VARIANT_WALK_PACK: the built pack to walk (variant_walk_pack_test).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include "../pack.h"
#include "../variants.h"

namespace {

// The first of a stage's `count` variants (`count`: its generic program) that no draw takes; `count` + 1 where every
// one is taken
uint32_t firstUntaken(const gm20b::PackVariant *variants, uint32_t count, uint32_t chipFacts)
{
    for (uint32_t i = 0; i <= count; i++) {
        const uint32_t own = i < count ? variants[i].assume & ~chipFacts : 0;
        if (gm20b::chooseVariant(variants, count, own | chipFacts) != i)
            return i;
    }
    return count + 1;
}

constexpr uint32_t kChip = gm20b::AssumeTexsF16, kResidual = gm20b::AssumeUniformAligned;

}  // namespace

TEST(GM20BVariantWalk, ARecordOfVariantsAndAGenericSomeDrawTakesWalksClean)
{
    const gm20b::PackVariant variants[2] = {{gm20b::AssumeTurned, 1, 0}, {kResidual, 2, 0}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 3u);
    EXPECT_EQ(firstUntaken(nullptr, 0, kChip), 1u);
}

TEST(GM20BVariantWalk, AVariantBehindItsTwinWithAChipFactIsFound)
{
    const gm20b::PackVariant variants[2] = {{kResidual | kChip, 1, 0}, {kResidual, 2, 0}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 1u);
}

TEST(GM20BVariantWalk, AGenericBehindAVariantOfChipFactsAloneIsFound)
{
    const gm20b::PackVariant variants[2] = {{kResidual, 1, 0}, {kChip, 2, 0}};
    EXPECT_EQ(firstUntaken(variants, 2, kChip), 2u);
}

TEST(GM20BVariantWalk, AVariantAssumingNothingHidesTheGeneric)
{
    const gm20b::PackVariant variants[1] = {{0, 1, 0}};
    EXPECT_EQ(firstUntaken(variants, 1, kChip), 1u);
}

#ifdef SWITCH_METAL_VARIANT_WALK_PACK
TEST(GM20BVariantWalk, EveryProgramOfThePackIsOneADrawTakes)
{
    const char *path = getenv("SWITCH_METAL_VARIANT_WALK_PACK");
    ASSERT_NE(path, nullptr);
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(path, 0, err)) << err;
    const uint32_t chip = pack.header()->chipFacts;
    const gm20b::PackPipeline *pipelines = pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    uint32_t untaken = 0, walked = 0;
    for (uint32_t r = 0; r < pack.header()->pipelines; r++)
        for (uint32_t stage = 0; stage < 2; stage++) {
            uint32_t count = pipelines[r].variantCount[stage];
            const gm20b::PackVariant *variants = pack.variants(pipelines[r], stage, count);
            const uint32_t at = firstUntaken(variants, count, chip);
            walked += count + 1;
            if (at <= count && untaken++ < 8)
                ADD_FAILURE() << "stage " << stage << (at < count ? " variant " + std::to_string(at) : " generic")
                              << " of " << count << " variants no draw takes: " << pack.string(pipelines[r].keyString);
        }
    EXPECT_EQ(untaken, 0u) << "of " << walked << " programs walked";
}
#endif
