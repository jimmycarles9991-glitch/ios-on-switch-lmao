// SPDX-License-Identifier: MIT
// The device's answers to Metal's capability and identity queries, per kind: the target's identity (iPad7,1's A10X on
// 21H461, iPad12,1's A13 on 24B119: drivers/NativeMetal/bridge/NativeMetalTarget.rs) bounded by what GM20B takes, as m2v's
// device answers them today, but where the device has a form of a feature m2v's answers lack: counter sampling at stage
// boundaries, as the identity's AGX driver answers it (raw_counters.h). maxPredicatedNestingDepth is the loop depth the
// raw path lays down (raw_loop_copies.h LoopCopies::kMaxDepth). Vertex amplification and dynamic attribute stride are
// the target's: the A13's supportsVertexAmplificationCount: takes 0 to 8 (AGXA13FamilyDevice, `cmp w2,#9; cset lo`),
// its limits table's maxVertexAmplificationCount 2 and maxVertexAmplificationFactor 8, supportsDynamicAttributeStride
// supportsFamily:Apple4; the A10X takes a count of 0 or 1 (`tst x2,#~1`) and no dynamic stride. Tessellation is the
// target's: _MTLDevice's supportsTessellation is supportsFamily:Apple3 and supportsIndirectTessellation
// supportsFamily:Apple5, maxTessellationFactor its limits table's 16 (A10X) or 64 (A13), as the Metal Feature Set
// Tables list them. raw/test/selcover holds every other capability equal to the m2v device's on the host, for the kind
// its bridge was built for.
// An argument answer holds `value` for every argument in [first, last] and 0 elsewhere.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include "MetalABI.h"
#include "SwitchGM20BCaps.h"

namespace raw::answers {

// A feature row of kits/SwitchGM20B/caps/features.tsv as a Metal flag: 1 where the hardware has it and a device run proved
// it (grade chip), so the answer moves with the row
constexpr uint64_t chipFeature(switch_gm20b_feature f)
{
    return switch_gm20b_features[f].present && switch_gm20b_features[f].grade == SWITCH_GM20B_GRADE_CHIP;
}

struct Count {
    const char *selector;
    uint64_t value;
    bool flag;  // a BOOL query, else an NSUInteger one
};

struct ArgRange {
    const char *selector;
    bool flag;
    uint64_t first, last, value;
};

struct Kind {
    std::string_view kind;
    const Count *counts;
    size_t countCount;
    const ArgRange *args;
    size_t argCount;
    // The identity's Apple GPU subtype, that of the AGX slices the IPSW ships: MTLArchitecture names it (Metal's
    // newArchitectureNameString: 0x61 applegpu_g9g, 0xd2 applegpu_g12p), and frameworks open their per-GPU archives by
    // that name (CMImaging's -[FigMetalContext _identifyGPU]: <bundle>/binaryArchive.g12p on 24B119)
    uint32_t gpuSubtype;
    // Its AGX driver's vendorName, familyName and productName (MPSCore matches the joined name, "Apple A13 GPU")
    std::string_view gpuVendor, gpuFamily, gpuProduct;
    // the target's Metal driver (NativeMetalTarget.rs), the domain its errors take: a creation's binary archive miss is code 3
    // in it (an M4 answers AGXMetalG16X)
    const char *driver;
    // a render pass of several layers (renderTargetArrayLength): the A13 (Apple6) renders one, the A10X (Apple3) has
    // no layered rendering in Apple's feature tables
    bool layeredRendering;
};

inline constexpr Count kCounts21H461[] = {
    {"gpuCoreCount", 2, false},
    {"maxVertexAmplificationCount", 1, false},
    {"deviceLinearTextureAlignmentBytes", 32, false},
    {"linearTextureAlignmentBytes", 32, false},
    {"iosurfaceTextureAlignmentBytes", 32, false},
    {"iosurfaceReadOnlyTextureAlignmentBytes", 16, false},
    {"maxVertexBuffers", 31, false},
    {"maxFragmentBuffers", 31, false},
    {"maxComputeBuffers", 31, false},
    {"maxTileBuffers", 0, false},
    {"maxVertexTextures", 31, false},
    {"maxFragmentTextures", 31, false},
    {"maxComputeTextures", 31, false},
    {"maxTileTextures", 0, false},
    {"maxVertexSamplers", 16, false},
    {"maxFragmentSamplers", 16, false},
    {"maxComputeSamplers", 16, false},
    {"maxTileSamplers", 0, false},
    {"maxComputeLocalMemorySizes", 31, false},
    {"maxComputeThreadgroupMemory", 16384, false},
    {"maxComputeThreadgroupMemoryAlignmentBytes", 16, false},
    {"maxPredicatedNestingDepth", 8, false},
    {"maxRasterizationRateLayerCount", 2, false},
    {"maxTextureWidth2D", 16384, false},
    {"maxTextureHeight2D", 16384, false},
    {"maxTextureLayers", 2048, false},
    {"maxTotalComputeThreadsPerThreadgroup", 1024, false},
    {"maxViewportCount", 1, false},
    {"isFramebufferReadSupported", 0, true},
    {"isLargeMRTSupported", 0, true},
    {"isRGB10A2GammaSupported", 0, true},
    {"supportsBufferWithAddressRanges", 0, true},
    {"supportsBufferWithIOSurface", 0, true},
    {"supportsCommandBufferJump", 1, true},
    {"supportsComputeCompressedTextureWrite", 0, true},
    {"supportsConditionalLoadStore", 0, true},
    {"supportsDynamicAttributeStride", 0, true},
    {"supportsExtendedXR10Formats", 1, true},
    {"supportsFloat16BCubicFiltering", 1, true},
    {"supportsHeapWithAddressRanges", 0, true},
    {"supportsImageBlocks", 0, true},
    {"supportsMemoryOrderAtomics", 1, true},
    {"supportsNativeHardwareFP16", 0, true},
    {"supportsNonSquareTileShaders", 0, true},
    {"supportsProgrammableSamplePositions", 1, true},
    {"supportsResourceDetachBacking", 1, true},
    {"supportsSharedTextureHandles", 1, true},
    {"supportsSIMDReduction", 0, true},
    {"supportsSIMDShuffleAndFill", 0, true},
    {"supportsTexture2DMultisampleArray", 0, true},
    {"supportsTileShaders", 0, true},
    {"maxThreadgroupMemoryLength", 16384, false},
    {"argumentBuffersSupport", 0, false},
    {"readWriteTextureSupport", 2, false},
    {"maxColorAttachments", 8, false},
    {"minConstantBufferAlignmentBytes", 4, false},
    {"deviceLinearReadOnlyTextureAlignmentBytes", 32, false},
    {"areRasterOrderGroupsSupported", 0, true},
    {"supportsRasterOrderGroups", 0, true},
    {"isDepth24Stencil8PixelFormatSupported", 0, true},
    {"areProgrammableSamplePositionsSupported", 1, true},
    {"supports32BitFloatFiltering", chipFeature(SWITCH_GM20B_FEATURE_3D_H9), true},
    {"supportsMemorylessRenderTargets", 1, true},
    {"supportsSamplerReductionMode", 0, true},
    {"supportsYCBCRFormats12", 0, true},
    {"supportsYCBCRPackedFormats12", 0, true},
    {"supportsSIMDGroup", 0, true},
    {"maxFramebufferStorageBits", 256, false},
    {"maxVertexAttributes", 31, false},
    {"maxVertexInlineDataSize", 4096, false},
    {"maxFragmentInlineDataSize", 4096, false},
    {"maxComputeInlineDataSize", 4096, false},
    {"maxInterpolants", 60, false},
    {"maxInterpolatedComponents", 60, false},
    {"maxVisibilityQueryOffset", 65528, false},
    {"maxTextureWidth1D", 16384, false},
    {"maxTextureWidth3D", 2048, false},
    {"maxTextureHeight3D", 2048, false},
    {"maxTextureDepth3D", 2048, false},
    {"maxTextureDimensionCube", 16384, false},
    {"maxTextureBufferWidth", 134217728, false},
    {"maxFunctionConstantIndices", 65536, false},
    {"maxIndirectBuffers", 31, false},
    {"maxIndirectTextures", 31, false},
    {"maxIndirectSamplers", 16, false},
    {"maxIndirectSamplersPerDevice", 96, false},
    {"maxFenceInstances", 32768, false},
    {"maxCustomSamplePositions", 4, false},
    {"maxVertexAmplificationFactor", 1, false},
    {"maxComputeAttributes", 31, false},
    {"maxConstantBufferArguments", 31, false},
    {"supportsTessellation", 1, true},
    {"supportsIndirectTessellation", 0, true},
    {"maxTessellationFactor", 16, false},
};

inline constexpr ArgRange kArgs21H461[] = {
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 1, 1, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 10, 14, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 20, 20, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 22, 26, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 30, 34, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 40, 44, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 53, 55, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 60, 60, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 62, 66, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 70, 74, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 80, 81, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 84, 85, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 90, 94, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 103, 105, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 110, 110, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 112, 116, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 123, 125, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 263, 263, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 300, 304, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 313, 315, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 323, 325, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 500, 510, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 520, 530, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 540, 548, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 550, 556, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 560, 560, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 562, 613, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 615, 645, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 651, 652, 16},
    {"supportsTextureSampleCount:", true, 1, 2, 1},
    {"supportsTextureSampleCount:", true, 4, 4, 1},
    {"supportsVertexAmplificationCount:", true, 0, 1, 1},
    {"supportsRasterizationRateMapWithLayerCount:", true, 1, 2, 1},
    {"supportsCounterSampling:", true, 0, 0, 1},
    {"supportsFamily:", true, 1001, 1003, 1},
    {"supportsFamily:", true, 3001, 3002, 1},
    {"supportsPrimitiveType:", true, 0, 4, 1},
    {"supportsTextureWriteRoundingMode:", true, 0, 2, 1},
};

inline constexpr Count kCounts24B119[] = {
    {"gpuCoreCount", 2, false},
    {"maxVertexAmplificationCount", 2, false},
    {"deviceLinearTextureAlignmentBytes", 32, false},
    {"linearTextureAlignmentBytes", 32, false},
    {"iosurfaceTextureAlignmentBytes", 32, false},
    {"iosurfaceReadOnlyTextureAlignmentBytes", 16, false},
    {"maxVertexBuffers", 31, false},
    {"maxFragmentBuffers", 31, false},
    {"maxComputeBuffers", 31, false},
    {"maxTileBuffers", 0, false},
    {"maxVertexTextures", 128, false},
    {"maxFragmentTextures", 128, false},
    {"maxComputeTextures", 128, false},
    {"maxTileTextures", 0, false},
    {"maxVertexSamplers", 16, false},
    {"maxFragmentSamplers", 16, false},
    {"maxComputeSamplers", 16, false},
    {"maxTileSamplers", 0, false},
    {"maxComputeLocalMemorySizes", 31, false},
    {"maxComputeThreadgroupMemory", 32768, false},
    {"maxComputeThreadgroupMemoryAlignmentBytes", 16, false},
    {"maxPredicatedNestingDepth", 8, false},
    {"maxRasterizationRateLayerCount", 2, false},
    {"maxTextureWidth2D", 16384, false},
    {"maxTextureHeight2D", 16384, false},
    {"maxTextureLayers", 2048, false},
    {"maxTotalComputeThreadsPerThreadgroup", 1024, false},
    {"maxViewportCount", 16, false},
    {"isFramebufferReadSupported", 0, true},
    {"isLargeMRTSupported", 0, true},
    {"isRGB10A2GammaSupported", 0, true},
    {"supportsBufferWithAddressRanges", 0, true},
    {"supportsBufferWithIOSurface", 0, true},
    {"supportsCommandBufferJump", 1, true},
    {"supportsComputeCompressedTextureWrite", 0, true},
    {"supportsConditionalLoadStore", 0, true},
    {"supportsDynamicAttributeStride", 1, true},
    {"supportsExtendedXR10Formats", 1, true},
    {"supportsFloat16BCubicFiltering", 1, true},
    {"supportsHeapWithAddressRanges", 0, true},
    {"supportsImageBlocks", 0, true},
    {"supportsMemoryOrderAtomics", 1, true},
    {"supportsNativeHardwareFP16", 0, true},
    {"supportsNonSquareTileShaders", 0, true},
    {"supportsProgrammableSamplePositions", 1, true},
    {"supportsResourceDetachBacking", 1, true},
    {"supportsSharedTextureHandles", 1, true},
    {"supportsSIMDReduction", 0, true},
    {"supportsSIMDShuffleAndFill", 0, true},
    {"supportsTexture2DMultisampleArray", 0, true},
    {"supportsTileShaders", 0, true},
    {"maxThreadgroupMemoryLength", 32768, false},
    {"argumentBuffersSupport", 1, false},
    {"readWriteTextureSupport", 2, false},
    {"maxColorAttachments", 8, false},
    {"minConstantBufferAlignmentBytes", 4, false},
    {"deviceLinearReadOnlyTextureAlignmentBytes", 32, false},
    {"areRasterOrderGroupsSupported", 1, true},
    {"supportsRasterOrderGroups", 1, true},
    {"isDepth24Stencil8PixelFormatSupported", 0, true},
    {"areProgrammableSamplePositionsSupported", 1, true},
    {"supports32BitFloatFiltering", chipFeature(SWITCH_GM20B_FEATURE_3D_H9), true},
    {"supportsMemorylessRenderTargets", 1, true},
    {"supportsSamplerReductionMode", 0, true},
    {"supportsYCBCRFormats12", 0, true},
    {"supportsYCBCRPackedFormats12", 0, true},
    {"supportsSIMDGroup", 1, true},
    {"maxFramebufferStorageBits", 512, false},
    {"maxVertexAttributes", 31, false},
    {"maxVertexInlineDataSize", 4096, false},
    {"maxFragmentInlineDataSize", 4096, false},
    {"maxComputeInlineDataSize", 4096, false},
    {"maxInterpolants", 124, false},
    {"maxInterpolatedComponents", 124, false},
    {"maxVisibilityQueryOffset", 65528, false},
    {"maxTextureWidth1D", 16384, false},
    {"maxTextureWidth3D", 2048, false},
    {"maxTextureHeight3D", 2048, false},
    {"maxTextureDepth3D", 2048, false},
    {"maxTextureDimensionCube", 16384, false},
    {"maxTextureBufferWidth", 134217728, false},
    {"maxFunctionConstantIndices", 65536, false},
    {"maxIndirectBuffers", 500000, false},
    {"maxIndirectTextures", 500000, false},
    {"maxIndirectSamplers", 500000, false},
    {"maxIndirectSamplersPerDevice", 96, false},
    {"maxFenceInstances", 32768, false},
    {"maxCustomSamplePositions", 4, false},
    {"maxVertexAmplificationFactor", 8, false},
    {"maxComputeAttributes", 31, false},
    {"maxConstantBufferArguments", 31, false},
    {"supportsTessellation", 1, true},
    {"supportsIndirectTessellation", 1, true},
    {"maxTessellationFactor", 64, false},
};

inline constexpr ArgRange kArgs24B119[] = {
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 1, 1, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 10, 14, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 20, 20, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 22, 26, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 30, 34, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 40, 44, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 53, 55, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 60, 60, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 62, 66, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 70, 74, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 80, 81, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 84, 85, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 90, 94, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 103, 105, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 110, 110, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 112, 116, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 123, 125, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 263, 263, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 300, 304, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 313, 315, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 323, 325, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 500, 510, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 520, 530, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 540, 548, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 550, 556, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 560, 560, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 562, 613, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 615, 645, 16},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 651, 652, 16},
    {"supportsTextureSampleCount:", true, 1, 2, 1},
    {"supportsTextureSampleCount:", true, 4, 4, 1},
    {"supportsVertexAmplificationCount:", true, 0, 8, 1},
    {"supportsRasterizationRateMapWithLayerCount:", true, 1, 2, 1},
    {"supportsCounterSampling:", true, 0, 0, 1},
    {"supportsFamily:", true, 1001, 1003, 1},
    {"supportsFamily:", true, 3001, 3003, 1},
    {"supportsPrimitiveType:", true, 0, 4, 1},
    {"supportsTextureWriteRoundingMode:", true, 0, 2, 1},
};

inline constexpr Kind kKinds[] = {
    {"21H461", kCounts21H461, std::size(kCounts21H461), kArgs21H461, std::size(kArgs21H461), 0x61, "Apple", "A10X",
     "GPU", "AGXMetalA10", false},
    {"24B119", kCounts24B119, std::size(kCounts24B119), kArgs24B119, std::size(kArgs24B119), 0xd2, "Apple", "A13",
     "GPU", "AGXMetalA13", true},
};

// The kind this build is for (MetalABI.h): its table's index, or the table count when it has none
inline constexpr size_t kindIndex()
{
    for (size_t i = 0; i < std::size(kKinds); i++)
        if (kKinds[i].kind == SWITCH_METAL_KIND)
            return i;
    return std::size(kKinds);
}

static_assert(kindIndex() < std::size(kKinds), "no device answers for this kind (raw_device_answers.h)");
inline constexpr const Kind &kThisKind = kKinds[kindIndex()];

// The largest argument this kind answers argument query `selector` YES for (0: none)
constexpr uint64_t thisKindLastYes(std::string_view selector)
{
    uint64_t last = 0;
    for (size_t i = 0; i < kThisKind.argCount; i++)
        if (kThisKind.args[i].selector == selector && kThisKind.args[i].value && kThisKind.args[i].last > last)
            last = kThisKind.args[i].last;
    return last;
}

// This kind's answer to count or flag query `selector` (0: none)
constexpr uint64_t thisKindCount(std::string_view selector)
{
    for (size_t i = 0; i < kThisKind.countCount; i++)
        if (kThisKind.counts[i].selector == selector)
            return kThisKind.counts[i].value;
    return 0;
}

}  // namespace raw::answers
