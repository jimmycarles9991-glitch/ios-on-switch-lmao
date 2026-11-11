/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: one stage of a render state compiled as metal2vulkan's pipeline
 * libraries compile it, or a kernel as its compute pipeline compiles it, with
 * no pipeline, for the raw GM20B path to make the state's record from
 * (nvk_switch_compile.c). The plugin finds it with dlsym.
 */

#ifndef NVK_SWITCH_COMPILE_H
#define NVK_SWITCH_COMPILE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#include "nvk_switch_split.h"

#ifdef __cplusplus
extern "C" {
#endif

/* raw/runtime/raw_direct_nvk.h copies these structs field for field (the
 * plugin has no Vulkan headers): a field added here goes into its copies too,
 * which switch_nvk_compile_abi's sizes hold them to
 */
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
   /* A fragment stage's split for the raw path too (nvk_switch_split.h): it
    * compiles the stage about twice over, which the device does off the
    * state's creation
    */
   bool split;
   /* The colour attachments of a 16-bit float format (bit j: attachment j),
    * whose float outputs the fragment program rounds to nearest even
    * (nvk_switch_round.h)
    */
   uint32_t half_targets;
   /* Root push words [0x700, 0x780) constant across draws (bit i: word i). */
   uint32_t const_push_mask;
   uint32_t const_push_words[32];
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
   /* A fragment stage's split for the raw path (nvk_switch_split.h), NULL
    * where it has none; owned with code and data
    */
   const struct switch_nvk_split *split;
   void *owner; /* holds code and data until switch_nvk_stage_finish */
};

VkResult switch_nvk_compile_stage(VkDevice device,
                                  const struct switch_nvk_stage_info *info,
                                  struct switch_nvk_stage *out);
void switch_nvk_stage_finish(VkDevice device, struct switch_nvk_stage *stage);

/* A stage's split as the raw path records one: its per-pixel program and its
 * prologue as stages (the prologue's header zeros, as programs.bin holds a
 * kernel's; neither owns anything), its layout, QMD and threshold
 */
struct switch_nvk_split_parts {
   struct switch_nvk_stage fs;
   struct switch_nvk_stage prologue;
   struct switch_nvk_split_layout layout;
   uint32_t qmd[64];
   uint32_t threshold_pixels;
};

void switch_nvk_split_parts(const struct switch_nvk_split *split,
                            struct switch_nvk_split_parts *out);

/* sizeof switch_nvk_stage_info, switch_nvk_stage and switch_nvk_split_parts:
 * the plugin's copies of them are checked against these
 */
void switch_nvk_compile_abi(uint32_t sizes[3]);

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
/* The header NVK made for `sampler` */
bool switch_nvk_sampler_header(VkDevice device, VkSampler sampler,
                               uint32_t header[8]);
/* The QMD NVK fills for a dispatch of compute `stage` with set 0 of
 * `set_bytes`, its program offset, grid and constant buffer addresses 0;
 * false where the stage reads another constant buffer than the root table,
 * set 0 and its shader data
 */
bool switch_nvk_compute_qmd(VkDevice device,
                            const struct switch_nvk_stage *stage,
                            uint32_t set_bytes, uint32_t qmd[64]);
/* Word 0 of the texture header of a view of `format` with `components`, as
 * a sampled or storage image (format and swizzle alone)
 */
bool switch_nvk_view_header_word0(VkDevice device, VkFormat format,
                                  const VkComponentMapping *components,
                                  bool storage, uint32_t *word0);
/* The size a buffer descriptor of `type` over `range` bytes holds */
uint32_t switch_nvk_buffer_descriptor_size(VkDevice device,
                                           VkDescriptorType type,
                                           uint64_t range);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_COMPILE_H */
