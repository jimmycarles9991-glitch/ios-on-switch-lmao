// SPDX-License-Identifier: MIT
// The direct compile's copies of NVK's stage compile ABI (third_party/mesa/src/nouveau/vulkan/nvk_switch_compile.h), with
// Vulkan's handles as pointers (this build has no Vulkan headers), and the calls through the exports DirectNvk found.
#pragma once
#include <cstddef>
#include <cstdint>
#include "raw_direct.h"

namespace raw {

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
    uint64_t xrPlaneBindings = 0;  // nvk_switch_xr_planes.h: set-0 bindings read through a header pair
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

namespace nvk {

// switch_nak_compile_stage: the stage compiled with no device, from the frozen table and the layout as data
inline int32_t nakCompileStage(const DirectNvk &n, const void *frozen, const NvkStageInfo &info,
                               const NakLayout &layout, NvkStage &out)
{
    return reinterpret_cast<int32_t (*)(const void *, const NvkStageInfo *, const NakLayout *, NvkStage *)>(
        n.nakCompileStage)(frozen, &info, &layout, &out);
}

inline void nakStageFinish(const DirectNvk &n, NvkStage &stage)
{
    reinterpret_cast<void (*)(NvkStage *)>(n.nakStageFinish)(&stage);
}

}  // namespace nvk
}  // namespace raw
