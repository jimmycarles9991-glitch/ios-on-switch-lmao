/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2018 Intel Corporation
 * Copyright 2025 Alyssa Rosenzweig
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "nvk_shader.h"
#include "nvk_descriptor_types.h"
#include "nvk_root_table.h"
#include "nvk_switch_round.h"
#include "nvk_switch_split.h"
#include "nvk_switch_stage.h"
#include "nvk_switch_turn.h"
#include "nvk_switch_xr_planes.h"

#include "vk_graphics_state.h"
#include "vk_pipeline.h"

#include "nak.h"
#include "nir.h"
#include "compiler/spirv/nir_spirv.h"

const struct nak_constant_offset_info nak_const_offsets_base = {
   .sample_info_cb = 0,
   .sample_locations_offset = nvk_root_descriptor_offset(draw.sample_locations),
   .sample_masks_offset = nvk_root_descriptor_offset(draw.sample_masks),
   .printf_cb = 0,
   .printf_buffer_offset = nvk_root_descriptor_offset(printf_buffer_addr),
   .interlock_ticket_buffer_cb = 0,
   .interlock_ticket_buffer_offset = nvk_root_descriptor_offset(interlock_ticket_buffer),
};

const struct nak_constant_offset_info nak_const_offsets_turing_graphics = {
   .sample_info_cb = NVK_HW_ROOT_TABLE_FIRST_CB +
                     nvk_hw_root_table_index(draw.sample_locations),
   .sample_locations_offset = nvk_hw_root_table_offset(draw.sample_locations),
   .sample_masks_offset = nvk_hw_root_table_offset(draw.sample_masks),
   .printf_cb = NVK_HW_ROOT_TABLE_FIRST_CB +
                nvk_hw_root_table_index(printf_buffer_addr),
   .printf_buffer_offset = nvk_hw_root_table_offset(printf_buffer_addr),
   .interlock_ticket_buffer_cb = NVK_HW_ROOT_TABLE_FIRST_CB +
                                 nvk_hw_root_table_index(interlock_ticket_buffer),
   .interlock_ticket_buffer_offset = nvk_hw_root_table_offset(interlock_ticket_buffer),
};
static_assert(nvk_hw_root_table_index(draw.sample_locations) ==
              nvk_hw_root_table_index(draw.sample_masks),
              "Sample info is in same root table");

struct spirv_to_nir_options
switch_nvk_spirv_options(const struct switch_nvk_target *t,
                         const struct vk_pipeline_robustness_state *rs)
{
   return (struct spirv_to_nir_options) {
      .ssbo_addr_format = nvk_ssbo_addr_format(rs),
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .ubo_addr_format = nvk_ubo_addr_format(t->info, rs),
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ssbo_alignment = t->min_ssbo_alignment,
      .min_ubo_alignment = nvk_min_cbuf_alignment(t->info),
   };
}

void
switch_nvk_preprocess(const struct switch_nvk_target *t, nir_shader *nir)
{
   /* Lower primitive id to varyings to prepare for nir_opt_varyings_bulk */
   struct nir_lower_sysvals_to_varyings_options sysvals_opts = {
      .primitive_id = nir->info.stage == MESA_SHADER_FRAGMENT,
   };
   nir_lower_sysvals_to_varyings(nir, &sysvals_opts);

   nak_preprocess_nir(nir, t->nak);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_input_attachment_options ia_opts = {
         .use_ia_coord_intrin = true,
      };
      NIR_PASS(_, nir, nir_lower_input_attachments, &ia_opts);
   }
}

static void
nvk_populate_fs_key(struct nak_fs_key *key,
                    const struct vk_graphics_pipeline_state *state)
{
   memset(key, 0, sizeof(*key));
   key->center_frag_depth = switch_nvk_center_frag_depth_current;
   key->texs_f16 = switch_nvk_texs_f16_current;
   key->half_targets = (uint8_t)switch_nvk_half_targets_current;
   key->turned = switch_nvk_turned_current;
   for (unsigned i = 0; i < 8 * SWITCH_XR_PLANE_WORDS; i++)
      key->xr_plane_bindings[i] = (uint8_t)(switch_nvk_xr_plane_bindings_current[i / 8] >> (8 * (i % 8)));

   /* Turn underestimate on when no state is availaible or if explicitly set */
   if (state == NULL || state->rs == NULL ||
       state->rs->conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT)
      key->uses_underestimate = true;

   if (state == NULL)
      return;

   if (state->pipeline_flags &
       VK_PIPELINE_CREATE_2_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
      key->zs_self_dep = true;

   /* We force per-sample interpolation whenever sampleShadingEnable is set
    * regardless of minSampleShading or rasterizationSamples.
    *
    * When sampleShadingEnable is set, few guarantees are made about the
    * location of interpolation of the inputs.  The only real guarantees are
    * that the inputs are interpolated within the pixel and that you get at
    * least `rasterizationSamples * minSampleShading` unique positions.
    * Importantly, it does not require that when `rasterizationSamples *
    * minSampleShading <= 1.0` that those positions are at the fragment
    * center.  Therefore, it's valid to just always do per-sample (which maps
    * to CENTROID on NVIDIA hardware) all the time and let the hardware sort
    * it out based on what we set in HYBRID_ANTI_ALIAS_CONTROL::passes.
    *
    * Also, we set HYBRID_ANTI_ALIAS_CONTROL::centroid at draw time based on
    * `rasterizationSamples * minSampleShading` so it should be per-pixel
    * whenever we're running only a single pass.  However, this would still be
    * correct even if it got interpolated at some other sample.
    *
    * The one caveat here is that we have to be careful about gl_SampleMaskIn.
    * When `nak_fs_key::force_sample_shading = true` we also turn any reads of
    * gl_SampleMaskIn into `1 << gl_SampleID` because the hardware sample mask
    * is actually per-fragment, not per-pass.  We handle this by smashing
    * minSampleShading to 1.0 whenever gl_SampleMaskIn is read.
    */
   const struct vk_multisample_state *ms = state->ms;
   if (ms != NULL && ms->sample_shading_enable)
      key->force_sample_shading = true;
}

void
switch_nvk_fs_key(struct nak_fs_key *key,
                  const struct vk_graphics_pipeline_state *state)
{
   nvk_populate_fs_key(key, state);
}

VkResult
switch_nvk_compile_nir(const struct switch_nvk_target *t, nir_shader *nir,
                       VkShaderCreateFlagsEXT shader_flags,
                       const struct vk_pipeline_robustness_state *rs,
                       const struct nak_fs_key *fs_key,
                       struct nvk_shader *shader)
{

   const bool dump_asm =
      shader_flags & VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA;

   const bool has_task_shader =
      (shader_flags & VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT) == 0;

   nir_variable_mode robust2_modes = 0;
   if (rs->uniform_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      robust2_modes |= nir_var_mem_ubo;
   if (rs->storage_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT)
      robust2_modes |= nir_var_mem_ssbo;

   nir_shader *split_src = NULL;
   if (nir->info.stage == MESA_SHADER_FRAGMENT && switch_nvk_split_current)
      split_src = nir_shader_clone(NULL, nir);

   shader->nak = nak_compile_shader(nir, dump_asm, t->nak,
                                    robust2_modes, fs_key, has_task_shader);
   if (!shader->nak) {
      ralloc_free(split_src);
      return VK_ERROR_UNKNOWN;
   }

   shader->info = shader->nak->info;
   shader->asm_str = shader->nak->asm_str;
   shader->code_ptr = shader->nak->code;
   shader->code_size = shader->nak->code_size;

   if (nir->constant_data_size > 0) {
      uint32_t data_align = nvk_min_cbuf_alignment(t->info);
      uint32_t data_size = align(nir->constant_data_size, data_align);

      void *data = malloc(data_size);
      if (data == NULL)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      memcpy(data, nir->constant_data, nir->constant_data_size);

      assert(nir->constant_data_size <= data_size);
      memset(data + nir->constant_data_size, 0,
             data_size - nir->constant_data_size);

      shader->data_ptr = data;
      shader->data_size = data_size;
   }

   if (split_src != NULL) {
      const uint64_t id =
         switch_nvk_program_id(&shader->info, shader->code_ptr,
                               shader->code_size, shader->data_ptr,
                               shader->data_size);
      shader->switch_split =
         switch_nvk_split_compile(t, split_src, &shader->cbuf_map,
                                  robust2_modes, fs_key, &shader->info, id);
      ralloc_free(split_src);
   }

   if (dump_asm)
      shader->nir_str = nir_shader_as_str(nir, NULL);

   return VK_SUCCESS;
}

void
switch_nvk_nir_opts(nir_shader *nir, void *data)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_loop);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);

      NIR_PASS(progress, nir, nir_opt_if, 0);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);

      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){
                  .limit = 8,
                  .discard_ok = true,
               });

      NIR_PASS(progress, nir, nir_opt_phi_precision);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
   } while (progress);
}

void
switch_nvk_varyings(nir_shader *nir)
{
   if (nir->xfb_info) {
      NIR_PASS(_, nir, nir_opt_constant_folding);
      nir_io_add_intrinsic_xfb_info(nir);
   }
   nir_opt_varyings_bulk(&nir, 1, true, UINT32_MAX, UINT32_MAX, switch_nvk_nir_opts, NULL);
}
