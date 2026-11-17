// SPDX-License-Identifier: MIT
// The direct compile's records as the translator hands them to raw (RawDirect.mm, raw_direct.cpp, raw_direct_compute.cpp):
// a render state's stages, bindings and probe draws, a compute state's kernel at a dispatch shape, the samplers and heap
// textures raw asks the translator about, and a function's argument buffer layouts. The bridge's Rust mirrors each
// (NativeMetalDirect.rs, NativeMetalObjects.rs, NativeMetalHeap.rs) field for field.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NmbSamplerDescriptor {
    uint64_t min_filter, mag_filter, mip_filter, s_address, t_address, r_address, border_color, normalized,
        max_anisotropy, compare, lod_average;
    float lod_min, lod_max;
};

struct NmbArgumentRun {
    uint32_t id, count, ids_per_element, width, kind, nested;
    uint64_t offset, stride, element_stride, size;
};

struct NmbArgumentLayout {
    uint64_t encoded_length, alignment;
    const struct NmbArgumentRun *runs;
    uint32_t run_count, reserved;
};

// One stage as the pipeline library m2v creates for it compiles it (nvk_switch_compile.h's stage info, with the state's layout)
typedef struct NmbDirectStage {
    const uint32_t *spirv;
    size_t spirvBytes;
    const char *entry;
    uint32_t stage;              // VkShaderStageFlagBits
    uint32_t flags;              // VkPipelineShaderStageCreateFlags
    uint32_t subgroupSize;       // 0: none required
    const void *specialization;  // VkSpecializationInfo, null without constants
    const void *semantic;
    const void *portableContext;
    bool (*portable)(const void *context, const uint32_t **words, size_t *bytes);
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
    NmbDirectOther,
    NmbDirectDepthStandIn,
    NmbDirectStencilStandIn,
    NmbDirectTextureTable,   // a stage's handle table of argument-buffer textures (m2v Synthetic(HandleTable))
    NmbDirectSamplerTable,   // and of argument-buffer samplers (Synthetic(SamplerTable))
    NmbDirectArgumentTable,  // push fields past the push window (Synthetic(ArgumentTable)), which instantiation fills
    // a tessellation state's control stage: the encoder's factor buffer, the draw's patch and control-point index buffers
    NmbDirectTessFactors,
    NmbDirectPatchIndices,
    NmbDirectControlPointIndices,
    NmbDirectExtendedShadow  // an extended-range colour's half shadow (m2v WriteSource::ExtendedShadow), a storage image
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
    uint32_t format;  // a fragment texture's: the MTLPixelFormat of the texture the probe bound at its slot, 0 for none
    uint32_t
        compare;  // 1: a sampler slot the state samples by comparison (m2v's compare-enabled VkSampler for this use)
} NmbDirectWrite;

// A sampler's state as vk_sampler_state_init makes it of the VkSamplerCreateInfo metal2vulkan creates (Vulkan values;
// kits/SwitchGM20B/SwitchGM20BSampler.h packs it)
typedef struct NmbDirectSamplerState {
    uint32_t magFilter, minFilter, mipmapMode, address[3];
    float mipLodBias, minLod, maxLod, maxAnisotropy;
    uint32_t compareOp, borderColor, reduction, borderComponents[4];
    bool anisotropy, compare, unnormalized;
} NmbDirectSamplerState;

// A binding of a variant's set 0 layout: its VkDescriptorType, count and VkDescriptorBindingFlags, and its immutable
// sampler's state
typedef struct NmbDirectBinding {
    uint32_t binding;
    int32_t type;
    uint32_t count, flags;
    bool immutable;
    NmbDirectSamplerState sampler;
} NmbDirectBinding;

// A set of a variant's layout, its push constant ranges (VkPushConstantRange's layout), and the layout: what NVK's stage
// compile reads of a VkPipelineLayout
typedef struct NmbDirectSet {
    const NmbDirectBinding *bindings;
    uint32_t bindingCount;
    uint32_t flags;  // VkDescriptorSetLayoutCreateFlags
} NmbDirectSet;

typedef struct NmbDirectPushRange {
    uint32_t stages, offset, size;
} NmbDirectPushRange;

typedef struct NmbDirectLayout {
    const NmbDirectSet *sets;
    uint32_t setCount;
    const NmbDirectPushRange *push;
    uint32_t pushCount;
} NmbDirectLayout;

// One probe draw on the variant its bind state settles on
typedef struct NmbDirectDraw {
    uint64_t set0;  // that variant's set 0 VkDescriptorSetLayout (0: none)
    const NmbDirectBinding *set0Bindings;
    uint32_t set0BindingCount;
    const NmbDirectWrite *writes;
    uint32_t writeCount;
    uint32_t pushMask;  // the push-constant words [0, 128) the draw writes, and their values
    uint32_t push[32];
    uint32_t blendColorWord, halfBlendColorWord;  // push word + 1, zero when absent
    uint32_t depthTestWord, stencilTestWord;
    uint32_t shadowStencilWord;  // m2v's ShadowStencilState (a fragment function's [[stencil]] output): push word + 1
    uint32_t pushHoles;          // push words the consumer fills with addresses
    // Of them, each stage's (vertex, fragment) root-table and ExecutionStatus address as push word + 1 (0: none), and the
    // holes of any other kind; the buffer slots each stage's root table holds (bit n: [[buffer(n)]], m2v BufferForm::Psb)
    uint8_t rootTableWord[2], statusWord[2];
    uint32_t otherHoles;
    uint32_t rootSlots[2];
    uint64_t set1;  // the variant's set 1 VkDescriptorSetLayout, its heap (0: none, or no device)
    // set 1's arrays by binding: each one's class (metal2vulkan heap_class_code, 0: one it cannot word)
    uint32_t heapArrays;
    uint32_t heapClass[8];
    uint32_t vertexSlot;  // the first fixed-function vertex buffer's Metal slot (UINT32_MAX: none) and stride
    uint32_t vertexStride;
    uint32_t vertexStrideDynamic;  // that buffer's stride is its bind's (MTLBufferLayoutStrideDynamic)
    // a pulled vertex buffer layout of a dynamic stride: its Metal buffer index and m2v's VertexBufferStride push word + 1
    uint32_t pulledStrides[4][2];
    // vertex amplification: the count the draw was planned at (the descriptor's maxVertexAmplificationCount, one draw per
    // amplification), the push words + 1 of m2v's AmplificationId and AmplificationCount (0: none)
    uint32_t amplificationMax, amplificationIdWord, amplificationCountWord;
    // and of its AmplificationLayerOffset and AmplificationViewportOffset (it is compiled for a layered pass whose
    // mappings offset the viewport), which each amplification writes
    uint32_t amplificationLayerWord, amplificationViewportWord;
    // push words + 1 of m2v's RenderTargetArrayLength (a vertex function that chooses its slice: compiled for a layered
    // pass; 0xFF where the translation has no layered form of the state, compiled for one layer) and ViewportCount (one
    // that chooses its viewport), which each draw writes
    uint32_t arrayLengthWord, viewportCountWord;
    // the vertex attributes the translation reads in the shader, by Metal index: fixed-function input fetches the others,
    // numbered densely in Metal attribute order (m2v link/varyings)
    uint32_t pulledAttributes;
    const char *variant;
    // Each stage's argument table (NmbDirectArgumentTable's words, m2v's push fields past its push window) as instantiation
    // filled it for the draw; a dynamic field there is numbered from push word 32 (raw_spill_table.h)
    const uint32_t *spill[2];
    uint32_t spillWords[2];
    // By stage and buffer slot, the record's word + 1 of the slot's SlotBindOffset residual as the plan places it: a root
    // push word (1-32) or the stage's argument table word + 33 (33-63); 0: none; 255: placed past what a record names
    uint8_t residualWord[2][32];
} NmbDirectDraw;

typedef struct NmbDirectStorage {
    uint32_t mask, writes, samplePlanes, samples;
    uint32_t formats[8];  // actual identity-mapped storage view, including sRGB's integer view
    uint32_t fragmentTests;
    uint32_t depthFormat, stencilFormat, testSamples, testPlanes;
    // the attachments whose fixed-function blend the shader computes, and the union of their blend models' assumptions
    // (m2v StorageAttachmentUse::blend_model)
    uint32_t blendMask, blendModel;
    // a fragment function's [[stencil]] output tested in its program (m2v ShadowStencilUse): the shadow's VkFormat (0: none,
    // or a pass without a stencil attachment) and samples
    uint32_t shadowFormat, shadowSamples;
} NmbDirectStorage;

// A tessellation state's draws (m2v link/tessellation): each patch one control point of a PATCH_LIST of one. `parameters`:
// SET_TESSELLATION_PARAMETERS as NVK's NVK_MME_SET_TESS_PARAMS makes it of the control stage's modes; `controlWord`: the
// push word of the control stage's first field + 1 (control::PUSH_FIELDS: factor residual, instance stride, scale, patch
// index residual, patch index bound, control-point index residual); `controlPoints`: the function's [[patch(_, N)]] N (0:
// unstated); `indexBytes`: its control-point index size (0: none)
typedef struct NmbDirectTess {
    uint32_t parameters;
    uint32_t controlWord;
    uint32_t controlPoints;
    uint32_t indexBytes;
} NmbDirectTess;

typedef struct NmbDirectState {
    uint64_t device;                    // VkDevice of the stage compile
    uint64_t layout;                    // VkPipelineLayout of draw 0's variant
    const NmbDirectLayout *layoutData;  // that layout, as data
    uint32_t stageCount;                // 2, 4 for a tessellation state, 1 for a state of no fragment function
    NmbDirectStage stages[4];           // vertex, fragment, then a tessellation state's control and evaluation stages
    NmbDirectTess tess;
    bool sampleShading;
    bool storageForm;  // the fragment function reads color attachments through storage under the interlock
    NmbDirectStorage storage;
    uint32_t blended;  // the color attachments m2v hands a Vulkan blend, by index
    NmbDirectDraw draws
        [4];  // probe.mm's: 256, 16 and 4096 inline bytes per slot, and 256 bytes in buffers at each slot's own offset
    bool twoPlane;  // compiled with each RGB10A8_2P_XR10 colour in its two planes (NmbDirectBinds::twoPlane)
} NmbDirectState;

// A sampler slot the probe binds a cubic sampler state at (MTLSamplerMinMagFilterCubic, no mip filter): its address
// modes and border colour (MTLSamplerAddressMode, MTLSamplerBorderColor) and coordinates; the fragment stage's unless
// `vertex`
typedef struct NmbDirectCubicSampler {
    uint32_t slot, sAddress, tAddress, rAddress, borderColor;
    bool normalized;
    bool vertex;
} NmbDirectCubicSampler;

// A texture slot the probe binds a texture of another MTLPixelFormat at, the fragment stage's unless `vertex`
typedef struct NmbDirectTextureFormat {
    uint32_t slot;
    uint64_t format;
    bool vertex;
} NmbDirectTextureFormat;

// What the probe binds beyond its tagged textures and samplers (the raw path's cubic variants: raw/runtime/raw_cubic.h).
// `twoPlane`: each RGB10A8_2P_XR10 attachment held in Metal's two planes (raw/runtime/raw_xr_pass.h)
typedef struct NmbDirectBinds {
    const NmbDirectCubicSampler *cubic;
    uint32_t cubicCount;
    const NmbDirectTextureFormat *textures;
    uint32_t textureCount;
    uint32_t fragmentTests;
    uint32_t bindStride;  // the attributeStride the probe binds every vertex buffer at (0: none)
    bool twoPlane;
    // the extended-range colours whose half shadow the draws' pass holds, bit n colour n (m2v BindState::extended_shadows)
    uint32_t extendedShadows;
} NmbDirectBinds;

// A texture a kernel's dispatch binds at `slot`, as its MTLTextureDescriptor answers
typedef struct NmbDirectBoundTexture {
    uint32_t slot;
    uint32_t planes;  // 1: an RGB10A8_2P_XR10 texture held in Metal's two planes (raw_xr_pass.h), 0: one image
    uint64_t pixelFormat, textureType, usage, levels, arrayLength, samples;
} NmbDirectBoundTexture;

typedef struct NmbDirectBoundSampler {
    uint32_t slot;
    struct NmbSamplerDescriptor descriptor;
} NmbDirectBoundSampler;

// What a compute state's dispatch binds besides its buffers, which the export binds itself (the probe's: 4096 inline bytes at
// every slot, the same bytes in buffers elsewhere, and each buffer `1 + slot % 15` bytes past its start)
typedef struct NmbDirectDispatch {
    uint32_t threads[3];
    uint32_t memory[31];  // threadgroup memory lengths by index, 0: none
    const NmbDirectBoundTexture *textures;
    uint32_t textureCount;
    const NmbDirectBoundSampler *samplers;
    uint32_t samplerCount;
    uint32_t regionMode;
} NmbDirectDispatch;

typedef struct NmbDirectKernelWrite {
    NmbDirectWrite write;
    uint32_t viewFormat;  // a texture's view: its VkFormat, its VkComponentSwizzles with the texture's own composed in
    uint32_t components[4];  //   (identity where the function swizzles itself), 0: no view
    uint32_t plane;          // 1: the view of a two-plane RGB10A8_2P_XR10 texture's alpha plane (R8_UINT) at its slot
    uint64_t sampler;  // a sampler made for the write (VkSampler on the kernel's device, live during the call), 0: none
    NmbDirectSamplerState samplerState;  // that sampler's state
    bool hasSampler;                     // the write takes a sampler (its state; offline, with no VkSampler)
} NmbDirectKernelWrite;

typedef struct NmbDirectKernel {
    uint64_t device;                    // VkDevice of the stage compile
    uint64_t layout;                    // VkPipelineLayout of the dispatch's variant
    const NmbDirectLayout *layoutData;  // that layout, as data
    uint64_t set0;                      // its set 0 VkDescriptorSetLayout (0: none)
    const NmbDirectBinding *set0Bindings;
    uint32_t set0BindingCount;
    NmbDirectStage stage;  // the dispatch's region: its specialization, stage flags and required subgroup size
    const NmbDirectKernelWrite *writes;
    uint32_t writeCount;
    // The push-constant words [0, 128) of the probe's three dispatches: inline bytes, buffers elsewhere, buffers off 16
    uint32_t pushMask, push[32], movedMask, moved[32], boundMask, bound[32];
    // The buffers-elsewhere dispatch ran on the same variant, specialization and set-0 plan; the bind-offset one too
    bool movedSame, boundSame;
    int32_t regionWord;  // the push word of m2v's DispatchRegion field, -1: none
    uint32_t regions;    // the plain dispatch's Vulkan dispatches
    uint32_t pushHoles;  // push words the consumer fills with addresses
    const char *variant;
    uint32_t bufferReads, bufferWrites, textureReads, textureWrites;
    // Its reads through argument buffers, as NmbDirectDraw's for the kernel stage: the push word + 1 of its root table's
    // address and of its ExecutionStatus buffer's (0: none), the other holes, the buffer slots its root table holds, its
    // set 1 (a VkDescriptorSetLayout, 0: none, or no device) and each array's class (switch_nmb_heap_holds' words)
    uint8_t rootTableWord, statusWord;
    uint32_t otherHoles, rootSlots;
    uint64_t set1;
    uint32_t heapArrays, heapClass[8];
    // Its argument table's words in each of the three dispatches (NmbDirectDraw's spill)
    const uint32_t *spill, *movedSpill, *boundSpill;
    uint32_t spillWords, movedSpillWords, boundSpillWords;
    uint8_t residualWord[32];  // NmbDirectDraw's for the kernel stage
    // a stage-in kernel's: the push word of m2v's StageInRegion field (two uvec4: origin, size; -1: none), and each layout
    // of a dynamic stride's Metal buffer index and StageInStride push word + 1
    int32_t stageInRegionWord;
    uint32_t stageInStrides[4][2];
} NmbDirectKernel;

// A texture as the raw path's device-wide heap asks of it (raw_heap_tables.h HeapTextureFacts): its MTLTextureDescriptor's
// pixel format, type, sample count, usage, array length, levels and swizzle (MTLTextureSwizzle values)
typedef struct NmbHeapTextureFacts {
    uint32_t pixelFormat, textureType, sampleCount, usage, arrayLength, levels;
    uint8_t swizzle[4];
} NmbHeapTextureFacts;

#ifdef __cplusplus
}
#endif
