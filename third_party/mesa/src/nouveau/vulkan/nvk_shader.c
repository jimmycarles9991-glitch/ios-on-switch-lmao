/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2018 Intel Corporation
 * Copyright 2025 Alyssa Rosenzweig
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "nvk_shader.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_turn.h"
#include "nvk_switch_xr_planes.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_compile.h"
#include "nvk_switch_split.h"

#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_instance.h"
#include "nvk_mme.h"
#include "nvk_physical_device.h"
#include "nvk_sampler.h"
#include "nvk_shader.h"

#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"
#include "vk_ycbcr_conversion.h"

#include "nak.h"
#include "nir.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"

#include "util/mesa-blake3.h"
#include "util/u_debug.h"

#include "cla097.h"
#include "clb097.h"
#include "clc597.h"
#include "nv_push_cl9097.h"
#include "nv_push_clb197.h"
#include "nv_push_clc397.h"
#include "nv_push_clc597.h"
#include "nv_push_clc797.h"

uint64_t
nvk_physical_device_compiler_flags(const struct nvk_physical_device *pdev)
{
   const struct nvk_instance *instance = nvk_physical_device_instance(pdev);
   bool no_cbufs = pdev->debug_flags & NVK_DEBUG_NO_CBUF;
   bool use_edb_buffer_views = nvk_use_edb_buffer_views(pdev);
   uint64_t nak_flags = nak_debug_flags(pdev->nak);

   assert(nak_flags <= UINT16_MAX);

   return (no_cbufs ? 1 << 12 : 0)
      | (use_edb_buffer_views ? 1 << 13 : 0)
      | (instance->drirc.misc.ssbo_align_4b ? 1 << 14 : 0)
      | (nak_flags << 48);
}

static const nir_shader_compiler_options *
nvk_get_nir_options(struct vk_physical_device *vk_pdev,
                    mesa_shader_stage stage,
                    UNUSED const struct vk_pipeline_robustness_state *rs)
{
   const struct nvk_physical_device *pdev =
      container_of(vk_pdev, struct nvk_physical_device, vk);
   return nak_nir_options(pdev->nak);
}

struct switch_nvk_target
switch_nvk_target_of(const struct nvk_physical_device *pdev)
{
   return (struct switch_nvk_target) {
      .info = &pdev->info,
      .nak = pdev->nak,
      .debug_flags = pdev->debug_flags,
      .min_ssbo_alignment =
         nvk_min_ssbo_alignment(nvk_physical_device_instance(pdev)),
   };
}

static struct spirv_to_nir_options
nvk_get_spirv_options(struct vk_physical_device *vk_pdev,
                      UNUSED mesa_shader_stage stage,
                      const struct vk_pipeline_robustness_state *rs)
{
   const struct nvk_physical_device *pdev =
      container_of(vk_pdev, struct nvk_physical_device, vk);
   const struct switch_nvk_target t = switch_nvk_target_of(pdev);
   return switch_nvk_spirv_options(&t, rs);
}

static void
nvk_preprocess_nir(struct vk_physical_device *vk_pdev,
                   nir_shader *nir,
                   UNUSED const struct vk_pipeline_robustness_state *rs)
{
   const struct nvk_physical_device *pdev =
      container_of(vk_pdev, struct nvk_physical_device, vk);
   const struct switch_nvk_target t = switch_nvk_target_of(pdev);
   switch_nvk_preprocess(&t, nir);
}

static void
nvk_hash_state(struct vk_physical_device *device,
               const struct vk_graphics_pipeline_state *state,
               const struct vk_features *enabled_features,
               VkShaderStageFlags stages,
               blake3_hash blake3_out)
{
   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);
   if (state && (stages & VK_SHADER_STAGE_FRAGMENT_BIT)) {
      struct nak_fs_key key;
      switch_nvk_fs_key(&key, state);
      _mesa_blake3_update(&blake3_ctx, &key, sizeof(key));

      /* This doesn't impact the shader compile but it does go in the
       * nvk_shader and gets [de]serialized along with the binary so we
       * need to hash it.
       */
      if (state->ms && state->ms->sample_shading_enable) {
         _mesa_blake3_update(&blake3_ctx, &state->ms->min_sample_shading,
                             sizeof(state->ms->min_sample_shading));
      }
   }
   _mesa_blake3_final(&blake3_ctx, blake3_out);
}

struct lower_ycbcr_state {
   uint32_t set_layout_count;
   struct vk_descriptor_set_layout * const *set_layouts;
};

static const struct vk_ycbcr_conversion_state *
lookup_ycbcr_conversion(const void *_state, uint32_t set,
                        uint32_t binding, uint32_t array_index)
{
   const struct lower_ycbcr_state *state = _state;
   assert(set < state->set_layout_count);
   assert(state->set_layouts[set] != NULL);
   const struct nvk_descriptor_set_layout *set_layout =
      vk_to_nvk_descriptor_set_layout(state->set_layouts[set]);
   assert(binding < set_layout->binding_count);

   const struct nvk_descriptor_set_binding_layout *bind_layout =
      &set_layout->binding[binding];

   if (bind_layout->immutable_samplers == NULL)
      return NULL;

   array_index = MIN2(array_index, bind_layout->array_size - 1);

   const struct nvk_sampler *sampler =
      bind_layout->immutable_samplers[array_index];

   return sampler && sampler->vk.ycbcr_conversion ?
          &sampler->vk.ycbcr_conversion->state : NULL;
}

static void
nvk_lower_nir(struct nvk_device *dev, nir_shader *nir,
              VkShaderCreateFlagsEXT shader_flags,
              const struct vk_pipeline_robustness_state *rs,
              uint32_t set_layout_count,
              struct vk_descriptor_set_layout * const *set_layouts,
              struct nvk_cbuf_map *cbuf_map_out)
{
   const struct switch_nvk_target t =
      switch_nvk_target_of(nvk_device_physical(dev));
   const struct lower_ycbcr_state ycbcr_state = {
      .set_layout_count = set_layout_count,
      .set_layouts = set_layouts,
   };
   switch_nvk_lower(&t, nir, shader_flags, rs, set_layout_count, set_layouts,
                    lookup_ycbcr_conversion, &ycbcr_state, cbuf_map_out);
}

#ifndef NDEBUG
static void
nvk_shader_dump(struct nvk_shader *shader)
{
   unsigned pos;

   if (shader->info.stage != MESA_SHADER_COMPUTE) {
      _debug_printf("dumping HDR for %s shader\n",
                    _mesa_shader_stage_to_string(shader->info.stage));
      for (pos = 0; pos < ARRAY_SIZE(shader->info.hdr); ++pos)
         _debug_printf("HDR[%02"PRIxPTR"] = 0x%08x\n",
                      pos * sizeof(shader->info.hdr[0]), shader->info.hdr[pos]);
   }
   _debug_printf("shader binary code (0x%x bytes):", shader->code_size);
   for (pos = 0; pos < shader->code_size / 4; ++pos) {
      if ((pos % 8) == 0)
         _debug_printf("\n");
      _debug_printf("%08x ", ((const uint32_t *)shader->code_ptr)[pos]);
   }
   _debug_printf("\n");
}
#endif

static VkResult
nvk_compile_nir(struct nvk_device *dev, nir_shader *nir,
                VkShaderCreateFlagsEXT shader_flags,
                const struct vk_pipeline_robustness_state *rs,
                const struct nak_fs_key *fs_key,
                struct nvk_shader *shader)
{
   const struct switch_nvk_target t =
      switch_nvk_target_of(nvk_device_physical(dev));
   return switch_nvk_compile_nir(&t, nir, shader_flags, rs, fs_key, shader);
}

static uint32_t
nvk_shader_get_hdr_size(struct nvk_device *dev, struct nvk_shader *shader)
{
   if (shader->info.stage == MESA_SHADER_COMPUTE)
      return 0;

   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   return pdev->info.cls_eng3d >= TURING_A ? TU102_SHADER_HEADER_SIZE
                                           : GF100_SHADER_HEADER_SIZE;
}

static uint32_t
nvk_shader_get_shader_alignment(struct nvk_device *dev)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   /* Fermi   needs 0x40 alignment
    * Kepler+ needs the first instruction to be 0x80 aligned, so we waste 0x30 bytes
    */
   return pdev->info.cls_eng3d >= KEPLER_A ? 0x80 : 0x40;
}

static uint32_t
nvk_shader_get_shader_size(struct nvk_device *dev, struct nvk_shader *shader,
                           uint32_t *out_hdr_offset, uint32_t *out_code_offset,
                           uint32_t *out_gs_hdr_offset)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const uint32_t hdr_size = nvk_shader_get_hdr_size(dev, shader);
   const uint32_t alignment = nvk_shader_get_shader_alignment(dev);

   uint32_t total_size = 0;
   if (pdev->info.cls_eng3d >= KEPLER_A &&
       pdev->info.cls_eng3d < TURING_A &&
       hdr_size > 0) {
      /* The instructions are what has to be aligned so we need to start at a
       * small offset (0x30 B) into the upload area.
       */
      total_size = alignment - hdr_size;
   }

   if (out_hdr_offset)
      *out_hdr_offset = total_size;

   total_size += hdr_size;

   if (out_code_offset) {
      *out_code_offset = total_size;
      assert(*out_code_offset % alignment == 0);
   }

   total_size += shader->code_size;

   const bool has_mesh_gs_sph = shader->info.stage == MESA_SHADER_MESH &&
                                shader->info.mesh.has_gs_sph;

   if (out_gs_hdr_offset)
      *out_gs_hdr_offset = 0;
   if (has_mesh_gs_sph) {
      total_size = align(total_size, nvk_min_cbuf_alignment(&pdev->info));
      if (out_gs_hdr_offset)
         *out_gs_hdr_offset = total_size;
      total_size += hdr_size;
   }

   return total_size;
}

static VkResult
nvk_shader_upload(struct nvk_device *dev, struct nvk_shader *shader)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   const bool has_mesh_gs_sph = shader->info.stage == MESA_SHADER_MESH &&
                                shader->info.mesh.has_gs_sph;
   const uint32_t hdr_size = nvk_shader_get_hdr_size(dev, shader);
   uint32_t hdr_offset;
   uint32_t code_offset;
   uint32_t gs_hdr_offset;

   uint32_t total_size = nvk_shader_get_shader_size(dev, shader, &hdr_offset,
                                                    &code_offset, &gs_hdr_offset);
   uint32_t alignment = nvk_shader_get_shader_alignment(dev);

   uint32_t data_offset = 0;
   if (shader->data_size > 0) {
      uint32_t cbuf_alignment = nvk_min_cbuf_alignment(&pdev->info);
      alignment = MAX2(alignment, cbuf_alignment);
      total_size = align(total_size, cbuf_alignment);
      data_offset = total_size;
      total_size += shader->data_size;
   }

   char *data = malloc(total_size);
   if (data == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(hdr_size <= sizeof(shader->info.hdr));
   memcpy(data + hdr_offset, shader->info.hdr, hdr_size);
   memcpy(data + code_offset, shader->code_ptr, shader->code_size);
   if (has_mesh_gs_sph)
      memcpy(data + gs_hdr_offset, shader->info.mesh.gs_hdr, hdr_size);
   if (shader->data_size > 0)
      memcpy(data + data_offset, shader->data_ptr, shader->data_size);

#ifndef NDEBUG
   if (debug_get_bool_option("NV50_PROG_DEBUG", false))
      nvk_shader_dump(shader);
#endif

   VkResult result = nvk_heap_upload(dev, &dev->shader_heap, data,
                                     total_size, alignment,
                                     &shader->upload_addr);
   if (result == VK_SUCCESS) {
      shader->upload_size = total_size;

      shader->hdr_addr = shader->upload_addr + hdr_offset;
      if (pdev->info.cls_eng3d < VOLTA_A) {
         const uint64_t heap_base_addr =
            nvk_heap_contiguous_base_address(&dev->shader_heap);
         assert(shader->upload_addr - heap_base_addr < UINT32_MAX);
         shader->hdr_addr -= heap_base_addr;
      }
      shader->gs_hdr_addr = shader->upload_addr + gs_hdr_offset;
      shader->data_addr = shader->upload_addr + data_offset;
      switch_nvk_program_uploaded(dev, shader);
   }
   free(data);

   return result;
}

uint32_t
mesa_to_nv9097_shader_type(mesa_shader_stage stage, bool has_task_shader)
{
   if (stage == MESA_SHADER_MESH && !has_task_shader)
      stage = MESA_SHADER_TASK;

   static const uint32_t mesa_to_nv9097[] = {
      [MESA_SHADER_VERTEX]    = NV9097_SET_PIPELINE_SHADER_TYPE_VERTEX,
      [MESA_SHADER_TESS_CTRL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION_INIT,
      [MESA_SHADER_TESS_EVAL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION,
      [MESA_SHADER_GEOMETRY]  = NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY,
      [MESA_SHADER_FRAGMENT]  = NV9097_SET_PIPELINE_SHADER_TYPE_PIXEL,
      [MESA_SHADER_TASK]      = NV9097_SET_PIPELINE_SHADER_TYPE_VERTEX,
      [MESA_SHADER_MESH]      = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION,
   };
   assert(stage < ARRAY_SIZE(mesa_to_nv9097));
   return mesa_to_nv9097[stage];
}

static uint32_t
mesa_to_nvb197_interlock_mode(enum nak_fs_interlock_mode mode)
{
   STATIC_ASSERT(NAK_FS_INTERLOCK_MODE_DISABLED ==
                 NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL_TILE_COALESCER_MODE_NO_CONFLICT_DETECT);
   STATIC_ASSERT(NAK_FS_INTERLOCK_MODE_SAMPLE ==
                 NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL_TILE_COALESCER_MODE_CONFLICT_DETECT_SAMPLE);
   STATIC_ASSERT(NAK_FS_INTERLOCK_MODE_PIXEL ==
                 NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL_TILE_COALESCER_MODE_CONFLICT_DETECT_PIXEL);
   return mode;
}

uint32_t
nvk_pipeline_bind_group(mesa_shader_stage stage, bool has_task_shader)
{
   if (stage == MESA_SHADER_MESH && !has_task_shader)
      return MESA_SHADER_VERTEX;
   else if (stage == MESA_SHADER_MESH)
      return MESA_SHADER_TESS_EVAL;
   else if (stage == MESA_SHADER_TASK)
      return MESA_SHADER_VERTEX;

   return stage;
}

uint16_t
nvk_max_shader_push_dw(const struct nvk_physical_device *pdev,
                       mesa_shader_stage stage, bool last_vtgm)
{
   if (stage == MESA_SHADER_COMPUTE)
      return 0;

   uint16_t max_dw_count = 9;

   if (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TASK ||
       stage == MESA_SHADER_MESH)
      max_dw_count += 2;

   if (stage == MESA_SHADER_TESS_CTRL || stage == MESA_SHADER_TESS_EVAL)
      max_dw_count += 2;

   if (stage == MESA_SHADER_FRAGMENT)
      max_dw_count += 17;

   if (stage == MESA_SHADER_TASK)
      max_dw_count += 2;

   if (stage == MESA_SHADER_MESH)
      max_dw_count += 15;

   if (last_vtgm) {
      max_dw_count += 8;
      max_dw_count += 4 * (5 + (128 / 4));
   }

   return max_dw_count;
}

VkResult
nvk_shader_fill_push(struct nvk_device *dev,
                     struct nvk_shader *shader,
                     const VkAllocationCallbacks* pAllocator)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   ASSERTED uint16_t max_dw_count = 0;
   uint32_t push_dw[200];
   struct nv_push push, *p = &push;
   nv_push_init(&push, push_dw, ARRAY_SIZE(push_dw),
                nvk_queue_subchannels_from_engines(NVKMD_ENGINE_3D));

   bool has_task_shader = shader->info.stage == MESA_SHADER_MESH &&
                          shader->info.mesh.has_task_shader;
   const uint32_t type =
      mesa_to_nv9097_shader_type(shader->info.stage, has_task_shader);

   /* We always map index == type */
   const uint32_t idx = type;

   max_dw_count += 2;
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(idx), {
      .enable  = ENABLE_TRUE,
      .type    = type,
   });

   max_dw_count += 4;
   uint64_t addr = shader->hdr_addr;
   if (pdev->info.cls_eng3d >= VOLTA_A) {
      P_MTHD(p, NVC397, SET_PIPELINE_PROGRAM_ADDRESS_A(idx));
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_A(p, idx, addr >> 32);
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_B(p, idx, addr);

      /* On Ampere B and later, we can be prefetched for up to 127 blocks of a
       * shader. */
      if (pdev->info.cls_eng3d >= AMPERE_B) {
         uint32_t shader_size =
            nvk_shader_get_shader_size(dev, shader, NULL, NULL, NULL);
         uint32_t shader_prefetch_size_in_blocks =
            MIN2(DIV_ROUND_UP(shader_size, 256), 127);
         P_NVC797_SET_PIPELINE_PROGRAM_PREFETCH(p, idx,
                                                shader_prefetch_size_in_blocks);
      }
   } else {
      assert(addr < 0xffffffff);
      P_IMMD(p, NV9097, SET_PIPELINE_PROGRAM(idx), addr);
   }

   max_dw_count += 3;
   P_MTHD(p, NVC397, SET_PIPELINE_REGISTER_COUNT(idx));
   P_NVC397_SET_PIPELINE_REGISTER_COUNT(p, idx, shader->info.num_gprs);
   P_NVC397_SET_PIPELINE_BINDING(p, idx,
      nvk_pipeline_bind_group(shader->info.stage, has_task_shader));

   if (shader->info.stage == MESA_SHADER_TESS_CTRL ||
       shader->info.stage == MESA_SHADER_TESS_EVAL) {
      max_dw_count += 2;
      P_1INC(p, NVB197, CALL_MME_MACRO(NVK_MME_SET_TESS_PARAMS));
      P_INLINE_DATA(p, nvk_mme_tess_params(shader->info.stage,
                                           shader->info.ts.domain,
                                           shader->info.ts.spacing,
                                           shader->info.ts.ccw,
                                           shader->info.ts.point_mode));
   }

   bool could_be_first_stage = shader->info.stage == MESA_SHADER_VERTEX ||
                               shader->info.stage == MESA_SHADER_TASK ||
                               shader->info.stage == MESA_SHADER_MESH;
   bool is_first_stage =
      could_be_first_stage && (shader->info.stage != MESA_SHADER_MESH ||
                               !shader->info.mesh.has_task_shader);

   if (could_be_first_stage) {
      max_dw_count += 2;

      if (pdev->info.cls_eng3d >= TURING_A && is_first_stage)
         P_IMMD(p, NVC597, SET_MESH_CONTROL,
                shader->info.stage != MESA_SHADER_VERTEX);
   }

   if (shader->info.stage == MESA_SHADER_TASK) {
      max_dw_count += 2;
      uint16_t smem_lines = DIV_ROUND_UP(shader->info.task.smem_size, 128);
      uint16_t task_smem_lines = DIV_ROUND_UP(shader->info.task.payload_smem_size, 128);

      /* Task payload should be part of shared memory */
      assert(task_smem_lines <= smem_lines);

      P_IMMD(p, NVC597, SET_MESH_INIT_SHADER, {
         .thread_count = shader->info.task.local_size,
         .local_buffer_lines = smem_lines,
         .output_to_m_s_lines = task_smem_lines,
      });
   } else if (shader->info.stage == MESA_SHADER_MESH) {
      max_dw_count += 15;

      assert(shader->info.mesh.max_vertices != 0);
      assert(shader->info.mesh.max_primitives != 0);

      /* On Turing only, if a task+mesh pipeline was previously bound and we
       * bind a mesh only pipeline after it, the hardware will misbehave in
       * TRACK_WITH_FILTER mode and assume that the vertex stage has a task
       * shader instead.
       *
       * NVIDIA proprietary driver apply this workaround on all generations so
       * we also do the same here just in case.
       */
      P_IMMD(p, NV9097, SET_MME_SHADOW_RAM_CONTROL, MODE_METHOD_TRACK);
      P_MTHD(p, NVC597, SET_MESH_SHADER_A);
      P_NVC597_SET_MESH_SHADER_A(p, {
         .output_topology = shader->info.mesh.topology,
         .max_vertex = shader->info.mesh.max_vertices,
         .max_primitive = shader->info.mesh.max_primitives,
      });
      P_NVC597_SET_MESH_SHADER_B(p, {
         .shared_mem_lines = DIV_ROUND_UP(shader->info.mesh.smem_size, 128),
         .thread_count = shader->info.mesh.local_size,
      });
      P_IMMD(p, NV9097, SET_MME_SHADOW_RAM_CONTROL, MODE_METHOD_TRACK_WITH_FILTER);

      if (shader->info.mesh.has_gs_sph) {
         P_IMMD(p, NV9097, SET_PIPELINE_SHADER(NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY), {
            .enable  = shader->info.mesh.has_gs_sph,
            .type    = TYPE_GEOMETRY,
         });

         uint64_t gs_hdr_addr = shader->gs_hdr_addr;
         P_MTHD(p, NVC397, SET_PIPELINE_PROGRAM_ADDRESS_A(NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY));
         P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_A(p, NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY, gs_hdr_addr >> 32);
         P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_B(p, NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY, gs_hdr_addr);
         P_IMMD(p, NVC397, SET_GS_MODE, TYPE_ANY);
      }
   } else if (shader->info.stage == MESA_SHADER_FRAGMENT) {
      max_dw_count += 13;

      P_MTHD(p, NVC397, SET_SUBTILING_PERF_KNOB_A);
      P_NV9097_SET_SUBTILING_PERF_KNOB_A(p, {
         .fraction_of_spm_register_file_per_subtile         = 0x10,
         .fraction_of_spm_pixel_output_buffer_per_subtile   = 0x40,
         .fraction_of_spm_triangle_ram_per_subtile          = 0x16,
         .fraction_of_max_quads_per_subtile                 = 0x20,
      });
      P_NV9097_SET_SUBTILING_PERF_KNOB_B(p, 0x20);

      P_IMMD(p, NV9097, SET_API_MANDATED_EARLY_Z,
             shader->info.fs.early_fragment_tests);

      if (pdev->info.cls_eng3d >= MAXWELL_B) {
         P_IMMD(p, NVB197, SET_POST_Z_PS_IMASK,
                shader->info.fs.post_depth_coverage);
      } else {
         assert(!shader->info.fs.post_depth_coverage);
      }

      P_IMMD(p, NV9097, SET_ZCULL_BOUNDS, {
         .z_min_unbounded_enable = shader->info.fs.writes_depth,
         .z_max_unbounded_enable = shader->info.fs.writes_depth,
      });

      /* NAK's SM50 bound textures read their handles from the constant
       * buffer this names, one for every stage, so only fragment shaders
       * take them
       */
      max_dw_count += 2;
      if (shader->info.bound_textures) {
         P_IMMD(p, NVB197, SET_BINDLESS_TEXTURE,
                shader->info.bound_textures - 1);
      }

      if (pdev->info.cls_eng3d >= MAXWELL_B &&
          nvk_device_fragment_interlock(dev)) {
         max_dw_count += 2;

         const enum nak_fs_interlock_mode coalescer_mode =
            nvk_fs_interlock_mode(shader->info.fs.interlock_mode,
                                  shader->sample_shading_enable ||
                                  shader->info.fs.uses_sample_shading);
         P_IMMD(p, NVB197, SET_PIXEL_SHADER_INTERLOCK_CONTROL, {
            .tile_coalescer_mode = mesa_to_nvb197_interlock_mode(coalescer_mode),
            .tile_coalescer_tile_size = TILE_COALESCER_TILE_SIZE_TC_TILE_SIZE_16X16,
            .tile_coalescer_fragment_order = shader->info.fs.interlock_unordered,
         });
      }

      if (pdev->info.cls_eng3d >= TURING_A) {
         /* From the Vulkan 1.3.297 spec:
          *
          *    "If sample shading is enabled, an implementation must invoke
          *    the fragment shader at least
          *
          *    max( ⌈ minSampleShading × rasterizationSamples ⌉, 1)
          *
          *    times per fragment."
          *
          * The max() here means that, regardless of the actual value of
          * minSampleShading, we need to invoke at least once per pixel,
          * meaning that we need to disable fragment shading rate.  We also
          * need to disable FSR if sample shading is used by the shader.
          */
         P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_SET_SHADING_RATE_CONTROL));
         P_INLINE_DATA(p, nvk_mme_shading_rate_control_sample_shading(
            shader->sample_shading_enable ||
            shader->info.fs.uses_sample_shading));
      }

      float mss = 0;
      if (shader->info.fs.uses_sample_shading) {
         mss = 1;
      } else if (shader->sample_shading_enable) {
         mss = CLAMP(shader->min_sample_shading, 0, 1);
      } else {
         mss = 0;
      }
      P_1INC(p, NVB197, CALL_MME_MACRO(NVK_MME_SET_ANTI_ALIAS));
      P_INLINE_DATA(p, nvk_mme_anti_alias_min_sample_shading(mss));
   }

   /* Stash this before we do XFB and clip/cull */
   shader->push_dw_count = nv_push_dw_count(&push);
   assert(max_dw_count ==
          nvk_max_shader_push_dw(pdev, shader->info.stage, false));

   if (shader->info.stage != MESA_SHADER_FRAGMENT &&
       shader->info.stage != MESA_SHADER_TESS_CTRL) {
      max_dw_count += 8;

      P_IMMD(p, NV9097, SET_RT_LAYER, {
         .v       = 0,
         .control = shader->info.vtg.writes_layer ?
                    CONTROL_GEOMETRY_SHADER_SELECTS_LAYER :
                    CONTROL_V_SELECTS_LAYER,
      });

      if (pdev->info.cls_eng3d >= AMPERE_B) {
         P_IMMD(p, NVC797, SET_VARIABLE_PIXEL_RATE_SHADING_TABLE_SELECT, {
            .source = shader->info.vtg.writes_vprs_table_index ?
                      SOURCE_FROM_VPRS_TABLE_INDEX :
                      SOURCE_FROM_CONSTANT,
            .source_constant_value = 0,
         });
      }

      const uint8_t clip_enable = shader->info.vtg.clip_enable;
      const uint8_t cull_enable = shader->info.vtg.cull_enable;
      P_IMMD(p, NV9097, SET_USER_CLIP_ENABLE, {
         .plane0 = ((clip_enable | cull_enable) >> 0) & 1,
         .plane1 = ((clip_enable | cull_enable) >> 1) & 1,
         .plane2 = ((clip_enable | cull_enable) >> 2) & 1,
         .plane3 = ((clip_enable | cull_enable) >> 3) & 1,
         .plane4 = ((clip_enable | cull_enable) >> 4) & 1,
         .plane5 = ((clip_enable | cull_enable) >> 5) & 1,
         .plane6 = ((clip_enable | cull_enable) >> 6) & 1,
         .plane7 = ((clip_enable | cull_enable) >> 7) & 1,
      });
      P_IMMD(p, NV9097, SET_USER_CLIP_OP, {
         .plane0 = (cull_enable >> 0) & 1,
         .plane1 = (cull_enable >> 1) & 1,
         .plane2 = (cull_enable >> 2) & 1,
         .plane3 = (cull_enable >> 3) & 1,
         .plane4 = (cull_enable >> 4) & 1,
         .plane5 = (cull_enable >> 5) & 1,
         .plane6 = (cull_enable >> 6) & 1,
         .plane7 = (cull_enable >> 7) & 1,
      });

      struct nak_xfb_info *xfb = &shader->info.vtg.xfb;
      for (uint8_t b = 0; b < ARRAY_SIZE(xfb->attr_count); b++) {
         const uint8_t attr_count = xfb->attr_count[b];

         max_dw_count += 5 + (128 / 4);

         P_MTHD(p, NV9097, SET_STREAM_OUT_CONTROL_STREAM(b));
         P_NV9097_SET_STREAM_OUT_CONTROL_STREAM(p, b, xfb->stream[b]);
         P_NV9097_SET_STREAM_OUT_CONTROL_COMPONENT_COUNT(p, b, attr_count);
         P_NV9097_SET_STREAM_OUT_CONTROL_STRIDE(p, b, xfb->stride[b]);

         if (attr_count > 0) {
            /* upload packed varying indices in multiples of 4 bytes */
            const uint32_t n = DIV_ROUND_UP(attr_count, 4);
            P_MTHD(p, NV9097, SET_STREAM_OUT_LAYOUT_SELECT(b, 0));
            P_INLINE_ARRAY(p, (const uint32_t*)xfb->attr_index[b], n);
         }
      }

      shader->vtgm_push_dw_count = nv_push_dw_count(&push);
      assert(max_dw_count ==
             nvk_max_shader_push_dw(pdev, shader->info.stage, true));
   }

   assert(nv_push_dw_count(&push) <= max_dw_count);
   assert(max_dw_count <= ARRAY_SIZE(push_dw));

   uint16_t dw_count = nv_push_dw_count(&push);
   shader->push_dw =
      vk_zalloc2(&dev->vk.alloc, pAllocator, dw_count * sizeof(*push_dw),
                 sizeof(*push_dw), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (shader->push_dw == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   memcpy(shader->push_dw, push_dw, dw_count * sizeof(*push_dw));

   return VK_SUCCESS;
}

static const struct vk_shader_ops nvk_shader_ops;

static void
nvk_shader_destroy(struct vk_device *vk_dev,
                   struct vk_shader *vk_shader,
                   const VkAllocationCallbacks* pAllocator)
{
   struct nvk_device *dev = container_of(vk_dev, struct nvk_device, vk);
   struct nvk_shader *shader = container_of(vk_shader, struct nvk_shader, vk);

   vk_free2(&dev->vk.alloc, pAllocator, shader->push_dw);
   switch_nvk_split_free(shader->switch_split);

   if (shader->upload_size > 0) {
      switch_nvk_program_freed(dev, shader);
      nvk_heap_free(dev, &dev->shader_heap,
                    shader->upload_addr,
                    shader->upload_size);
   }

   if (shader->nak) {
      nak_shader_bin_destroy(shader->nak);
   } else {
      /* This came from deserialize, just free it */
      free((void *)shader->code_ptr);
      free((void *)shader->asm_str);
   }

   free((void *)shader->data_ptr);
   ralloc_free((void *)shader->nir_str);

   vk_shader_free(&dev->vk, pAllocator, &shader->vk);
}

static VkResult
nvk_compile_shader(struct nvk_device *dev,
                   struct vk_shader_compile_info *info,
                   const struct vk_graphics_pipeline_state *state,
                   const VkAllocationCallbacks* pAllocator,
                   struct vk_shader **shader_out,
                   struct nvk_cbuf_map *cbuf_map)
{
   struct nvk_shader *shader;
   VkResult result;

   /* We consume the NIR, regardless of success or failure */
   nir_shader *nir = info->nir;

   shader = vk_shader_zalloc(&dev->vk, &nvk_shader_ops, info->stage,
                             pAllocator, sizeof(*shader));
   if (shader == NULL) {
      ralloc_free(nir);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (!nir->info.io_lowered) {
      nvk_lower_nir(dev, nir, info->flags, info->robustness,
                  info->set_layout_count, info->set_layouts,
                  cbuf_map);
   }

   switch_nvk_lower_late(nir, info->flags);
   shader->cbuf_map = *cbuf_map;

   struct nak_fs_key fs_key_tmp, *fs_key = NULL;
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      switch_nvk_fs_key(&fs_key_tmp, state);
      fs_key = &fs_key_tmp;
   }

   result = nvk_compile_nir(dev, nir, info->flags, info->robustness,
                            fs_key, shader);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return result;
   }

   if (dev->nvkmd) {
      result = nvk_shader_upload(dev, shader);
      if (result != VK_SUCCESS) {
         nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
         return result;
      }
   }

   if (info->stage == MESA_SHADER_FRAGMENT) {
      if (state != NULL && state->ms != NULL) {
         shader->sample_shading_enable = state->ms->sample_shading_enable;
         if (state->ms->sample_shading_enable)
            shader->min_sample_shading = state->ms->min_sample_shading;
      }
   }

   if (info->stage != MESA_SHADER_COMPUTE && dev->nvkmd) {
      result = nvk_shader_fill_push(dev, shader, pAllocator);
      if (result != VK_SUCCESS) {
         nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
         return result;
      }
   }

   *shader_out = &shader->vk;

   return VK_SUCCESS;
}

VkResult
nvk_compile_nir_shader(struct nvk_device *dev, nir_shader *nir,
                       const VkAllocationCallbacks *alloc,
                       struct nvk_shader **shader_out)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   const struct vk_pipeline_robustness_state rs_none = {
      .uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT,
      .storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT,
      .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT,
   };

   assert(nir->info.stage == MESA_SHADER_COMPUTE);
   if (nir->options == NULL)
      nir->options = nvk_get_nir_options((struct vk_physical_device *)&pdev->vk,
                                         nir->info.stage, &rs_none);

   struct vk_shader_compile_info info = {
      .stage = nir->info.stage,
      .nir = nir,
      .robustness = &rs_none,
   };

   struct vk_shader *shader = NULL;
   struct nvk_cbuf_map cbuf_map = {0};
   VkResult result = nvk_compile_shader(dev, &info, NULL, alloc, &shader, &cbuf_map);
   if (result != VK_SUCCESS)
      return result;

   *shader_out = container_of(shader, struct nvk_shader, vk);

   return VK_SUCCESS;
}

static VkResult
nvk_compile_shaders(struct vk_device *vk_dev,
                    uint32_t shader_count,
                    struct vk_shader_compile_info *infos,
                    const struct vk_graphics_pipeline_state *state,
                    const struct vk_features *enabled_features,
                    const VkAllocationCallbacks* pAllocator,
                    struct vk_shader **shaders_out)
{
   struct nvk_device *dev = container_of(vk_dev, struct nvk_device, vk);

   nir_shader *mesh_shader = NULL;
   nir_shader *shaders[shader_count];
   struct nvk_cbuf_map cbuf_maps[shader_count];
   memset(cbuf_maps, 0, sizeof(cbuf_maps));

   /* Lower shaders, notably lowering I/O. This is a prerequisite for
    * intershader optimization.
    */
   for (uint32_t i = 0; i < shader_count; i++) {
      const struct vk_shader_compile_info *info = &infos[i];
      nir_shader *nir = info->nir;

      if (nir->info.stage == MESA_SHADER_MESH)
         mesh_shader = nir;

      if (nir->info.stage == MESA_SHADER_FRAGMENT && mesh_shader) {
         nir_foreach_shader_in_variable(var, nir) {
            /* These variables are implicitly per-primitive when used with
             * mesh->fragment stages and this can't be determined with only the
             * FS. nir_opt_varyings relies on inputs and outputs agreeing on
             * per-primitive.
             */
            if (var->data.location == VARYING_SLOT_PRIMITIVE_ID ||
                var->data.location == VARYING_SLOT_VIEWPORT ||
                var->data.location == VARYING_SLOT_LAYER) {
               var->data.per_primitive = true;
            }
         }
      }

      nvk_lower_nir(dev, nir, info->flags, info->robustness,
                    info->set_layout_count, info->set_layouts,
                    &cbuf_maps[i]);

      if (nir->xfb_info) {
         /* Fold constant offset srcs for IO. */
         NIR_PASS(_, nir, nir_opt_constant_folding);

         nir_io_add_intrinsic_xfb_info(nir);
      }

      shaders[i] = nir;
   }

   nir_opt_varyings_bulk(shaders, shader_count, true, UINT32_MAX, UINT32_MAX,
                         switch_nvk_nir_opts, NULL);

   for (uint32_t i = 0; i < shader_count; i++) {
      VkResult result = nvk_compile_shader(dev, &infos[i], state,
                                           pAllocator, &shaders_out[i], &cbuf_maps[i]);
      if (result != VK_SUCCESS) {
         /* Clean up all the shaders before this point */
         for (uint32_t j = 0; j < i; j++)
            nvk_shader_destroy(&dev->vk, shaders_out[j], pAllocator);

         /* Clean up all the NIR after this point */
         for (uint32_t j = i + 1; j < shader_count; j++)
            ralloc_free(infos[j].nir);

         /* Memset the output array */
         memset(shaders_out, 0, shader_count * sizeof(*shaders_out));

         return result;
      }
   }

   return VK_SUCCESS;
}

static uint8_t
nvk_shader_get_shader_version_override(const struct nvk_device *dev,
                                       mesa_shader_stage stage)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const struct nvk_instance *instance = nvk_physical_device_instance(pdev);

   if (mesa_shader_stage_is_compute(stage))
      return instance->drirc.misc.override_compute_shader_version;

   assert(!mesa_shader_stage_is_rt(stage));
   return instance->drirc.misc.override_graphics_shader_version;
}

static VkResult
nvk_deserialize_shader(struct vk_device *vk_dev,
                       struct blob_reader *blob,
                       uint32_t binary_version,
                       const VkAllocationCallbacks* pAllocator,
                       struct vk_shader **shader_out)
{
   struct nvk_device *dev = container_of(vk_dev, struct nvk_device, vk);
   struct nvk_shader *shader;
   VkResult result;

   struct nak_shader_info info;
   blob_copy_bytes(blob, &info, sizeof(info));

   struct nvk_cbuf_map cbuf_map;
   blob_copy_bytes(blob, &cbuf_map, sizeof(cbuf_map));

   bool sample_shading_enable;
   blob_copy_bytes(blob, &sample_shading_enable, sizeof(sample_shading_enable));

   float min_sample_shading;
   blob_copy_bytes(blob, &min_sample_shading, sizeof(min_sample_shading));

   const uint8_t shader_version_override = blob_read_uint8(blob);
   const uint8_t expected_shader_version_override =
      nvk_shader_get_shader_version_override(dev, info.stage);

   if (shader_version_override != expected_shader_version_override)
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);

   const uint32_t code_size = blob_read_uint32(blob);
   const uint32_t data_size = blob_read_uint32(blob);
   if (blob->overrun)
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);

   shader = vk_shader_zalloc(&dev->vk, &nvk_shader_ops, info.stage,
                             pAllocator, sizeof(*shader));
   if (shader == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->info = info;
   shader->cbuf_map = cbuf_map;
   shader->sample_shading_enable = sample_shading_enable;
   shader->min_sample_shading = min_sample_shading;
   shader->code_size = code_size;
   shader->data_size = data_size;

   shader->code_ptr = malloc(code_size);
   if (shader->code_ptr == NULL) {
      nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   shader->data_ptr = malloc(data_size);
   if (shader->data_ptr == NULL) {
      nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   blob_copy_bytes(blob, (void *)shader->code_ptr, shader->code_size);
   blob_copy_bytes(blob, (void *)shader->data_ptr, shader->data_size);

   const char *asm_str = blob_read_string(blob);
   const char *nir_str = blob_read_string(blob);
   shader->asm_str = (asm_str && asm_str[0]) ? strdup(asm_str) : NULL;
   shader->nir_str = (nir_str && nir_str[0]) ? ralloc_strdup(NULL, nir_str) : NULL;

   if (blob->overrun) {
      nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
   }

   if (dev->nvkmd) {
      result = nvk_shader_upload(dev, shader);
      if (result != VK_SUCCESS) {
         nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
         return result;
      }
   }

   if (info.stage != MESA_SHADER_COMPUTE && dev->nvkmd) {
      result = nvk_shader_fill_push(dev, shader, pAllocator);
      if (result != VK_SUCCESS) {
         nvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
         return result;
      }
   }

   *shader_out = &shader->vk;

   return VK_SUCCESS;
}

static bool
nvk_shader_serialize(struct vk_device *vk_dev,
                     const struct vk_shader *vk_shader,
                     struct blob *blob)
{
   const struct nvk_device *dev = container_of(vk_dev, struct nvk_device, vk);
   struct nvk_shader *shader = container_of(vk_shader, struct nvk_shader, vk);

   blob_write_bytes(blob, &shader->info, sizeof(shader->info));
   blob_write_bytes(blob, &shader->cbuf_map, sizeof(shader->cbuf_map));
   blob_write_bytes(blob, &shader->sample_shading_enable,
                    sizeof(shader->sample_shading_enable));
   blob_write_bytes(blob, &shader->min_sample_shading,
                    sizeof(shader->min_sample_shading));
   blob_write_uint8(blob, nvk_shader_get_shader_version_override(dev, shader->info.stage));

   blob_write_uint32(blob, shader->code_size);
   blob_write_uint32(blob, shader->data_size);
   blob_write_bytes(blob, shader->code_ptr, shader->code_size);
   blob_write_bytes(blob, shader->data_ptr, shader->data_size);

   blob_write_string(blob, shader->asm_str ? shader->asm_str : "");
   blob_write_string(blob, shader->nir_str ? shader->nir_str : "");

   return !blob->out_of_memory;
}

#define WRITE_STR(field, ...) ({                               \
   memset(field, 0, sizeof(field));                            \
   UNUSED int i = snprintf(field, sizeof(field), __VA_ARGS__); \
   assert(i > 0 && i < sizeof(field));                         \
})

static VkResult
nvk_shader_get_executable_properties(
   UNUSED struct vk_device *device,
   const struct vk_shader *vk_shader,
   uint32_t *executable_count,
   VkPipelineExecutablePropertiesKHR *properties)
{
   struct nvk_shader *shader = container_of(vk_shader, struct nvk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutablePropertiesKHR, out,
                          properties, executable_count);

   vk_outarray_append_typed(VkPipelineExecutablePropertiesKHR, &out, props) {
      props->stages = mesa_to_vk_shader_stage(shader->info.stage);
      props->subgroupSize = 32;
      WRITE_STR(props->name, "%s",
                _mesa_shader_stage_to_string(shader->info.stage));
      WRITE_STR(props->description, "%s shader",
                _mesa_shader_stage_to_string(shader->info.stage));
   }

   return vk_outarray_status(&out);
}

static VkResult
nvk_shader_get_executable_statistics(
   UNUSED struct vk_device *device,
   const struct vk_shader *vk_shader,
   uint32_t executable_index,
   uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   struct nvk_shader *shader = container_of(vk_shader, struct nvk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableStatisticKHR, out,
                          statistics, statistic_count);

   assert(executable_index == 0);

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Instruction count");
      WRITE_STR(stat->description, "Number of instructions used by this shader");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_instrs;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Static cycle count");
      WRITE_STR(stat->description,
                "Total cycles used by fixed-latency instructions in this shader");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_static_cycles;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Max warps/SM");
      WRITE_STR(stat->description,
                "Maximum number of warps per SM based on static information");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.max_warps_per_sm;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Spills to memory");
      WRITE_STR(stat->description, "Number of spills from GPRs to memory");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_spills_to_mem;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Fills from memory");
      WRITE_STR(stat->description, "Number of fills from memory to GPRs");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_fills_from_mem;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Spills to reg");
      WRITE_STR(stat->description,
                "Number of spills between different register files");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_spills_to_reg;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Fills from reg");
      WRITE_STR(stat->description,
                "Number of fills between different register files");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_fills_from_reg;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Code size");
      WRITE_STR(stat->description,
                "Size of the compiled shader binary, in bytes");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->code_size;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Number of GPRs");
      WRITE_STR(stat->description, "Number of GPRs used by this pipeline");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.num_gprs;
   }

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "SLM size");
      WRITE_STR(stat->description,
                "Size of shader local (scratch) memory, in bytes");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->info.slm_size;
   }

   uint16_t smem_size = 0;
   if (shader->info.stage == MESA_SHADER_COMPUTE)
      smem_size = shader->info.cs.smem_size;

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat) {
      WRITE_STR(stat->name, "Shared size");
      WRITE_STR(stat->description,
                "Size of shader shared memory, in bytes");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = smem_size;
   }

   return vk_outarray_status(&out);
}

static bool
write_ir_text(VkPipelineExecutableInternalRepresentationKHR* ir,
              const char *data)
{
   ir->isText = VK_TRUE;

   size_t data_len = strlen(data) + 1;

   if (ir->pData == NULL) {
      ir->dataSize = data_len;
      return true;
   }

   strncpy(ir->pData, data, ir->dataSize);
   if (ir->dataSize < data_len)
      return false;

   ir->dataSize = data_len;
   return true;
}

static VkResult
nvk_shader_get_executable_internal_representations(
   UNUSED struct vk_device *device,
   const struct vk_shader *vk_shader,
   uint32_t executable_index,
   uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR *internal_representations)
{
   struct nvk_shader *shader = container_of(vk_shader, struct nvk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableInternalRepresentationKHR, out,
                          internal_representations,
                          internal_representation_count);
   bool incomplete_text = false;

   assert(executable_index == 0);

   if (shader->nir_str != NULL) {
      vk_outarray_append_typed(VkPipelineExecutableInternalRepresentationKHR, &out, ir) {
         WRITE_STR(ir->name, "NIR shader");
         WRITE_STR(ir->description, "NIR shader");
         if (!write_ir_text(ir, shader->nir_str))
            incomplete_text = true;
      }
   }

   if (shader->asm_str != NULL) {
      vk_outarray_append_typed(VkPipelineExecutableInternalRepresentationKHR, &out, ir) {
         WRITE_STR(ir->name, "NAK assembly");
         WRITE_STR(ir->description, "NAK assembly");
         if (!write_ir_text(ir, shader->asm_str))
            incomplete_text = true;
      }
   }

   return incomplete_text ? VK_INCOMPLETE : vk_outarray_status(&out);
}

static const struct vk_shader_ops nvk_shader_ops = {
   .destroy = nvk_shader_destroy,
   .serialize = nvk_shader_serialize,
   .get_executable_properties = nvk_shader_get_executable_properties,
   .get_executable_statistics = nvk_shader_get_executable_statistics,
   .get_executable_internal_representations =
      nvk_shader_get_executable_internal_representations,
};

const struct vk_device_shader_ops nvk_device_shader_ops = {
   .get_nir_options = nvk_get_nir_options,
   .get_spirv_options = nvk_get_spirv_options,
   .preprocess_nir = nvk_preprocess_nir,
   .hash_state = nvk_hash_state,
   .compile = nvk_compile_shaders,
   .deserialize = nvk_deserialize_shader,
   .cmd_set_dynamic_graphics_state = vk_cmd_set_dynamic_graphics_state,
   .cmd_bind_shaders = nvk_cmd_bind_shaders,
};
