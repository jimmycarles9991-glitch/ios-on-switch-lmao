// SPDX-License-Identifier: MIT
// The raw GM20B path's direct compile of a render state (NativeMetalDirect.rs): what m2v's translator makes of the state for
// each of the pack generator's probe draws, with no pipeline state, command buffer or capture.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMetalObjects.h"

#ifdef __cplusplus
extern "C" {
#endif

// One stage as the pipeline library m2v creates for it compiles it (nvk_switch_compile.h's stage info, with the state's layout)
typedef struct NmbDirectStage {
    const uint32_t *spirv;
    size_t spirvBytes;
    const char *entry;
    uint32_t stage;              // VkShaderStageFlagBits
    uint32_t flags;              // VkPipelineShaderStageCreateFlags
    uint32_t subgroupSize;       // 0: none required
    const void *specialization;  // VkSpecializationInfo, null without constants
} NmbDirectStage;

typedef enum NmbDirectSource : uint32_t {
    NmbDirectBuffer,
    NmbDirectBytes,
    NmbDirectTexture,
    NmbDirectSampler,
    NmbDirectCombined,
    NmbDirectAttachment,         // a color attachment as an input attachment (the barrier form's fetch)
    NmbDirectStorageAttachment,  // a color attachment as a storage image (the interlocked storage form's)
    NmbDirectNeutral,
    NmbDirectColorTable,  // m2v's sRGB colour table (bind::color_write_table), the same bytes for every state
    NmbDirectOther
} NmbDirectSource;

#define NMB_DIRECT_IMMUTABLE UINT32_MAX
#define NMB_DIRECT_NEUTRAL   (UINT32_MAX - 1)

typedef struct NmbDirectWrite {
    uint32_t set, binding, element;
    int32_t type;  // VkDescriptorType
    NmbDirectSource source;
    uint32_t stage,
        slot;          // a buffer's or texture's Metal stage (1 vertex, 2 fragment) and index; an attachment's color
    uint32_t sampler;  // a sampler's slot, NMB_DIRECT_IMMUTABLE (the layout's constexpr sampler) or NMB_DIRECT_NEUTRAL
    uint64_t range;    // a buffer's bound range
    int32_t viewType;  // an image\'s VkImageViewType (a neutral\'s too), -1 for no image
} NmbDirectWrite;

// One probe draw on the variant its bind state settles on
typedef struct NmbDirectDraw {
    uint64_t set0;  // that variant's set 0 VkDescriptorSetLayout (0: none)
    const NmbDirectWrite *writes;
    uint32_t writeCount;
    uint32_t pushMask;  // the push-constant words [0, 128) the draw writes, and their values
    uint32_t push[32];
    uint32_t pushHoles;   // push words the consumer fills with addresses
    uint32_t vertexSlot;  // the first fixed-function vertex buffer's Metal slot (UINT32_MAX: none) and stride
    uint32_t vertexStride;
    const char *variant;
} NmbDirectDraw;

typedef struct NmbDirectState {
    uint64_t device;  // VkDevice of the stage compile
    uint64_t layout;  // VkPipelineLayout of draw 0's variant
    uint32_t stageCount;
    NmbDirectStage stages[2];
    bool sampleShading;
    bool storageForm;  // the fragment function reads color attachments through storage under the interlock
    uint32_t blended;  // the color attachments m2v hands a Vulkan blend, by index
    NmbDirectDraw draws
        [4];  // probe.mm's: 256, 16 and 4096 inline bytes per slot, and 256 bytes in buffers at each slot's own offset
} NmbDirectState;

// `descriptor`'s state (its stages' linked functions included) as the translator makes it for the probe's draws, handed to
// `each` (valid during the call). `barrier`: compiled as on a device without the pixel interlock; `textureFormat` and
// `textureSamples`: the MTLPixelFormat and sample count of the probe's textures (0: BGRA8Unorm, 1). False with
// nmb_last_error: refused, or `each`'s false
bool nmb_render_pipeline_direct(NmbDevice *device, const NmbRenderPipelineDescriptor *descriptor, bool barrier,
                                uint64_t textureFormat, uint32_t textureSamples,
                                bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
