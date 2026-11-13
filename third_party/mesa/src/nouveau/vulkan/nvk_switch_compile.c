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
#include "nvk_switch_turn.h"
#include "nvk_switch_xr_planes.h"

#include "nvk_descriptor_types.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_split.h"
#include "nvk_switch_ir.h"

#include "vk_graphics_state.h"
#include "vk_pipeline.h"
#include "vk_shader.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

_Thread_local bool switch_nvk_center_frag_depth_current;
_Thread_local bool switch_nvk_texs_f16_current;

static void
qmd_inputs(const struct nak_shader_info *info, struct switch_nvk_stage *out)
{
   out->slm_size = info->slm_size;
   out->crs_size = info->crs_size;
   if (info->stage != MESA_SHADER_COMPUTE)
      return;
   out->num_barriers = info->num_control_barriers;
   memcpy(out->local_size, info->cs.local_size, sizeof(out->local_size));
   out->smem_size = info->cs.smem_size;
}

static nir_shader *
build_semantic(const void *program,
                const struct nir_shader_compiler_options *options,
                const struct spirv_to_nir_options *address_options,
                const VkSpecializationInfo *specialization)
{
   return switch_nvk_ir_to_nir(program, options, address_options, specialization);
}

void
switch_nvk_stage_of(const struct nvk_shader *shader,
                    struct switch_nvk_stage *out)
{
   memcpy(out->header, shader->info.hdr, sizeof(out->header));
   out->code = shader->code_ptr;
   out->code_bytes = shader->code_size;
   out->data = shader->data_ptr;
   out->data_bytes = shader->data_size;
   out->cbuf_count = MIN2(shader->cbuf_map.cbuf_count, 16);
   for (uint32_t c = 0; c < out->cbuf_count; c++) {
      out->cbuf_types[c] = switch_nvk_cbuf_code(&shader->cbuf_map.cbufs[c]);
      out->cbuf_sets[c] = shader->cbuf_map.cbufs[c].desc_set;
   }
   qmd_inputs(&shader->info, out);
   out->num_gprs = shader->info.num_gprs;
   out->bound_textures = shader->info.bound_textures;
   out->split = shader->switch_split;
   if (shader->info.stage == MESA_SHADER_FRAGMENT) {
      out->early_fragment_tests = shader->info.fs.early_fragment_tests;
      out->writes_depth = shader->info.fs.writes_depth;
      out->post_depth_coverage = shader->info.fs.post_depth_coverage;
      out->uses_sample_shading = shader->info.fs.uses_sample_shading || shader->sample_shading_enable;
      const unsigned mode = nvk_fs_interlock_mode(shader->info.fs.interlock_mode,
                                                  out->uses_sample_shading);
      out->interlock_control = mode | (shader->info.fs.interlock_unordered ? 8 : 0);
   }
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
   switch_nvk_half_targets_current = info->half_targets;
   struct vk_shader *shader = NULL;
   switch_nvk_split_current = info->split;
   switch_nvk_center_frag_depth_current = info->center_frag_depth != 0;
   switch_nvk_texs_f16_current = info->texs_f16;
   switch_nvk_turned_current = info->turned;
   switch_nvk_xr_plane_bindings_current = info->xr_plane_bindings;
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
   switch_nvk_half_targets_current = 0;
   switch_nvk_split_current = false;
   switch_nvk_center_frag_depth_current = false;
   switch_nvk_texs_f16_current = false;
   switch_nvk_turned_current = false;
   switch_nvk_xr_plane_bindings_current = 0;
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

static void
split_program(const struct switch_nvk_program *p, struct switch_nvk_stage *out)
{
   out->code = p->code;
   out->code_bytes = p->code_bytes;
   out->data = p->data;
   out->data_bytes = p->data_bytes;
   out->cbuf_count = MIN2(p->cbuf_count, 16);
   memcpy(out->cbuf_types, p->cbuf_types, sizeof(out->cbuf_types));
   out->num_gprs = p->info.num_gprs;
   out->bound_textures = p->info.bound_textures;
   qmd_inputs(&p->info, out);
}

SWITCH_EXPORT void
switch_nvk_split_parts(const struct switch_nvk_split *split,
                       struct switch_nvk_split_parts *out)
{
   memset(out, 0, sizeof(*out));
   split_program(&split->fs, &out->fs);
   memcpy(out->fs.header, split->fs.info.hdr, sizeof(out->fs.header));
   out->fs.early_fragment_tests = split->fs.info.fs.early_fragment_tests;
   out->fs.writes_depth = split->fs.info.fs.writes_depth;
   out->fs.post_depth_coverage = split->fs.info.fs.post_depth_coverage;
   out->fs.uses_sample_shading = split->fs.info.fs.uses_sample_shading;
   out->fs.interlock_control =
      nvk_fs_interlock_mode(split->fs.info.fs.interlock_mode,
                            split->fs.info.fs.uses_sample_shading) |
      (split->fs.info.fs.interlock_unordered ? 8 : 0);
   split_program(&split->prologue, &out->prologue);
   out->layout = split->layout;
   memcpy(out->qmd, split->qmd, sizeof(out->qmd));
   out->threshold_pixels = split->threshold_pixels;
   out->cost = split->cost;
}

SWITCH_EXPORT void
switch_nvk_compile_abi(uint32_t sizes[3])
{
   sizes[0] = sizeof(struct switch_nvk_stage_info);
   sizes[1] = sizeof(struct switch_nvk_stage);
   sizes[2] = sizeof(struct switch_nvk_split_parts);
}
