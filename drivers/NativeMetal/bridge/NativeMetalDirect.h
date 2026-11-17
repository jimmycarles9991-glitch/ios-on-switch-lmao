// SPDX-License-Identifier: MIT
// The raw GM20B path's direct compile of a render state (NativeMetalDirect.rs): what m2v's translator makes of the state for
// each of the pack generator's probe draws, with no pipeline state, command buffer or capture.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMetalObjects.h"
#include "../../TegraGM20BMetal/translate/NativeMetalTranslate.h"
#include "NativeMetalBlend.h"

#ifdef __cplusplus
extern "C" {
#endif

// `descriptor`'s state (its stages' linked functions included) as the translator makes it for the probe's draws, handed to
// `each` (valid during the call). `barrier`: compiled as on a device without the pixel interlock; `textureFormat` and
// `textureSamples`: the MTLPixelFormat and sample count of the probe's textures (0: BGRA8Unorm, 1); `binds` (null: none):
// what the probe binds besides. False with nmb_last_error: refused, or `each`'s false
bool nmb_render_pipeline_direct(NmbDevice *device, const NmbRenderPipelineDescriptor *descriptor, bool barrier,
                                uint64_t textureFormat, uint32_t textureSamples, const NmbDirectBinds *binds,
                                bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);

// `descriptor`'s compute state as the translator makes it for a dispatch of `dispatch`'s threadgroup shape and bindings, handed
// to `each` (valid during the call). False with nmb_last_error: refused, or `each`'s false
bool nmb_compute_pipeline_direct(NmbDevice *device, const struct NmbComputePipelineDescriptor *descriptor,
                                 const NmbDirectDispatch *dispatch,
                                 bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);

// The build's compile profile held to `device`'s runtime (false: they differ, nmb_last_error says how), and the profile of
// `device`'s runtime as JSON into `out` (`length` bytes, unterminated): its length, 0 on failure
bool nmb_compile_profile_check(NmbDevice *device);
size_t nmb_compile_profile_freeze(NmbDevice *device, char *out, size_t length);

// Whether a heap array of class `heapClass` (metal2vulkan heap_class_code, a record's PackHeapArray) holds a texture of
// `facts`; the flags of its handle-table entry (metal2vulkan handle_entry_flags); a sampler's SamplerWord
bool switch_nmb_heap_holds(uint32_t heapClass, const NmbHeapTextureFacts *facts);
uint32_t switch_nmb_heap_texture_flags(const NmbHeapTextureFacts *facts);
uint32_t switch_nmb_sampler_word(const struct NmbSamplerDescriptor *descriptor);

#ifdef __cplusplus
}
#endif
