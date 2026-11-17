/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2018 Intel Corporation
 * Copyright 2025 Alyssa Rosenzweig
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "nvk_shader.h"
#include "nvk_debug.h"
#include "nvk_descriptor_types.h"
#include "nvk_root_table.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_turn.h"
#include "nvk_switch_ubo_fold.h"
#include "nvk_switch_xr_planes.h"

#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"

#include "nak.h"
#include "nir.h"
#include "nir_builder.h"

#include "cla097.h"
#include "clb097.h"
#include "clc597.h"

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

nir_address_format
nvk_ubo_addr_format(const struct nv_device_info *info,
                    const struct vk_pipeline_robustness_state *rs)
{
   if (nvk_use_bindless_cbuf(info)) {
      return nir_address_format_vec2_index_32bit_offset;
   } else if (rs->null_uniform_buffer_descriptor) {
      /* We need bounds checking for null descriptors */
      return nir_address_format_64bit_bounded_global;
   } else {
      switch (rs->uniform_buffers) {
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT:
         return nir_address_format_64bit_global_32bit_offset;
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT:
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT:
         return nir_address_format_64bit_bounded_global;
      default:
         UNREACHABLE("Invalid robust buffer access behavior");
      }
   }
}

nir_address_format
nvk_ssbo_addr_format(const struct vk_pipeline_robustness_state *rs)
{
   if (rs->null_storage_buffer_descriptor) {
      /* We need bounds checking for null descriptors */
      return nir_address_format_64bit_bounded_global;
   } else {
      switch (rs->storage_buffers) {
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT:
         return nir_address_format_64bit_global_32bit_offset;
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT:
      case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT:
         return nir_address_format_64bit_bounded_global;
      default:
         UNREACHABLE("Invalid robust buffer access behavior");
      }
   }
}

static bool
lower_load_intrinsic(nir_builder *b, nir_intrinsic_instr *load,
                     UNUSED void *data)
{
   const struct nv_device_info *info = data;

   switch (load->intrinsic) {
   case nir_intrinsic_load_ubo: {
      b->cursor = nir_before_instr(&load->instr);

      nir_def *index = load->src[0].ssa;
      nir_def *offset = load->src[1].ssa;
      const enum gl_access_qualifier access = nir_intrinsic_access(load);
      const uint32_t align_mul = nir_intrinsic_align_mul(load);
      const uint32_t align_offset = nir_intrinsic_align_offset(load);

      nir_def *val;
      if (load->src[0].ssa->num_components == 1) {
         val = nir_ldc_nv(b, load->num_components, load->def.bit_size,
                           index, offset, .access = access,
                           .align_mul = align_mul,
                           .align_offset = align_offset);
      } else if (load->src[0].ssa->num_components == 2) {
         nir_def *handle = nir_pack_64_2x32(b, load->src[0].ssa);
         val = nir_ldcx_nv(b, load->num_components, load->def.bit_size,
                           handle, offset, .access = access,
                           .align_mul = align_mul,
                           .align_offset = align_offset);
      } else {
         UNREACHABLE("Invalid UBO index");
      }
      nir_def_rewrite_uses(&load->def, val);
      return true;
   }

   case nir_intrinsic_load_global_constant_bounded:
      /* Handled inside nak_nir_lower_load_store */
      if (info->sm >= 73)
         return false;
      FALLTHROUGH;
   case nir_intrinsic_load_global_constant_offset: {
      b->cursor = nir_before_instr(&load->instr);

      nir_def *base_addr = load->src[0].ssa;
      nir_def *offset = load->src[1].ssa;

      nir_def *zero = NULL;
      if (load->intrinsic == nir_intrinsic_load_global_constant_bounded) {
         nir_def *bound = load->src[2].ssa;

         unsigned bit_size = load->def.bit_size;
         assert(bit_size >= 8 && bit_size % 8 == 0);
         unsigned byte_size = bit_size / 8;

         zero = nir_imm_zero(b, load->num_components, bit_size);

         unsigned load_size = byte_size * load->num_components;

         nir_def *sat_offset =
            nir_umin(b, offset, nir_imm_int(b, UINT32_MAX - (load_size - 1)));
         nir_def *in_bounds =
            nir_ult(b, nir_iadd_imm(b, sat_offset, load_size - 1), bound);

         nir_push_if(b, in_bounds);
      }

      nir_def *val =
         nir_load_global_constant(b, load->def.num_components,
                                        load->def.bit_size,
                                        nir_iadd(b, base_addr, nir_u2u64(b, offset)),
                                        .align_mul = nir_intrinsic_align_mul(load),
                                        .align_offset = nir_intrinsic_align_offset(load));

      if (load->intrinsic == nir_intrinsic_load_global_constant_bounded) {
         nir_pop_if(b, NULL);
         val = nir_if_phi(b, val, zero);
      }

      nir_def_rewrite_uses(&load->def, val);
      return true;
   }

   default:
      return false;
   }
}

void
switch_nvk_lower(const struct switch_nvk_target *t, nir_shader *nir,
                 VkShaderCreateFlagsEXT shader_flags,
                 const struct vk_pipeline_robustness_state *rs,
                 uint32_t set_layout_count,
                 struct vk_descriptor_set_layout * const *set_layouts,
                 nir_vk_ycbcr_conversion_lookup_cb ycbcr,
                 const void *ycbcr_data,
                 struct nvk_cbuf_map *cbuf_map_out)
{

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS(_, nir, nir_lower_patch_vertices,
               nir->info.tess.tcs_vertices_out, NULL);
   }

   if (switch_nvk_turned_current)
      NIR_PASS(_, nir, switch_nvk_turn_fragment, nvk_root_descriptor_offset(switch_turn));

   NIR_PASS(_, nir, switch_nvk_xr_planes, switch_nvk_xr_plane_bindings_current);

   NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, ycbcr, ycbcr_data);

   nir_lower_compute_system_values_options csv_options = {
      .has_base_workgroup_id = mesa_shader_stage_is_compute(nir->info.stage),
      .lower_local_invocation_index = mesa_shader_stage_is_compute(nir->info.stage),
      .lower_workgroup_id_to_index = mesa_shader_stage_is_mesh(nir->info.stage),
      .lower_cs_local_id_to_index = mesa_shader_stage_is_mesh(nir->info.stage),
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   /* Lower push constants before lower_descriptors */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, switch_nvk_fold_const_push,
            switch_nvk_const_push_current);
   NIR_PASS(_, nir, switch_nvk_round_half_outputs,
            switch_nvk_half_targets_current);

   struct nvk_cbuf_map *cbuf_map = NULL;
   if (!(t->debug_flags & NVK_DEBUG_NO_CBUF)) {
      cbuf_map = cbuf_map_out;

      /* Large constant support assumes cbufs */
      /* Needs to run before load_const_to_scalar */
      NIR_PASS(_, nir, nir_opt_large_constants, NULL, 32);
   } else {
      *cbuf_map_out = (struct nvk_cbuf_map) {
         .cbuf_count = 1,
         .cbufs = {
            { .type = NVK_CBUF_TYPE_ROOT_DESC },
         }
      };
   }

   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);

   nir_opt_access_options opt_access_options = {
      .is_vulkan = true,
   };
   NIR_PASS(_, nir, nir_opt_access, &opt_access_options);

   /* On Kepler, we have to lower images to addresses */
   if (t->info->cls_eng3d < MAXWELL_A)
      NIR_PASS(_, nir, nak_nir_lower_image_addrs, t->nak);

   NIR_PASS(_, nir, nvk_nir_lower_descriptors, t->info,
            t->debug_flags & NVK_DEBUG_FORCE_EDB_BVIEW, shader_flags, rs,
            set_layout_count, set_layouts, cbuf_map);

   if (nvk_use_bindless_cbuf(t->info)) {
      /* On Turing+ where we have bindless cbufs, we use ACCESS_NON_UNIFORM to
       * determine whether or not it's safe to assume a uniform handle so we
       * want to optimize it away whenever possible.
       */
      if (nir_has_non_uniform_access(nir, nir_lower_non_uniform_ubo_access))
         NIR_PASS(_, nir, nir_opt_non_uniform_access);
   }

   if (t->info->cls_eng3d < TURING_A) {
      /* NOTE: This does nothing for images on Kepler since those are lowered
       * to suldga/sustga before we get here.  That's fine, though, because
       * our nil_su_info fetches and calculations work fine with non-uniform
       * descriptors.
       */
      struct nir_lower_non_uniform_access_options opts = {
         .types = nir_lower_non_uniform_texture_access |
                  nir_lower_non_uniform_texture_query |
                  nir_lower_non_uniform_image_access |
                  nir_lower_non_uniform_image_query,
         .callback = NULL,
      };
      /* In practice, most shaders do not have non-uniform-qualified accesses
       * thus a cheaper and likely to fail check is run first.
       */
      if (nir_has_non_uniform_access(nir, opts.types)) {
         NIR_PASS(_, nir, nir_opt_non_uniform_access);
         NIR_PASS(_, nir, nir_lower_non_uniform_access, &opts);
      }
   }

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            nvk_ssbo_addr_format(rs));
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            nvk_ubo_addr_format(t->info, rs));
   NIR_PASS(_, nir, switch_nvk_fold_ubo, cbuf_map,
            switch_nvk_ubo_fold_current);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass,
            lower_load_intrinsic, nir_metadata_none, (void *)t->info);

   if (mesa_shader_stage_uses_workgroup(nir->info.stage)) {
      nir_variable_mode var_modes = nir_var_mem_shared;

      if (mesa_shader_stage_is_mesh(nir->info.stage))
         var_modes |= nir_var_mem_task_payload;

      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, var_modes,
               shared_var_info);
      NIR_PASS(_, nir, nir_lower_explicit_io, var_modes,
               nir_address_format_32bit_offset);
   }

   nak_lower_nir_before_linking(nir, t->nak);
   nir->info.io_lowered = true;
}

void
switch_nvk_lower_late(nir_shader *nir, VkShaderCreateFlagsEXT shader_flags)
{
   if (mesa_shader_stage_uses_workgroup(nir->info.stage)) {
      if (nir->info.stage == MESA_SHADER_TASK)
         NIR_PASS(_, nir, nvk_nir_lower_task_shader);
      else if (nir->info.stage == MESA_SHADER_MESH)
         NIR_PASS(_, nir, nvk_nir_lower_mesh_shader, shader_flags);

      if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {
         uint32_t alignment;
         uint32_t chunk_size;

         if (mesa_shader_stage_is_mesh(nir->info.stage)) {
            /* With task/mesh shaders, shared is in ISBE attribute space and is
             * allocated in "lines" of 128 bytes. Additionally, we ISBE I/O
             * instructions only support 1B and 4B granualities.*/
            alignment = 128;
            chunk_size = 4;
         } else {
            /* QMD::SHARED_MEMORY_SIZE requires an alignment of 256B so it's
             * safe to align everything up to 16B so we can write whole vec4s.
             */
            alignment = 16;
            chunk_size = 16;
         }

         nir->info.shared_size = align(nir->info.shared_size, alignment);
         NIR_PASS(_, nir, nir_zero_initialize_shared_memory,
                  nir->info.shared_size, chunk_size);

         /* We need to call lower_compute_system_values again because
         * nir_zero_initialize_shared_memory generates load_invocation_id which
         * has to be lowered to load_invocation_index.
         */
         nir_lower_compute_system_values_options csv_options = {
            .lower_local_invocation_index = mesa_shader_stage_is_compute(nir->info.stage),
         };
         NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);
      }
   }
}
