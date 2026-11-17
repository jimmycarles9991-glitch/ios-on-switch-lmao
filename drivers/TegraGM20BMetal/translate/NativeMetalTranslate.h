// SPDX-License-Identifier: MIT
// The C ABI of the raw path's translation with no m2v runtime (native_metal_translate, NativeMetalTranslate.rs): libraries
// and functions the translator makes alone, the states and kernels it compiles of them on the build's compile profile's
// devices (every Vulkan handle they hand over 0), and the pipeline descriptors' fields, which the bridge's runtime device
// takes too (NativeMetalObjects.h). A call that fails returns 0/NULL/false and leaves its reason in nmb_last_error() (per
// thread).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../raw/runtime/raw_direct_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *nmb_last_error(void);

struct NmbConstant {
    uint64_t type, index;
    const char *name;
    const void *bytes;
    uint64_t length;
};

struct NmbColorAttachment {
    uint64_t format, write_mask, blending, source_rgb, destination_rgb, rgb_operation, source_alpha, destination_alpha,
        alpha_operation;
};

struct NmbVertexAttribute {
    uint64_t format, offset, buffer;
};

struct NmbVertexLayout {
    uint64_t stride, step_function, step_rate;
};

struct switch_linked_group_name {
    const char *bytes;
    uint64_t length;
};

/* A render pipeline descriptor's fields over handles of `Function` (NmbFunction, or the offline compile's
 * NmbOfflineFunction below), as the bridge's NmbRenderPipelineDescriptorOf takes them. vertex_inputs 0: no
 * vertex descriptor. The linked arrays are vertexLinkedFunctions' and fragmentLinkedFunctions' functions and
 * privateFunctions, as NmbComputePipelineDescriptor's linked functions are. tessellation through
 * factor_scale_enabled are the descriptor's tessellator state, as MTLRenderPipelineDescriptor's (0: Metal's defaults),
 * and max_amplification its maxVertexAmplificationCount. */
#define NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS(Function)                                                           \
    Function *vertex, *fragment;                                                                                  \
    struct NmbColorAttachment colors[8];                                                                          \
    uint64_t depth_format, stencil_format, samples, topology, rasterization;                                      \
    uint64_t vertex_inputs;                                                                                       \
    struct NmbVertexAttribute attributes[31];                                                                     \
    struct NmbVertexLayout layouts[31];                                                                           \
    Function *const *vertex_linked;                                                                               \
    uint64_t vertex_linked_count;                                                                                 \
    Function *const *vertex_private_linked;                                                                       \
    uint64_t vertex_private_count;                                                                                \
    Function *const *fragment_linked;                                                                             \
    uint64_t fragment_linked_count;                                                                               \
    Function *const *fragment_private_linked;                                                                     \
    uint64_t fragment_private_count;                                                                              \
    uint64_t tessellation, max_tessellation_factor, partition_mode, factor_format, factor_step_function, winding, \
        control_point_index_type, factor_scale_enabled;                                                           \
    uint64_t max_amplification;

/* As NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS, a compute pipeline descriptor's. The linked arrays are MTLLinkedFunctions'
 * functions and privateFunctions, the external functions the kernel calls by name (counted arrays of live handles for
 * the duration of the call; none where the count is 0). The stage_ fields are the stageInputDescriptor (stage_inputs 0:
 * none): its attributes by index (format 0: none) and layouts by buffer index (stride UINT64_MAX:
 * MTLBufferLayoutStrideDynamic), its index type and index buffer. group_linked has group_count members; group_names
 * names each member's call-site group, preserving member order within each group. */
#define NMB_COMPUTE_PIPELINE_DESCRIPTOR_FIELDS(Function)                                                        \
    Function *function;                                                                                         \
    uint64_t max_threads, multiple_of_execution_width, indirect, max_call_stack_depth, adding_binary_functions; \
    uint64_t mutability[31];                                                                                    \
    Function *const *linked;                                                                                    \
    uint64_t linked_count;                                                                                      \
    Function *const *private_linked;                                                                            \
    uint64_t private_count;                                                                                     \
    Function *const *binary_linked;                                                                             \
    uint64_t binary_count;                                                                                      \
    uint64_t stage_inputs;                                                                                      \
    struct NmbVertexAttribute stage_attributes[31];                                                             \
    struct NmbVertexLayout stage_layouts[31];                                                                   \
    uint64_t stage_index_type, stage_index_buffer;                                                              \
    Function *const *group_linked;                                                                              \
    const struct switch_linked_group_name *group_names;                                                         \
    uint64_t group_count;

// The direct compile with no m2v runtime (NativeMetalOffline.rs): libraries and functions the translator makes alone, and
// states and kernels of them made on the build's compile profile's devices, every Vulkan handle they hand over 0.
typedef struct NmbOfflineLibrary NmbOfflineLibrary;
typedef struct NmbOfflineFunction NmbOfflineFunction;

// As nmb_library_new's metallib, nmb_function_new's and nmb_function_new_specialized's functions
NmbOfflineLibrary *nmb_offline_library_new(const void *bytes, size_t length);
void nmb_offline_library_release(NmbOfflineLibrary *);
NmbOfflineFunction *nmb_offline_function_new(const NmbOfflineLibrary *, const char *name);
NmbOfflineFunction *nmb_offline_function_new_specialized(const NmbOfflineLibrary *, const char *name,
                                                         const char *specialized_name,
                                                         const struct NmbConstant *constants, uint32_t count);
void nmb_offline_function_release(NmbOfflineFunction *);
uint64_t nmb_offline_function_type(const NmbOfflineFunction *);
// vertexAttributes, as nmb_function_attribute_count and nmb_function_attribute answer it
int64_t nmb_offline_function_vertex_attribute_count(const NmbOfflineFunction *);
size_t nmb_offline_function_vertex_attribute(const NmbOfflineFunction *, uint32_t at, char *out, size_t size,
                                             uint64_t info[5]);
// stageInputAttributes, likewise
int64_t nmb_offline_function_stage_input_attribute_count(const NmbOfflineFunction *);
size_t nmb_offline_function_stage_input_attribute(const NmbOfflineFunction *, uint32_t at, char *out, size_t size,
                                                  uint64_t info[5]);
// newArgumentEncoderWithBufferIndex:'s layouts at every buffer index the function declares, in index order: `each` gets
// the index, whether it is an argument buffer (else a plain buffer, whose encoder Metal makes only without API validation)
// and its layouts as nmb_function_argument_layouts gives them, or none and `refusal` where Metal makes no encoder. Valid
// during `each`; false with nmb_last_error where the function does not decode, or each's false.
bool nmb_offline_function_buffer_encoders(const NmbOfflineFunction *,
                                          bool (*each)(uint32_t index, bool argument_buffer,
                                                       const struct NmbArgumentLayout *layouts, uint32_t count,
                                                       const char *refusal, void *ctx),
                                          void *ctx);

typedef struct NmbOfflineRenderPipelineDescriptor {
    NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS(NmbOfflineFunction)
} NmbOfflineRenderPipelineDescriptor;

typedef struct NmbOfflineComputePipelineDescriptor {
    NMB_COMPUTE_PIPELINE_DESCRIPTOR_FIELDS(NmbOfflineFunction)
} NmbOfflineComputePipelineDescriptor;

// nmb_render_pipeline_direct's and nmb_compute_pipeline_direct's answers for offline functions
bool nmb_render_pipeline_offline(const NmbOfflineRenderPipelineDescriptor *descriptor, bool barrier,
                                 uint64_t textureFormat, uint32_t textureSamples, const NmbDirectBinds *binds,
                                 bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);
bool nmb_compute_pipeline_offline(const NmbOfflineComputePipelineDescriptor *descriptor,
                                  const NmbDirectDispatch *dispatch,
                                  bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);
// Whether the m2v runtime's own compile device compiles `descriptor`'s kernel, as its compute state creation does (false
// with nmb_last_error)
bool nmb_offline_compute_pipeline_compiles(const NmbOfflineComputePipelineDescriptor *descriptor);
// The answers the runtime's compute prepare gives `descriptor`'s state (its native compute artifact's metal_info), into
// `facts`: maxTotalThreadsPerThreadgroup, threadExecutionWidth, staticThreadgroupMemoryLength (false with nmb_last_error)
bool nmb_offline_compute_pipeline_facts(const NmbOfflineComputePipelineDescriptor *descriptor, uint32_t facts[3]);

// A state's reflection with no m2v runtime, as raw/runtime/raw_reflection_blob.h lays it out (NativeMetalReflection.rs):
// a render state's as newRenderPipelineStateWithDescriptor:options:reflection:error: answers it with argument info, its
// vertex, fragment, tile, object and mesh stages; a compute state's as the runtime's compute prepare gives it (its kernel's
// bindings). Null with nmb_last_error. Its bytes live until nmb_reflection_blob_release.
typedef struct NmbReflectionBlob NmbReflectionBlob;
NmbReflectionBlob *nmb_offline_render_pipeline_reflection(const NmbOfflineRenderPipelineDescriptor *descriptor);
NmbReflectionBlob *nmb_offline_compute_pipeline_reflection(const NmbOfflineComputePipelineDescriptor *descriptor);
const uint8_t *nmb_reflection_blob_bytes(const NmbReflectionBlob *blob, size_t *length);
void nmb_reflection_blob_release(NmbReflectionBlob *blob);

#ifdef __cplusplus
}
#endif
