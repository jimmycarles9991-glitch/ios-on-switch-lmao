/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a raw stage compile with no instance and no device. The frozen
 * table is every input NVK's device compile read off its physical and logical
 * device; raw/runtime/raw_nak_target.h holds the GM20B's, frozen from NVK on
 * the mock GM20B.
 */
#ifndef NVK_SWITCH_OFFLINE_H
#define NVK_SWITCH_OFFLINE_H

#include "switch_compile.h"

#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "nv_device_info.h"
#include "vk_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_nak_frozen {
   uint32_t version; /* sizeof(struct switch_nak_frozen) */
   struct nv_device_info info;
   uint64_t debug_flags;
   uint32_t min_ssbo_alignment;
   struct vk_pipeline_robustness_state robustness;
   struct spirv_capabilities spirv_caps;
   uint32_t subgroup_size, min_subgroup_size, max_subgroup_size;
   uint32_t sampler_descriptor_size, sampler_descriptor_alignment;
   uint32_t image_descriptor_size, image_descriptor_alignment;
   uint32_t buffer_descriptor_size, buffer_descriptor_alignment;
};

/* One binding of a set as the caller laid the set out (NVK's rules) */
struct switch_nak_binding {
   uint32_t binding;
   int32_t type;   /* VkDescriptorType */
   uint32_t count; /* descriptors, bytes for an inline uniform block */
   uint32_t flags; /* VkDescriptorBindingFlags */
   uint32_t offset, stride;
};

struct switch_nak_set {
   const struct switch_nak_binding *bindings;
   uint32_t binding_count;
   uint32_t flags; /* VkDescriptorSetLayoutCreateFlags */
};

struct switch_nak_layout {
   const struct switch_nak_set *sets;
   uint32_t set_count;
   uint32_t create_flags; /* VkPipelineLayoutCreateFlags */
   const VkPushConstantRange *push_ranges;
   uint32_t push_range_count;
};

/* NVK's device compile of the stage, with `info->layout` ignored for
 * `layout`; out's pointers live until switch_compile_stage_finish
 */
VkResult switch_compile_stage(const struct switch_nak_frozen *frozen,
                              const struct switch_nvk_stage_info *info,
                              const struct switch_nak_layout *layout,
                              struct switch_nvk_stage *out);
void switch_compile_stage_finish(struct switch_nvk_stage *stage);
/* The stage compiled from its native program and from its portable SPIR-V:
 * the portable one where it dominates on every metric
 */
VkResult switch_compile_ranked_stage(const struct switch_nak_frozen *frozen,
                                     const struct switch_nvk_stage_info *info,
                                     const struct switch_nak_layout *layout,
                                     struct switch_nvk_stage *out);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_OFFLINE_H */
