// SPDX-License-Identifier: MIT
// The direct compile's copies of the stage compile's ABI (third_party/mesa/src/nouveau/switch_compile: switch_compile.h,
// nvk_switch_offline.h), with Vulkan's handles as pointers (this build has no Vulkan headers), and its entry points, which
// the image links (switch_compile.o).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "raw_direct.h"

namespace raw {

// switch_compile.h SWITCH_XR_PLANE_WORDS: bit b % 64 of word b / 64, set-0 binding b
constexpr uint32_t kXrPlaneWords = 6;
using XrPlaneBindings = std::array<uint64_t, kXrPlaneWords>;

struct NvkStageInfo {
    const uint32_t *spirv;
    size_t spirvBytes;
    const char *entry;
    uint32_t stage;
    const void *specialization;
    void *layout;
    uint64_t pipelineFlags;
    const void *pipelinePnext;
    const void *stagePnext;
    bool sampleShading;
    bool split;
    uint32_t halfTargets;
    uint32_t constPushMask;
    uint32_t constPushWords[32];
    uint64_t centerFragDepth;
    const void *semantic;
    const void *portableContext;
    bool (*portable)(const void *context, const uint32_t **words, size_t *bytes);
    bool texsF16 = false;
    bool turned = false;
    XrPlaneBindings xrPlaneBindings = {};  // nvk_switch_xr_planes.h: set-0 bindings read through a header pair
    // nvk_switch_ubo_fold.h: the set-0 uniform buffer at descriptor byte uboFoldDesc, its bytes in uboFoldMask held as
    // uboFoldBytes
    uint32_t uboFoldDesc = 0;
    uint64_t uboFoldMask = 0;
    uint8_t uboFoldBytes[64] = {};
};

struct NvkStage {
    uint8_t header[80];
    const void *code;
    uint32_t codeBytes;
    const void *data;
    uint32_t dataBytes;
    uint8_t cbufTypes[16];
    uint32_t cbufCount;
    uint8_t numGprs;
    bool earlyFragmentTests, writesDepth, postDepthCoverage;
    const void *split;
    void *owner;
    uint32_t interlockControl;
    uint64_t usesSampleShading;
    uint32_t semanticVersion;
    uint8_t boundTextures;
    uint8_t numBarriers;
    uint16_t localSize[3];
    uint16_t smemSize;
    uint32_t slmSize, crsSize;
    uint8_t cbufSets[16];
    uint32_t cbufOffsets[16];  // a uniform buffer descriptor's offset in its set (cbuf_offsets)
};

struct NvkStageMetrics {
    uint32_t version, codeBytes, instructions, gprs, maxWarps;
    uint32_t scratchBytes, memorySpills, memoryFills, registerSpills, registerFills;
    uint64_t staticCycles;
};

static_assert(sizeof(NvkStageMetrics) == 48);

// nvk_switch_offline.h's layout as data: each binding with the offset and stride raw lays it out at (raw_set_layout.h)
struct NakBinding {
    uint32_t binding;
    int32_t type;
    uint32_t count, flags, offset, stride;
};

struct NakSet {
    const NakBinding *bindings;
    uint32_t bindingCount, flags;
};

struct NakLayout {
    const NakSet *sets;
    uint32_t setCount, createFlags;
    const void *pushRanges;  // VkPushConstantRange
    uint32_t pushRangeCount;
};

// VkPipelineShaderStageRequiredSubgroupSizeCreateInfo
struct SubgroupSize {
    uint32_t sType = 1000225001;
    const void *next = nullptr;
    uint32_t size;
};

enum DescriptorType : int32_t {
    Sampler = 0,
    CombinedImageSampler = 1,
    SampledImage = 2,
    StorageImage = 3,
    UniformTexelBuffer = 4,
    StorageTexelBuffer = 5,
    UniformBuffer = 6,
    StorageBuffer = 7,
    UniformBufferDynamic = 8,
    StorageBufferDynamic = 9,
    InputAttachment = 10
};

constexpr uint32_t kComputeStage = 0x20;  // VK_SHADER_STAGE_COMPUTE_BIT

inline bool bufferType(int32_t t)
{
    return t >= UniformBuffer && t <= StorageBufferDynamic;
}

inline void *handle(uint64_t h)
{
    return reinterpret_cast<void *>(uintptr_t(h));
}

// nvk_switch_split.h switch_nvk_split_layout, switch_compile.h switch_nvk_split_parts
struct NvkSplitLayout {
    uint32_t frontierBytes;
    uint8_t frontierCbuf, prologueCbufs, shaderDataCbuf, uboCount;
    uint8_t uboCbuf[6];
    uint16_t batchVersion, batchThreads;
    uint32_t frontierValues, prologueInstrs;
};

struct NvkSplitParts {
    NvkStage fs, prologue;
    NvkSplitLayout layout;
    uint32_t qmd[64];
    uint32_t thresholdPixels;
    SplitCost cost;
};

}  // namespace raw

extern "C" {
int32_t switch_compile_stage(const void *frozen, const raw::NvkStageInfo *info, const raw::NakLayout *layout,
                             raw::NvkStage *out);
int32_t switch_compile_ranked_stage(const void *frozen, const raw::NvkStageInfo *info, const raw::NakLayout *layout,
                                    raw::NvkStage *out);
void switch_compile_stage_finish(raw::NvkStage *stage);
void switch_compile_split_parts(const void *split, raw::NvkSplitParts *out);
void switch_compile_abi(uint32_t sizes[3]);
}

namespace raw {

// The stage compiled with no device, from the frozen table and the layout as data (the native seed's rank mode: the
// cheaper of its native and portable programs)
inline int32_t compileStage(const StageCompile &c, const NvkStageInfo &info, const NakLayout &layout, NvkStage &out)
{
    return (c.ranked ? switch_compile_ranked_stage : switch_compile_stage)(c.target, &info, &layout, &out);
}
}  // namespace raw
