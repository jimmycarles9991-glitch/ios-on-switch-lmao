/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: one stage of a render state compiled as metal2vulkan's pipeline
 * libraries compile it, with no pipeline, for the raw GM20B path to make the
 * state's record from (nvk_switch_compile.c). The plugin finds it with dlsym.
 */

#ifndef NVK_SWITCH_COMPILE_H
#define NVK_SWITCH_COMPILE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct switch_nvk_stage_info {
   const uint32_t *spirv;
   size_t spirv_bytes;
   const char *entry;
   VkShaderStageFlagBits stage;
   const VkSpecializationInfo *specialization;
   /* The layout the library is created with: its set layouts, its push range
    * for the stage and its flags (independent sets) all reach the code.
    */
   VkPipelineLayout layout;
   VkPipelineCreateFlags2KHR pipeline_flags;
   /* The library creation's chain and the stage's: the robustness they ask
    * for, as vk_pipeline_robustness_state_fill reads it. Either may be NULL.
    */
   const void *pipeline_pnext;
   const void *stage_pnext;
   /* The fragment library's multisample state */
   bool sample_shading;
   /* The colour attachments of a 16-bit float format (bit j: attachment j),
    * whose float outputs the fragment program rounds to nearest even
    * (nvk_switch_round.h)
    */
   uint32_t half_targets;
};

struct switch_nvk_stage {
   uint8_t header[80];
   const void *code;
   uint32_t code_bytes;
   const void *data;
   uint32_t data_bytes;
   uint8_t cbuf_types[16]; /* switch_nvk_cbuf_code, the pack's PackProgram.cbufType */
   uint32_t cbuf_count;
   uint8_t num_gprs;
   bool early_fragment_tests;
   bool writes_depth;
   bool post_depth_coverage;
   void *owner; /* holds code and data until switch_nvk_stage_finish */
};

VkResult switch_nvk_compile_stage(VkDevice device,
                                  const struct switch_nvk_stage_info *info,
                                  struct switch_nvk_stage *out);
void switch_nvk_stage_finish(VkDevice device, struct switch_nvk_stage *stage);

/* Where a descriptor set layout's binding lives in a set's descriptor buffer */
struct switch_nvk_set_binding {
   uint32_t offset;
   uint32_t stride;
   uint32_t array_size;
   bool immutable_samplers;
};

/* Binding `binding` of `layout`; false past its bindings */
bool switch_nvk_set_layout_binding(VkDescriptorSetLayout layout,
                                   uint32_t binding,
                                   struct switch_nvk_set_binding *out);
/* The bytes a set of `layout` binds in the root table (its fixed part) */
uint32_t switch_nvk_set_layout_size(VkDevice device,
                                    VkDescriptorSetLayout layout);
/* The sampler header NVK made for `layout`'s immutable sampler at `binding`
 * and `element`; false where it has none
 */
bool switch_nvk_immutable_sampler_header(VkDevice device,
                                         VkDescriptorSetLayout layout,
                                         uint32_t binding, uint32_t element,
                                         uint32_t header[8]);
/* The size a buffer descriptor of `type` over `range` bytes holds */
uint32_t switch_nvk_buffer_descriptor_size(VkDevice device,
                                           VkDescriptorType type,
                                           uint64_t range);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_COMPILE_H */
