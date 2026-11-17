/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a render state's stage compiled from metal2vulkan's SPIR-V and
 * layout as its graphics pipeline libraries compile it (no link-time
 * optimization: each stage alone), with no pipeline, so the raw GM20B path
 * makes the state's programs without a twin pipeline or a capture.
 */

#include "nvk_switch_compile.h"
#include "nvk_switch_stage.h"
#include "nvk_switch_turn.h"
#include "nvk_switch_ubo_fold.h"
#include "nvk_switch_xr_planes.h"

#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_shader.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_split.h"
#include "nvk_switch_ir.h"

#include "vk_graphics_state.h"
#include "vk_pipeline.h"
#include "vk_shader.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

static nir_shader *
build_semantic(const void *program,
                const struct nir_shader_compiler_options *options,
                const struct spirv_to_nir_options *address_options,
                const VkSpecializationInfo *specialization)
{
   return switch_nvk_ir_to_nir(program, options, address_options, specialization);
}

/* The code, header, shader data and constant buffers of info's stage, as the
 * library metal2vulkan creates for it would hold them. The fragment library's
 * state has no rasterization state (vk_graphics_state.c files it under
 * pre-rasterization), so its fragment key underestimates, as the library's
 * does. out's pointers live until switch_nvk_stage_finish.
 */
SWITCH_EXPORT VkResult
switch_nvk_compile_stage(VkDevice _device,
                         const struct switch_nvk_stage_info *info,
                         struct switch_nvk_stage *out)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   memset(out, 0, sizeof(*out));

   VkShaderModuleCreateInfo module = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = info->stage_pnext,
      .codeSize = info->spirv_bytes,
      .pCode = info->spirv,
   };
   const VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &module,
      .stage = info->stage,
      .pName = info->entry,
      .pSpecializationInfo = info->specialization,
   };
   const struct vk_multisample_state ms = {
      .sample_shading_enable = info->sample_shading,
   };
   const struct vk_graphics_pipeline_state state = {
      .shader_stages = info->stage,
      .pipeline_flags = info->pipeline_flags,
      .ms = info->stage == VK_SHADER_STAGE_FRAGMENT_BIT ? &ms : NULL,
   };

   struct switch_nvk_const_push push = {.mask = info->const_push_mask};
   memcpy(push.words, info->const_push_words, sizeof(push.words));
   switch_nvk_const_push_current = push.mask ? &push : NULL;
   struct switch_nvk_ubo_fold ubo = {.desc = info->ubo_fold_desc,
                                     .mask = info->ubo_fold_mask};
   memcpy(ubo.bytes, info->ubo_fold_bytes, sizeof(ubo.bytes));
   switch_nvk_ubo_fold_current = ubo.mask ? &ubo : NULL;
   switch_nvk_half_targets_current = info->half_targets;
   struct vk_shader *shader = NULL;
   switch_nvk_split_current = info->split;
   switch_nvk_center_frag_depth_current = info->center_frag_depth != 0;
   switch_nvk_texs_f16_current = info->texs_f16;
   switch_nvk_turned_current = info->turned;
   memcpy(switch_nvk_xr_plane_bindings_current, info->xr_plane_bindings,
          sizeof(switch_nvk_xr_plane_bindings_current));
   bool native_used = false;
   VkResult result =
      vk_pipeline_compile_native_stage(&dev->vk, info->pipeline_flags,
                                        info->pipeline_pnext, &stage,
                                        info->layout,
                                        info->stage == VK_SHADER_STAGE_COMPUTE_BIT ?
                                        NULL : &state,
                                        info->stage == VK_SHADER_STAGE_COMPUTE_BIT ? build_semantic : NULL,
                                        info->semantic, &native_used, &shader);
   if (result == VK_PIPELINE_COMPILE_REQUIRED && info->portable) {
      if (!info->portable(info->portable_context, &module.pCode, &module.codeSize)) {
         result = VK_ERROR_UNKNOWN;
      } else {
         result = vk_pipeline_compile_native_stage(&dev->vk, info->pipeline_flags,
                    info->pipeline_pnext, &stage, info->layout,
                    info->stage == VK_SHADER_STAGE_COMPUTE_BIT ? NULL : &state,
                    NULL, NULL, &native_used, &shader);
      }
   }
   switch_nvk_const_push_current = NULL;
   switch_nvk_ubo_fold_current = NULL;
   switch_nvk_half_targets_current = 0;
   switch_nvk_split_current = false;
   switch_nvk_center_frag_depth_current = false;
   switch_nvk_texs_f16_current = false;
   switch_nvk_turned_current = false;
   memset(switch_nvk_xr_plane_bindings_current, 0, sizeof(switch_nvk_xr_plane_bindings_current));
   if (result != VK_SUCCESS)
      return result;

   switch_nvk_stage_of(container_of(shader, struct nvk_shader, vk), out);
   out->owner = shader;
   out->semantic_version = native_used ? info->semantic->version : 0;
   return VK_SUCCESS;
}

SWITCH_EXPORT void
switch_nvk_stage_finish(VkDevice _device, struct switch_nvk_stage *stage)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   if (stage->owner != NULL)
      vk_shader_destroy(&dev->vk, stage->owner, &dev->vk.alloc);
   stage->owner = NULL;
}
