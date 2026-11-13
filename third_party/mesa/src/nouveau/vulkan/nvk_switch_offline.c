/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: switch_nvk_compile_stage's compile with no instance and no device.
 * What it read of NVK's physical and logical device comes from a frozen
 * table (switch_nak_frozen), and the layout comes as data. NVK's lowering
 * passes read set layouts through NVK's layout structs, which are filled
 * here as plain data: no Vulkan object is initialised.
 */

#include "nvk_switch_offline.h"

#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_ir.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_split.h"
#include "nvk_switch_turn.h"
#include "nvk_switch_xr_planes.h"

#include "vk_graphics_state.h"
#include "vk_nir.h"
#include "vk_nir_lower_descriptor_heaps.h"
#include "vk_pipeline.h"
#include "vk_pipeline_layout.h"

#include "compiler/glsl_types.h"

#include <pthread.h>

#define SWITCH_EXPORT __attribute__((visibility("default")))

static struct nak_compiler *frozen_nak;
static struct nv_device_info frozen_info;
static pthread_once_t frozen_once = PTHREAD_ONCE_INIT;
static const struct switch_nak_frozen *frozen_table;

/* With no vk_instance in the process, nothing else holds the GLSL type
 * singleton spirv_to_nir makes types in (vk_instance_init takes it): the
 * offline compile holds it for the process.
 */
static void
make_nak(void)
{
   glsl_type_singleton_init_or_ref();
   frozen_info = frozen_table->info;
   frozen_nak = nak_compiler_create(&frozen_info);
}

static struct switch_nvk_target
target_of(const struct switch_nak_frozen *f)
{
   frozen_table = f;
   pthread_once(&frozen_once, make_nak);
   return (struct switch_nvk_target) {
      .info = &frozen_info,
      .nak = frozen_nak,
      .debug_flags = f->debug_flags,
      .min_ssbo_alignment = f->min_ssbo_alignment,
   };
}

struct offline_layout {
   struct vk_pipeline_layout pipeline;
   struct nvk_descriptor_set_layout *sets[MESA_VK_MAX_DESCRIPTOR_SETS];
};

/* nvk_CreateDescriptorSetLayout's bindings, with the offsets and strides the
 * caller laid them out at
 */
static struct nvk_descriptor_set_layout *
set_of(const struct switch_nak_set *s)
{
   uint32_t count = 0;
   for (uint32_t i = 0; i < s->binding_count; i++)
      count = MAX2(count, s->bindings[i].binding + 1);
   struct nvk_descriptor_set_layout *l =
      calloc(1, sizeof(*l) + count * sizeof(l->binding[0]));
   if (l == NULL)
      return NULL;
   l->vk.flags = l->flags = s->flags;
   l->binding_count = count;
   uint8_t dynamic = 0;
   for (uint32_t b = 0; b < count; b++) {
      for (uint32_t i = 0; i < s->binding_count; i++) {
         const struct switch_nak_binding *in = &s->bindings[i];
         if (in->binding != b || in->count == 0)
            continue;
         struct nvk_descriptor_set_binding_layout *out = &l->binding[b];
         out->type = in->type;
         out->flags = in->flags;
         out->array_size = in->count;
         out->offset = in->offset;
         out->stride = in->stride;
         if (in->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
             in->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            out->dynamic_buffer_index = dynamic;
            if (in->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
               BITSET_SET_COUNT(l->dynamic_ubos, dynamic, in->count);
            dynamic += in->count;
         }
      }
   }
   l->vk.dynamic_descriptor_count = dynamic;
   return l;
}

static void
layout_finish(struct offline_layout *l)
{
   for (uint32_t s = 0; s < MESA_VK_MAX_DESCRIPTOR_SETS; s++)
      free(l->sets[s]);
}

static bool
layout_of(const struct switch_nak_layout *in, struct offline_layout *out)
{
   memset(out, 0, sizeof(*out));
   if (in->set_count > MESA_VK_MAX_DESCRIPTOR_SETS ||
       in->push_range_count > ARRAY_SIZE(out->pipeline.push_ranges))
      return false;
   out->pipeline.create_flags = in->create_flags;
   out->pipeline.set_count = in->set_count;
   for (uint32_t s = 0; s < in->set_count; s++) {
      out->sets[s] = set_of(&in->sets[s]);
      if (out->sets[s] == NULL)
         return false;
      out->pipeline.set_layouts[s] = &out->sets[s]->vk;
   }
   out->pipeline.push_range_count = in->push_range_count;
   memcpy(out->pipeline.push_ranges, in->push_ranges,
          in->push_range_count * sizeof(in->push_ranges[0]));
   return true;
}

static nir_shader *
spirv_nir(const struct switch_nak_frozen *f,
          const struct switch_nvk_stage_info *info, const uint32_t *words,
          size_t bytes, const struct spirv_to_nir_options *spirv_options,
          const struct nir_shader_compiler_options *nir_options)
{
   struct spirv_to_nir_options o = *spirv_options;
   o.capabilities = &f->spirv_caps;
   o.sampler_descriptor_size = f->sampler_descriptor_size;
   o.sampler_descriptor_alignment = f->sampler_descriptor_alignment;
   o.image_descriptor_size = f->image_descriptor_size;
   o.image_descriptor_alignment = f->image_descriptor_alignment;
   o.buffer_descriptor_size = f->buffer_descriptor_size;
   o.buffer_descriptor_alignment = f->buffer_descriptor_alignment;
   return vk_spirv_to_nir_with(words, bytes, vk_to_mesa_shader_stage(info->stage),
                               info->entry, info->specialization, &o,
                               nir_options, false, NULL);
}

/* vk_pipeline_compile_native_stage's NIR of the stage */
static nir_shader *
stage_nir(const struct switch_nak_frozen *f,
          const struct switch_nvk_stage_info *info,
          const struct spirv_to_nir_options *spirv_options,
          const struct nir_shader_compiler_options *nir_options,
          bool *native_used)
{
   const mesa_shader_stage stage = vk_to_mesa_shader_stage(info->stage);
   nir_shader *nir = NULL;
   *native_used = false;
   if (stage == MESA_SHADER_COMPUTE && info->semantic != NULL)
      nir = switch_nvk_ir_to_nir(info->semantic, nir_options, spirv_options,
                                 info->specialization);
   if (nir != NULL) {
      *native_used = true;
      vk_set_subgroup_size(nir, f->subgroup_size, f->min_subgroup_size,
                           f->max_subgroup_size, 0x10500, info->stage_pnext,
                           false, false);
   } else {
      const uint32_t *words = info->spirv;
      size_t bytes = info->spirv_bytes;
      if (bytes < 20 && info->portable != NULL &&
          !info->portable(info->portable_context, &words, &bytes))
         return NULL;
      if (bytes < 20)
         return NULL;
      nir = spirv_nir(f, info, words, bytes, spirv_options, nir_options);
      if (nir == NULL)
         return NULL;
      vk_set_subgroup_size(nir, f->subgroup_size, f->min_subgroup_size,
                           f->max_subgroup_size,
                           vk_spirv_version(words, bytes), info->stage_pnext,
                           false, false);
   }
   if (info->pipeline_flags &
       VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT_KHR)
      NIR_PASS(_, nir, nir_lower_view_index_to_device_index);
   return nir;
}

static VkResult
compile(const struct switch_nak_frozen *f,
        const struct switch_nvk_stage_info *info,
        const struct offline_layout *layout, struct nvk_shader *shader,
        bool *native_used)
{
   const struct switch_nvk_target t = target_of(f);
   const mesa_shader_stage stage = vk_to_mesa_shader_stage(info->stage);
   struct vk_pipeline_robustness_state rs;
   vk_pipeline_robustness_state_fill(&f->robustness, &rs, info->pipeline_pnext,
                                     info->stage_pnext);
   const struct nir_shader_compiler_options *nir_options =
      nak_nir_options(t.nak);
   const struct spirv_to_nir_options spirv_options =
      switch_nvk_spirv_options(&t, &rs);
   nir_shader *nir =
      stage_nir(f, info, &spirv_options, nir_options, native_used);
   if (nir == NULL)
      return VK_ERROR_UNKNOWN;
   switch_nvk_preprocess(&t, nir);

   struct vk_sampler_state_array embedded;
   bool heaps = false;
   NIR_PASS(heaps, nir, vk_nir_lower_descriptor_heaps, NULL, NULL, &embedded);
   if (heaps) {
      NIR_PASS(_, nir, nir_remove_dead_variables,
               nir_var_uniform | nir_var_image, NULL);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   vk_sampler_state_array_finish(&embedded);

   const VkShaderCreateFlagsEXT flags =
      vk_pipeline_to_shader_flags(info->pipeline_flags, stage,
                                  (struct vk_pipeline_layout *)&layout->pipeline);
   struct nvk_cbuf_map cbuf_map = {0};
   switch_nvk_lower(&t, nir, flags, &rs, layout->pipeline.set_count,
                    layout->pipeline.set_layouts, &cbuf_map);
   switch_nvk_varyings(nir);
   switch_nvk_lower_late(nir, flags);
   shader->cbuf_map = cbuf_map;

   const struct vk_multisample_state ms = {
      .sample_shading_enable = info->sample_shading,
   };
   const struct vk_graphics_pipeline_state state = {
      .shader_stages = info->stage,
      .pipeline_flags = info->pipeline_flags,
      .ms = stage == MESA_SHADER_FRAGMENT ? &ms : NULL,
   };
   struct nak_fs_key fs_key, *key = NULL;
   if (stage == MESA_SHADER_FRAGMENT) {
      switch_nvk_fs_key(&fs_key, &state);
      key = &fs_key;
   }
   VkResult result = switch_nvk_compile_nir(&t, nir, flags, &rs, key, shader);
   ralloc_free(nir);
   if (result == VK_SUCCESS && stage == MESA_SHADER_FRAGMENT)
      shader->sample_shading_enable = ms.sample_shading_enable;
   return result;
}

SWITCH_EXPORT VkResult
switch_nak_compile_stage(const struct switch_nak_frozen *frozen,
                         const struct switch_nvk_stage_info *info,
                         const struct switch_nak_layout *layout,
                         struct switch_nvk_stage *out)
{
   memset(out, 0, sizeof(*out));
   if (frozen->version != sizeof(*frozen))
      return VK_ERROR_INITIALIZATION_FAILED;
   struct offline_layout l;
   if (!layout_of(layout, &l)) {
      layout_finish(&l);
      return VK_ERROR_UNKNOWN;
   }
   struct nvk_shader *shader = calloc(1, sizeof(*shader));
   if (shader == NULL) {
      layout_finish(&l);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   shader->info.stage = vk_to_mesa_shader_stage(info->stage);

   struct switch_nvk_const_push push = {.mask = info->const_push_mask};
   memcpy(push.words, info->const_push_words, sizeof(push.words));
   switch_nvk_const_push_current = push.mask ? &push : NULL;
   switch_nvk_half_targets_current = info->half_targets;
   switch_nvk_split_current = info->split;
   switch_nvk_center_frag_depth_current = info->center_frag_depth != 0;
   switch_nvk_texs_f16_current = info->texs_f16;
   switch_nvk_turned_current = info->turned;
   switch_nvk_xr_plane_bindings_current = info->xr_plane_bindings;
   bool native_used = false;
   VkResult result = compile(frozen, info, &l, shader, &native_used);
   switch_nvk_const_push_current = NULL;
   switch_nvk_half_targets_current = 0;
   switch_nvk_split_current = false;
   switch_nvk_center_frag_depth_current = false;
   switch_nvk_texs_f16_current = false;
   switch_nvk_turned_current = false;
   switch_nvk_xr_plane_bindings_current = 0;
   layout_finish(&l);
   if (result != VK_SUCCESS) {
      switch_nak_stage_finish(&(struct switch_nvk_stage){.owner = shader});
      return result;
   }
   switch_nvk_stage_of(shader, out);
   out->owner = shader;
   out->semantic_version = native_used ? info->semantic->version : 0;
   return VK_SUCCESS;
}

SWITCH_EXPORT void
switch_nak_stage_finish(struct switch_nvk_stage *stage)
{
   struct nvk_shader *shader = stage->owner;
   if (shader == NULL)
      return;
   switch_nvk_split_free(shader->switch_split);
   if (shader->nak)
      nak_shader_bin_destroy(shader->nak);
   free((void *)shader->data_ptr);
   ralloc_free((void *)shader->nir_str);
   free(shader);
   stage->owner = NULL;
}

SWITCH_EXPORT size_t
switch_nak_freeze(VkDevice _device, struct switch_nak_frozen *out, size_t size)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   if (out == NULL || size != sizeof(*out))
      return sizeof(*out);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const struct vk_properties *p = &pdev->vk.properties;
   memset(out, 0, sizeof(*out));
   out->version = sizeof(*out);
   out->info = pdev->info;
   out->debug_flags = pdev->debug_flags;
   out->min_ssbo_alignment =
      switch_nvk_target_of(pdev).min_ssbo_alignment;
   out->robustness = dev->vk.robustness_state;
   out->spirv_caps = vk_physical_device_get_spirv_capabilities(&pdev->vk);
   out->subgroup_size = p->subgroupSize;
   out->min_subgroup_size = p->minSubgroupSize;
   out->max_subgroup_size = p->maxSubgroupSize;
   out->sampler_descriptor_size = p->samplerDescriptorSize;
   out->sampler_descriptor_alignment = p->samplerDescriptorAlignment;
   out->image_descriptor_size = p->imageDescriptorSize;
   out->image_descriptor_alignment = p->imageDescriptorAlignment;
   out->buffer_descriptor_size = p->bufferDescriptorSize;
   out->buffer_descriptor_alignment = p->bufferDescriptorAlignment;
   return sizeof(*out);
}
