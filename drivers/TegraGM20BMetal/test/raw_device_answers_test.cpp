// SPDX-License-Identifier: MIT
// raw/runtime/raw_device_answers.h, every kind: the counts GM20B has (kits/SwitchGM20B caps) where raw draws them, the
// identity's elsewhere; counter sampling is answered as the identity's driver answers it.
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

// The amplify macro keeps SWITCH_GM20B_AMP_MAX mapping words, and both kinds take every count up to it; 24B119's
// maxVertexAmplificationCount stays its A13's limits table's 2
TEST(RawDeviceAnswers, AmplificationIsWhatTheAmplifyMacroDraws)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        const bool a13 = k.kind == "24B119";
        EXPECT_EQ(count(k, "maxVertexAmplificationCount"), a13 ? 2u : uint64_t(SWITCH_GM20B_AMP_MAX)) << k.kind;
        EXPECT_EQ(count(k, "maxVertexAmplificationFactor"), uint64_t(SWITCH_GM20B_AMP_MAX)) << k.kind;
        for (uint64_t n = 1; n <= SWITCH_GM20B_AMP_MAX; n++)
            EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", n), 1u) << k.kind << " count " << n;
        EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", SWITCH_GM20B_AMP_MAX + 1), 0u) << k.kind;
    }
}

// Raw draws factor 64, indirect patches and dynamic strides on both kinds
TEST(RawDeviceAnswers, TessellationAndDynamicStrideAreGm20bs)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "supportsDynamicAttributeStride"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsTessellation"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsIndirectTessellation"), 1u) << k.kind;
        EXPECT_EQ(count(k, "maxTessellationFactor"), uint64_t(SWITCH_GM20B_LIMIT_TESSELLATION_LEVEL)) << k.kind;
        EXPECT_EQ(count(k, "maxTessellationFactor"), 64u) << k.kind;
    }
}

// 21H461's base class answers supportsLayeredRendering from feature tables the raw device leaves empty
TEST(RawDeviceAnswers, LayeredRenderingIsYesOn21H461)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        if (k.kind == "21H461")
            EXPECT_EQ(count(k, "supportsLayeredRendering"), 1u);
}

TEST(RawDeviceAnswers, ViewportsAreGm20bs)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "maxViewportCount"), uint64_t(SWITCH_GM20B_LIMIT_VIEWPORTS)) << k.kind;
}

// Eight targets of RGBA32Float, the widest colour GM20B renders
TEST(RawDeviceAnswers, FramebufferBitsAreEveryTargetAtTheWidestFormat)
{
    EXPECT_EQ(raw::answers::widestTargetBits(), 128u);
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "maxFramebufferStorageBits"), 1024u) << k.kind;
}

// GM20B's 32 generic vec4 inputs on 21H461; 124 is Metal 27's own ceiling, which 24B119 keeps
TEST(RawDeviceAnswers, InterpolantsAreGm20bsWhereMetalTakesThem)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        const uint64_t want = k.kind == "24B119" ? 124u : uint64_t(SWITCH_GM20B_LIMIT_INTERPOLATED_COMPONENTS);
        EXPECT_EQ(count(k, "maxInterpolants"), want) << k.kind;
        EXPECT_EQ(count(k, "maxInterpolatedComponents"), want) << k.kind;
    }
    EXPECT_EQ(SWITCH_GM20B_LIMIT_INTERPOLATED_COMPONENTS, 128u);
}

// The QMD's 48 KiB L1 configuration (SwitchGM20BQmd.h)
TEST(RawDeviceAnswers, ThreadgroupMemoryIsACtasWhole48KiB)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "maxThreadgroupMemoryLength"), 49152u) << k.kind;
        EXPECT_EQ(count(k, "maxComputeThreadgroupMemory"), 49152u) << k.kind;
    }
}

// Tier 2 on both kinds: IDs read through the device's handle tables (raw_heap_tables.h), 500000 of each kind of
// resource an argument buffer holds, as 24B119's A13 answers; distinct live textures stay bounded by the texture
// header pool (Device kTicCount), as every texture's are
TEST(RawDeviceAnswers, ArgumentBuffersAreTier2)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "argumentBuffersSupport"), 1u) << k.kind;
        for (const char *s : {"maxIndirectBuffers", "maxIndirectTextures", "maxIndirectSamplers"})
            EXPECT_EQ(count(k, s), 500000u) << k.kind << " " << s;
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

// RawMetalDevice.mm newBufferWithIOSurface: makes the buffer over the surface's pages (gpu.raw.surface_buffer)
TEST(RawDeviceAnswers, BuffersOverIOSurfacesAreMade)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "supportsBufferWithIOSurface"), 1u) << k.kind;
}

// The translator lowers SIMD-group permutes, votes, reductions, prefix sums and shuffle-and-fill to SHFL and VOTE
// (gpu.compute.simd_group)
TEST(RawDeviceAnswers, SimdGroupRowsAreYes)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "supportsSIMDReduction"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsSIMDShuffleAndFill"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsSIMDGroup"), 1u) << k.kind;
    }
}

// The pixel interlock the storage form draws in orders a raster order group's accesses (gpu.raw.raster_order)
TEST(RawDeviceAnswers, RasterOrderGroupsAreYes)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "areRasterOrderGroupsSupported"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsRasterOrderGroups"), 1u) << k.kind;
    }
}

// GM20B (SM53) has HADD2, HMUL2 and HFMA2 (gpu.compute.half_alu)
TEST(RawDeviceAnswers, HalfArithmeticIsNative)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "supportsNativeHardwareFP16"), 1u) << k.kind;
}

// Every Apple family reads its colour attachments in the shader; the raw path does it inside the pixel interlock
// (gpu.raw.framebuffer_fetch)
TEST(RawDeviceAnswers, FramebufferReadsAreYes)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "isFramebufferReadSupported"), 1u) << k.kind;
        EXPECT_EQ(count(k, "supportsProgrammableBlending"), 1u) << k.kind;
    }
}

// A linear texture's rows and a texture buffer's start take the pitch texture header's 32 bytes (RawMetalDevice.mm,
// RawMetalDeviceAnswers.mm answer both from this row)
TEST(RawDeviceAnswers, LinearAlignmentIsThePitchHeaders)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        for (uint64_t format : {10, 70, 80, 115, 125})
            EXPECT_EQ(argument(k, "minimumLinearTextureAlignmentForPixelFormat:", format), 32u)
                << k.kind << " format " << format;
}

// Apple1's, which both identities' AGX drivers answer (RawMetalSharedTextures.mm makes them)
TEST(RawDeviceAnswers, SharedTextureHandlesAreYes)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(count(k, "supportsSharedTextureHandles"), 1u) << k.kind;
}

}  // namespace
