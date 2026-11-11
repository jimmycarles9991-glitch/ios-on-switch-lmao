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
};

struct NvkSetBinding {
    uint32_t offset, stride, arraySize;
    bool immutableSamplers;
};

// VkPipelineShaderStageRequiredSubgroupSizeCreateInfo
struct SubgroupSize {
    uint32_t sType = 1000225001;
    const void *next = nullptr;
    uint32_t size;
};

// VkComponentMapping
struct NvkComponents {
    uint32_t r, g, b, a;
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

inline int32_t compileStage(const DirectNvk &n, void *device, const NvkStageInfo &info, NvkStage &out)
{
    return reinterpret_cast<int32_t (*)(void *, const NvkStageInfo *, NvkStage *)>(n.compileStage)(device, &info, &out);
}

inline void stageFinish(const DirectNvk &n, void *device, NvkStage &stage)
{
    reinterpret_cast<void (*)(void *, NvkStage *)>(n.stageFinish)(device, &stage);
}

inline bool setLayoutBinding(const DirectNvk &n, uint64_t layout, uint32_t binding, NvkSetBinding &out)
{
    return reinterpret_cast<bool (*)(void *, uint32_t, NvkSetBinding *)>(n.setLayoutBinding)(handle(layout), binding,
                                                                                             &out);
}

inline uint32_t setLayoutSize(const DirectNvk &n, void *device, uint64_t layout)
{
    return reinterpret_cast<uint32_t (*)(void *, void *)>(n.setLayoutSize)(device, handle(layout));
}

inline bool immutableSamplerHeader(const DirectNvk &n, void *device, uint64_t layout, uint32_t binding,
                                   uint32_t element, uint32_t *header)
{
    return reinterpret_cast<bool (*)(void *, void *, uint32_t, uint32_t, uint32_t *)>(n.immutableSamplerHeader)(
        device, handle(layout), binding, element, header);
}

inline uint32_t bufferDescriptorSize(const DirectNvk &n, void *device, int32_t type, uint64_t range)
{
    return reinterpret_cast<uint32_t (*)(void *, int32_t, uint64_t)>(n.bufferDescriptorSize)(device, type, range);
}

inline bool samplerHeader(const DirectNvk &n, void *device, uint64_t sampler, uint32_t *header)
{
    return reinterpret_cast<bool (*)(void *, void *, uint32_t *)>(n.samplerHeader)(device, handle(sampler), header);
}

inline bool computeQmd(const DirectNvk &n, void *device, const NvkStage &stage, uint32_t setBytes, uint32_t *qmd)
{
    return reinterpret_cast<bool (*)(void *, const NvkStage *, uint32_t, uint32_t *)>(n.computeQmd)(device, &stage,
                                                                                                    setBytes, qmd);
}

inline bool viewHeaderWord0(const DirectNvk &n, void *device, uint32_t format, const NvkComponents &components,
                            bool storage, uint32_t &word0)
{
    return reinterpret_cast<bool (*)(void *, uint32_t, const NvkComponents *, bool, uint32_t *)>(n.viewHeaderWord0)(
        device, format, &components, storage, &word0);
}

}  // namespace nvk
}  // namespace raw
