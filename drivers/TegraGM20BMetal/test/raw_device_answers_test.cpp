// SPDX-License-Identifier: MIT
// raw/runtime/raw_device_answers.h, every kind: a feature of the target's is the target's answer, and the answers agree
// with each other (RawMetalUnsupported.mm refuses the creations past them); counter sampling is answered as the identity's
// driver answers it.
#include "../raw/runtime/raw_device_answers.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

namespace {

std::optional<uint64_t> count(const raw::answers::Kind &k, const char *selector)
{
    for (size_t i = 0; i < k.countCount; i++)
        if (!strcmp(k.counts[i].selector, selector))
            return k.counts[i].value;
    return std::nullopt;
}

uint64_t argument(const raw::answers::Kind &k, const char *selector, uint64_t a)
{
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, selector) && a >= k.args[i].first && a <= k.args[i].last)
            return k.args[i].value;
    return 0;
}

// The target's: 24B119's A13 takes counts 0 to 8 (AGXA13FamilyDevice supportsVertexAmplificationCount:) and its limits
// table says 2 and 8; 21H461's A10X takes 0 and 1
TEST(RawDeviceAnswers, AmplificationIsTheTargetsAndEveryQuerySaysSo)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        const bool a13 = k.kind == "24B119";
        EXPECT_EQ(count(k, "maxVertexAmplificationCount"), a13 ? 2u : 1u) << k.kind;
        EXPECT_EQ(count(k, "maxVertexAmplificationFactor"), a13 ? 8u : 1u) << k.kind;
        EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", 1), 1u) << k.kind;
        for (uint64_t n = 2; n <= 8; n++)
            EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", n), a13 ? 1u : 0u) << k.kind << " count " << n;
        EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", 9), 0u) << k.kind;
    }
}

// supportsDynamicAttributeStride is supportsFamily:Apple4, supportsTessellation Apple3, supportsIndirectTessellation Apple5
TEST(RawDeviceAnswers, TargetFeaturesFollowTheirFamilies)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        const bool a13 = k.kind == "24B119";
        EXPECT_EQ(count(k, "supportsDynamicAttributeStride"), a13 ? 1u : 0u) << k.kind;
        EXPECT_EQ(count(k, "supportsTessellation"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsIndirectTessellation"), a13 ? 1u : 0u) << k.kind;
        EXPECT_EQ(count(k, "maxTessellationFactor"), a13 ? 64u : 16u) << k.kind;
    }
}

// The identity's AGX driver samples counters at stage boundaries (MTLCounterSamplingPointAtStageBoundary, 0) alone
TEST(RawDeviceAnswers, CountersAreSampledAtStageBoundariesAlone)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(argument(k, "supportsCounterSampling:", 0), 1u) << k.kind;
        for (uint64_t point = 1; point <= 4; point++)
            EXPECT_EQ(argument(k, "supportsCounterSampling:", point), 0u) << k.kind << " point " << point;
    }
}

// The loop depth the raw path lays down (raw_loop_copies.h LoopCopies::kMaxDepth); Ifs nest freely within it
TEST(RawDeviceAnswers, PredicationNestsAsDeepAsTheLoopsLaidDown)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "maxPredicatedNestingDepth"), 8u) << k.kind;
}

// Apple1's, which both identities' AGX drivers answer (RawMetalSharedTextures.mm makes them)
TEST(RawDeviceAnswers, SharedTextureHandlesAreYes)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "supportsSharedTextureHandles"), 1u) << k.kind;
}

}  // namespace
