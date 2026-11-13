// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_pass_fusion.h"
#include "../raw/runtime/raw_attachment_lifetime.h"
#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace {
struct Image {
    uint64_t va = 4096;
    uint32_t pitch = 256, height = 64, depth = 1, layers = 1;
    uint64_t layerBytes = 0;

    struct Span {
        uint64_t bytes = 0;
    } span;

    struct Chain {
        Span layout;
    } *chain = nullptr;

    bool blockLinear = false, shadowTarget = false, memoryless = false;
    bool sampled = false, standIn = false, planarStandIn = false, conversion = false;

    bool converted() const
    {
        return conversion;
    }
};

raw::FusionPass pass()
{
    raw::FusionPass p;
    p.bodySafe = true;
    p.width = 720;
    p.height = 1280;
    p.attachments[0] = {0x100, 0, true, true, true, false};
    return p;
}

TEST(PassFusion, StoredAttachmentCanContinueAcrossClearOrDiscardInitialLoads)
{
    auto a = pass(), b = pass();
    a.attachments[0].loads = false;
    EXPECT_TRUE(raw::canFusePasses(a, b));
    b.attachments[0].stores = false;
    EXPECT_TRUE(raw::canFusePasses(a, b));
    EXPECT_FALSE(raw::canFusePasses(b, pass()));
}

TEST(PassFusion, ResolvesClearsAndDiscardedResultsKeepTheirBoundaries)
{
    const auto a = pass();
    auto b = a;
    b.attachments[0].loads = false;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    b = a;
    b.attachments[0].resolves = true;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    EXPECT_FALSE(raw::canFusePasses(b, a));
    b = a;
    b.attachments[0].stores = false;
    EXPECT_FALSE(raw::canFusePasses(b, a));
}

TEST(PassFusion, EveryColorDepthAndStencilAttachmentMustMatch)
{
    auto a = pass();
    for (size_t i = 0; i < a.attachments.size(); ++i) {
        a.attachments[i] = {0x100 + i, 0, true, true, true, false};
        auto b = a;
        EXPECT_TRUE(raw::canFusePasses(a, b));
        b.attachments[i].identity++;
        EXPECT_FALSE(raw::canFusePasses(a, b));
        b = a;
        b.attachments[i].level++;
        EXPECT_FALSE(raw::canFusePasses(a, b));
        b = a;
        b.attachments[i].slice++;
        EXPECT_FALSE(raw::canFusePasses(a, b));
        b = a;
        b.attachments[i].native = false;
        EXPECT_FALSE(raw::canFusePasses(a, b));
    }
}

TEST(PassFusion, PassesOfOtherLayersKeepTheirBoundaries)
{
    auto a = pass(), b = a;
    a.layers = b.layers = 6;
    EXPECT_TRUE(raw::canFusePasses(a, b));
    b.layers = 1;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    EXPECT_FALSE(raw::canFusePasses(b, a));
}

TEST(PassFusion, ResourceSideEffectsAndRenderAreaChangesKeepTheirBoundaries)
{
    auto a = pass(), b = a;
    b.bodySafe = false;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    EXPECT_FALSE(raw::canFusePasses(b, a));
    b = a;
    b.width++;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    b = a;
    b.height++;
    EXPECT_FALSE(raw::canFusePasses(a, b));
    a.attachments = {};
    EXPECT_FALSE(raw::canFusePasses(a, a));
}

TEST(PassFusion, AliasRangesUseOverflowSafeHalfOpenBounds)
{
    EXPECT_TRUE(raw::fusionRangesOverlap(0x1000, 0x1000, 0x1800, 0x100));
    EXPECT_TRUE(raw::fusionRangesOverlap(0x1800, 0x100, 0x1000, 0x1000));
    EXPECT_FALSE(raw::fusionRangesOverlap(0x1000, 0x1000, 0x2000, 0x1000));
    EXPECT_FALSE(raw::fusionRangesOverlap(0, 0x1000, 0, 0x1000));
    EXPECT_TRUE(raw::fusionRangesOverlap(UINT64_MAX - 32, 16, UINT64_MAX - 20, 8));
}

TEST(AttachmentLifetime, PitchAndShadowTargetsCanKeepTheSameImageAcrossPasses)
{
    Image image;
    EXPECT_TRUE(raw::fusionTargetEligible(image, 0));
    image.shadowTarget = true;
    EXPECT_TRUE(raw::fusionTargetEligible(image, 0));
    image.blockLinear = true;
    EXPECT_TRUE(raw::fusionTargetEligible(image, 0));
    EXPECT_FALSE(raw::fusionTargetEligible(image, 1));
    image.va = 0;
    EXPECT_FALSE(raw::fusionTargetEligible(image, 0));
    image.va = 4096;
    for (bool Image::*flag :
         {&Image::memoryless, &Image::sampled, &Image::standIn, &Image::planarStandIn, &Image::conversion}) {
        image.*flag = true;
        EXPECT_FALSE(raw::fusionTargetEligible(image, 0));
        image.*flag = false;
    }
}

TEST(AttachmentLifetime, EverySimultaneousScratchMaskHasDisjointStorage)
{
    for (unsigned mask = 0; mask < 256; ++mask) {
        const auto tags = raw::transientColorTags(uint8_t(mask));
        std::set<uint32_t> used;
        for (unsigned slot = 0; slot < 8; ++slot) {
            if (mask & (1u << slot)) {
                ASSERT_TRUE(used.insert(tags[slot]).second) << mask;
                ASSERT_GE(tags[slot], raw::kTransientColorTag);
                ASSERT_LT(tags[slot], raw::kTransientColorTag + 8);
            } else {
                ASSERT_EQ(tags[slot], 0u);
            }
        }
    }
}

TEST(AttachmentLifetime, DisjointPassesUsePeakLiveStorageAcrossAllSlotPermutations)
{
    for (unsigned first = 0; first < 256; ++first)
        for (unsigned next = 0; next < 256; ++next) {
            std::set<uint32_t> held;
            for (const auto mask : {first, next})
                for (const auto tag : raw::transientColorTags(uint8_t(mask)))
                    if (tag)
                        held.insert(tag);
            EXPECT_EQ(held.size(), unsigned(std::max(__builtin_popcount(first), __builtin_popcount(next))));
        }
}

TEST(AttachmentLifetime, CeShadowsNeverReuseCompressedMemorylessBindings)
{
    const int oldMemory = 0, latestMemory = 1;
    EXPECT_TRUE(raw::backingCompressionMatches(false, nullptr, nullptr, false));
    EXPECT_FALSE(raw::backingCompressionMatches(true, &latestMemory, nullptr, false));
    EXPECT_FALSE(raw::backingCompressionMatches(false, nullptr, &latestMemory, true));
    EXPECT_FALSE(raw::backingCompressionMatches(true, &oldMemory, &latestMemory, true));
    EXPECT_TRUE(raw::backingCompressionMatches(true, &latestMemory, &latestMemory, true));
}

TEST(AttachmentLifetime, BufferTexturesExposeTheirRowsToAliasChecks)
{
    Image texture;
    EXPECT_EQ(raw::fusionImageBytes(texture), 16384u);
    EXPECT_TRUE(raw::fusionRangesOverlap(texture.va, raw::fusionImageBytes(texture), texture.va + 8192, 256));
    EXPECT_FALSE(raw::fusionRangesOverlap(texture.va, raw::fusionImageBytes(texture), texture.va + 16384, 256));
    texture.layerBytes = 32768;
    texture.layers = 3;
    EXPECT_EQ(raw::fusionImageBytes(texture), 98304u);
    Image::Chain chain;
    chain.layout.bytes = 131072;
    texture.chain = &chain;
    EXPECT_EQ(raw::fusionImageBytes(texture), 131072u);
    texture.span.bytes = 262144;
    EXPECT_EQ(raw::fusionImageBytes(texture), 262144u);
    texture.layerBytes = UINT64_MAX;
    EXPECT_EQ(raw::fusionImageBytes(texture), UINT64_MAX);
}

TEST(AttachmentLifetime, OnlyACompleteInitialClearEliminatesTheShadowInputCopy)
{
    struct Clear {
        uint32_t target, x0, y0, x1, y1;
    };

    std::vector<Clear> clears;
    EXPECT_FALSE(raw::attachmentStartsCleared(0, 64, 64, clears));
    clears.push_back({1, 0, 0, 64, 64});
    EXPECT_FALSE(raw::attachmentStartsCleared(0, 64, 64, clears));
    clears.push_back({0, 0, 0, 64, 32});
    EXPECT_FALSE(raw::attachmentStartsCleared(0, 64, 64, clears));
    clears.push_back({0, 0, 32, 64, 64});
    EXPECT_FALSE(raw::attachmentStartsCleared(0, 64, 64, clears));
    clears.push_back({0, 0, 0, 64, 64});
    EXPECT_TRUE(raw::attachmentStartsCleared(0, 64, 64, clears));
    EXPECT_TRUE(raw::attachmentStartsCleared(0, 31, 29, clears));
    EXPECT_FALSE(raw::attachmentStartsCleared(0, 65, 64, clears));
}
}  // namespace
