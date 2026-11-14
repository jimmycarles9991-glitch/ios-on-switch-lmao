// SPDX-License-Identifier: MIT
// The device's answers to Metal's capability and identity queries, per kind: the target's identity (iPad7,1's A10X on
// 21H461, iPad12,1's A13 on 24B119: drivers/NativeMetal/bridge/NativeMetalTarget.rs) bounded by what GM20B takes, as m2v's
// device answers them today, but where the device has a form of a feature m2v's answers lack: counter sampling at stage
// boundaries, as the identity's AGX driver answers it (raw_counters.h); buffers over IOSurfaces (RawMetalDevice.mm
// newBufferWithIOSurface:); SIMD-group permute, reduction and shift-and-fill, which the translator lowers to SHFL and
// VOTE (HW Test gpu.compute.simd_group); raster order groups on both kinds, through the pixel interlock the storage
// form draws in (gpu.raw.raster_order); framebuffer reads (programmable blending), the storage form's fetch inside the
// interlock (gpu.raw.framebuffer_fetch); native half arithmetic, SM53's HADD2, HMUL2 and HFMA2 (gpu.compute.half_alu).
// maxPredicatedNestingDepth is the loop depth the raw path lays down
// (raw_loop_copies.h LoopCopies::kMaxDepth). Tessellation is _MTLDevice's supportsTessellation (supportsFamily:Apple3).
// Above the identity, on purpose (NativeMetalTarget.rs): the counts GM20B has (kits/SwitchGM20B caps), each answered
// where raw draws it on both kinds: layered passes, 16 viewports, tessellation factor 64, indirect patches, dynamic
// attribute stride, the amplification the amplify macro keeps (SWITCH_GM20B_AMP_MAX), 48 KiB of threadgroup memory,
// eight targets of the widest colour a pixel, tier-2 argument buffers through the handle tables (raw_heap_tables.h),
// and 128 interpolated components on 21H461 (24B119 keeps Metal 27's ceiling of 124).
// An argument answer holds `value` for every argument in [first, last] and 0 elsewhere.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h"
#include "MetalABI.h"
#include "SwitchGM20BCaps.h"

namespace raw::answers {

// A feature row of kits/SwitchGM20B/caps/features.tsv as a Metal flag: 1 where the hardware has it and a device run proved
// it (grade chip), so the answer moves with the row
constexpr uint64_t chipFeature(switch_gm20b_feature f)
{
    return switch_gm20b_features[f].present && switch_gm20b_features[f].grade == SWITCH_GM20B_GRADE_CHIP;
}

// The bits of a pixel in its widest colour target: the largest texel of a format the hardware renders
constexpr uint64_t widestTargetBits()
{
    uint64_t widest = 0;
    for (const switch_gm20b_format &f : switch_gm20b_formats)
        if ((f.flags & SWITCH_GM20B_FORMAT_RENDER) && f.block_w == 1 && f.block_h == 1 && 8u * f.bytes > widest)
            widest = 8u * f.bytes;
    return widest;
}

// An immediate-mode GPU keeps no tile: every colour target may hold the widest format
constexpr uint64_t kFramebufferBits = SWITCH_GM20B_LIMIT_COLOR_TARGETS * widestTargetBits();

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
};

inline constexpr Count kCounts21H461[] = {
    {"gpuCoreCount", 2, false},
    {"maxVertexAmplificationCount", SWITCH_GM20B_AMP_MAX, false},
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
    {"maxComputeThreadgroupMemory", SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES, false},
    {"maxComputeThreadgroupMemoryAlignmentBytes", 16, false},
    {"maxPredicatedNestingDepth", 8, false},
    {"maxRasterizationRateLayerCount", 2, false},
    {"maxTextureWidth2D", 16384, false},
    {"maxTextureHeight2D", 16384, false},
    {"maxTextureLayers", 2048, false},
    {"maxTotalComputeThreadsPerThreadgroup", 1024, false},
    {"maxViewportCount", SWITCH_GM20B_LIMIT_VIEWPORTS, false},
    {"isFramebufferReadSupported", 1, true},
    {"supportsProgrammableBlending", 1, true},
    {"isLargeMRTSupported", 0, true},
    {"isRGB10A2GammaSupported", 0, true},
    {"supportsBufferWithAddressRanges", 0, true},
    {"supportsBufferWithIOSurface", 1, true},
    {"supportsCommandBufferJump", 1, true},
    {"supportsComputeCompressedTextureWrite", 0, true},
    {"supportsConditionalLoadStore", 0, true},
    {"supportsDynamicAttributeStride", 1, true},
    {"supportsExtendedXR10Formats", 1, true},
    {"supportsFloat16BCubicFiltering", 1, true},
    {"supportsHeapWithAddressRanges", 0, true},
    {"supportsImageBlocks", 0, true},
    {"supportsMemoryOrderAtomics", 1, true},
    {"supportsNativeHardwareFP16", 1, true},
    {"supportsNonSquareTileShaders", 0, true},
    {"supportsProgrammableSamplePositions", 1, true},
    {"supportsResourceDetachBacking", 1, true},
    {"supportsSharedTextureHandles", 1, true},
    {"supportsSIMDReduction", 1, true},
    {"supportsSIMDShuffleAndFill", 1, true},
    {"supportsTexture2DMultisampleArray", 0, true},
    {"supportsTileShaders", 0, true},
    {"maxThreadgroupMemoryLength", SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES, false},
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
    {"maxFramebufferStorageBits", kFramebufferBits, false},
    {"maxVertexAttributes", 31, false},
    {"maxVertexInlineDataSize", 4096, false},
    {"maxFragmentInlineDataSize", 4096, false},
    {"maxComputeInlineDataSize", 4096, false},
    {"maxInterpolants", SWITCH_GM20B_LIMIT_INTERPOLATED_COMPONENTS, false},
    {"maxInterpolatedComponents", SWITCH_GM20B_LIMIT_INTERPOLATED_COMPONENTS, false},
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
    {"maxVertexAmplificationFactor", SWITCH_GM20B_AMP_MAX, false},
    {"maxComputeAttributes", 31, false},
    {"maxConstantBufferArguments", 31, false},
    {"supportsTessellation", 1, true},
    {"supportsLayeredRendering", 1, true},
    {"supportsIndirectTessellation", 1, true},
    {"maxTessellationFactor", SWITCH_GM20B_LIMIT_TESSELLATION_LEVEL, false},
};

inline constexpr ArgRange kArgs21H461[] = {
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 1, 1, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 10, 14, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 20, 20, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 22, 26, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 30, 34, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 40, 44, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 53, 55, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 60, 60, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 62, 66, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 70, 74, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 80, 81, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 84, 85, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 90, 94, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 103, 105, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 110, 110, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 112, 116, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 123, 125, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 263, 263, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 300, 304, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 313, 315, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 323, 325, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 500, 510, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 520, 530, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 540, 548, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 550, 556, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 560, 560, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 562, 613, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 615, 645, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 651, 652, 32},
    {"supportsTextureSampleCount:", true, 1, 2, 1},
    {"supportsTextureSampleCount:", true, 4, 4, 1},
    {"supportsVertexAmplificationCount:", true, 0, SWITCH_GM20B_AMP_MAX, 1},
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
    {"maxComputeThreadgroupMemory", SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES, false},
    {"maxComputeThreadgroupMemoryAlignmentBytes", 16, false},
    {"maxPredicatedNestingDepth", 8, false},
    {"maxRasterizationRateLayerCount", 2, false},
    {"maxTextureWidth2D", 16384, false},
    {"maxTextureHeight2D", 16384, false},
    {"maxTextureLayers", 2048, false},
    {"maxTotalComputeThreadsPerThreadgroup", 1024, false},
    {"maxViewportCount", SWITCH_GM20B_LIMIT_VIEWPORTS, false},
    {"isFramebufferReadSupported", 1, true},
    {"supportsProgrammableBlending", 1, true},
    {"isLargeMRTSupported", 0, true},
    {"isRGB10A2GammaSupported", 0, true},
    {"supportsBufferWithAddressRanges", 0, true},
    {"supportsBufferWithIOSurface", 1, true},
    {"supportsCommandBufferJump", 1, true},
    {"supportsComputeCompressedTextureWrite", 0, true},
    {"supportsConditionalLoadStore", 0, true},
    {"supportsDynamicAttributeStride", 1, true},
    {"supportsExtendedXR10Formats", 1, true},
    {"supportsFloat16BCubicFiltering", 1, true},
    {"supportsHeapWithAddressRanges", 0, true},
    {"supportsImageBlocks", 0, true},
    {"supportsMemoryOrderAtomics", 1, true},
    {"supportsNativeHardwareFP16", 1, true},
    {"supportsNonSquareTileShaders", 0, true},
    {"supportsProgrammableSamplePositions", 1, true},
    {"supportsResourceDetachBacking", 1, true},
    {"supportsSharedTextureHandles", 1, true},
    {"supportsSIMDReduction", 1, true},
    {"supportsSIMDShuffleAndFill", 1, true},
    {"supportsTexture2DMultisampleArray", 0, true},
    {"supportsTileShaders", 0, true},
    {"maxThreadgroupMemoryLength", SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES, false},
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
    {"maxFramebufferStorageBits", kFramebufferBits, false},
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
    {"maxVertexAmplificationFactor", SWITCH_GM20B_AMP_MAX, false},
    {"maxComputeAttributes", 31, false},
    {"maxConstantBufferArguments", 31, false},
    {"supportsTessellation", 1, true},
    {"supportsIndirectTessellation", 1, true},
    {"maxTessellationFactor", SWITCH_GM20B_LIMIT_TESSELLATION_LEVEL, false},
};

inline constexpr ArgRange kArgs24B119[] = {
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 1, 1, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 10, 14, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 20, 20, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 22, 26, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 30, 34, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 40, 44, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 53, 55, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 60, 60, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 62, 66, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 70, 74, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 80, 81, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 84, 85, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 90, 94, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 103, 105, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 110, 110, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 112, 116, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 123, 125, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 263, 263, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 300, 304, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 313, 315, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 323, 325, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 500, 510, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 520, 530, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 540, 548, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 550, 556, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 560, 560, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 562, 613, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 615, 645, 32},
    {"minimumLinearTextureAlignmentForPixelFormat:", false, 651, 652, 32},
    {"supportsTextureSampleCount:", true, 1, 2, 1},
    {"supportsTextureSampleCount:", true, 4, 4, 1},
    {"supportsVertexAmplificationCount:", true, 0, SWITCH_GM20B_AMP_MAX, 1},
    {"supportsRasterizationRateMapWithLayerCount:", true, 1, 2, 1},
    {"supportsCounterSampling:", true, 0, 0, 1},
    {"supportsFamily:", true, 1001, 1003, 1},
    {"supportsFamily:", true, 3001, 3003, 1},
    {"supportsPrimitiveType:", true, 0, 4, 1},
    {"supportsTextureWriteRoundingMode:", true, 0, 2, 1},
};

inline constexpr Kind kKinds[] = {
    {"21H461", kCounts21H461, std::size(kCounts21H461), kArgs21H461, std::size(kArgs21H461), 0x61, "Apple", "A10X",
     "GPU", "AGXMetalA10"},
    {"24B119", kCounts24B119, std::size(kCounts24B119), kArgs24B119, std::size(kArgs24B119), 0xd2, "Apple", "A13",
     "GPU", "AGXMetalA13"},
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
