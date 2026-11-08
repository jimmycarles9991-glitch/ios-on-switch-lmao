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

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_COMPILE_H */
