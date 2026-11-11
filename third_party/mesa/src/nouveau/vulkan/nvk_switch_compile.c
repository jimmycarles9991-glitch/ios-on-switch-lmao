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

#include "nvk_descriptor_set_layout.h"
#include "nvk_descriptor_types.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_format.h"
#include "nvk_entrypoints.h"
#include "nvk_sampler.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_push.h"
#include "nvk_switch_round.h"
#include "nvk_switch_split.h"

#include "vk_graphics_state.h"
#include "vk_pipeline.h"
#include "vk_shader.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

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
   for (uint32_t c = 0; c < out->cbuf_count; c++)
      out->cbuf_types[c] = switch_nvk_cbuf_code(&shader->cbuf_map.cbufs[c]);
   out->num_gprs = shader->info.num_gprs;
   out->split = shader->switch_split;
   if (shader->info.stage == MESA_SHADER_FRAGMENT) {
      out->early_fragment_tests = shader->info.fs.early_fragment_tests;
      out->writes_depth = shader->info.fs.writes_depth;
      out->post_depth_coverage = shader->info.fs.post_depth_coverage;
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

   const VkShaderModuleCreateInfo module = {
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
   VkResult result =
      vk_pipeline_compile_library_stage(&dev->vk, info->pipeline_flags,
                                        info->pipeline_pnext, &stage,
                                        info->layout,
                                        info->stage == VK_SHADER_STAGE_COMPUTE_BIT ?
                                        NULL : &state, &shader);
   switch_nvk_const_push_current = NULL;
   switch_nvk_half_targets_current = 0;
   switch_nvk_split_current = false;
   if (result != VK_SUCCESS)
      return result;

   switch_nvk_stage_of(container_of(shader, struct nvk_shader, vk), out);
   out->owner = shader;
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
   split_program(&split->prologue, &out->prologue);
   out->layout = split->layout;
   memcpy(out->qmd, split->qmd, sizeof(out->qmd));
   out->threshold_pixels = split->threshold_pixels;
}

SWITCH_EXPORT void
switch_nvk_compile_abi(uint32_t sizes[3])
{
   sizes[0] = sizeof(struct switch_nvk_stage_info);
   sizes[1] = sizeof(struct switch_nvk_stage);
   sizes[2] = sizeof(struct switch_nvk_split_parts);
}

SWITCH_EXPORT bool
switch_nvk_set_layout_binding(VkDescriptorSetLayout _layout, uint32_t binding,
                              struct switch_nvk_set_binding *out)
{
   VK_FROM_HANDLE(nvk_descriptor_set_layout, layout, _layout);
   if (binding >= layout->binding_count)
      return false;
   const struct nvk_descriptor_set_binding_layout *b = &layout->binding[binding];
   *out = (struct switch_nvk_set_binding) {
      .offset = b->offset,
      .stride = b->stride,
      .array_size = b->array_size,
      .immutable_samplers = b->immutable_samplers != NULL,
   };
   return true;
}

/* nvk_descriptor_set_create's size: a set's fixed part, aligned as a set is
 * placed in its pool, which nvk_descriptor_set_addr hands the root table
 */
SWITCH_EXPORT uint32_t
switch_nvk_set_layout_size(VkDevice _device, VkDescriptorSetLayout _layout)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_descriptor_set_layout, layout, _layout);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const uint32_t align_B =
      MAX2(nvk_min_cbuf_alignment(&pdev->info), pdev->info.nc_atom_size_B);
   return align(layout->non_variable_descriptor_buffer_size, align_B);
}

static bool
sampler_header(const struct nvk_device *dev, const struct nvk_sampler *sampler,
               uint32_t header[8])
{
   const struct nvk_descriptor_table *table = &dev->samplers;
   const uint64_t addr = nvk_descriptor_table_base_address(
      (struct nvk_descriptor_table *)table) +
      (uint64_t)sampler->planes[0].desc_index * table->desc_size;
   if (!nvk_mem_arena_is_mapped(&table->arena) || table->desc_size != 32)
      return false;
   memcpy(header, nvk_mem_arena_map(&table->arena, addr, 32), 32);
   return true;
}

SWITCH_EXPORT bool
switch_nvk_immutable_sampler_header(VkDevice _device,
                                    VkDescriptorSetLayout _layout,
                                    uint32_t binding, uint32_t element,
                                    uint32_t header[8])
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_descriptor_set_layout, layout, _layout);
   if (binding >= layout->binding_count)
      return false;
   const struct nvk_descriptor_set_binding_layout *b = &layout->binding[binding];
   if (b->immutable_samplers == NULL || element >= b->array_size)
      return false;
   return sampler_header(dev, b->immutable_samplers[element], header);
}

SWITCH_EXPORT bool
switch_nvk_sampler_header(VkDevice _device, VkSampler _sampler,
                          uint32_t header[8])
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_sampler, sampler, _sampler);
   return sampler != NULL && sampler_header(dev, sampler, header);
}

SWITCH_EXPORT uint32_t
switch_nvk_buffer_descriptor_size(VkDevice _device, VkDescriptorType type,
                                  uint64_t range)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const VkDeviceAddressRangeEXT addr_range = { .address = 0, .size = range };
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return nvk_ubo_descriptor_addr(pdev, nvk_ubo_descriptor(pdev, addr_range)).size;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return nvk_ssbo_descriptor(pdev, addr_range).addr.size;
   default:
      return 0;
   }
}

/* nvk_cmd_upload_qmd's constant buffers for a dispatch of a compute stage
 * with set 0 of set_bytes: the root table, set 0 and the shader data, each
 * aligned as a constant buffer, at address 0
 */
static bool
compute_cbufs(const struct nvk_physical_device *pdev,
              const struct nvk_shader *shader, uint32_t set_bytes,
              struct nak_qmd_info *info)
{
   const uint32_t align_B = nvk_min_cbuf_alignment(&pdev->info);
   for (uint32_t c = 0; c < shader->cbuf_map.cbuf_count; c++) {
      const struct nvk_cbuf *cbuf = &shader->cbuf_map.cbufs[c];
      uint32_t size_B;
      switch (cbuf->type) {
      case NVK_CBUF_TYPE_INVALID:
         size_B = 0;
         break;
      case NVK_CBUF_TYPE_ROOT_DESC:
         size_B = sizeof(struct nvk_root_descriptor_table);
         break;
      case NVK_CBUF_TYPE_SHADER_DATA:
         size_B = shader->data_size;
         break;
      case NVK_CBUF_TYPE_DESC_SET:
         if (cbuf->desc_set != 0)
            return false;
         size_B = set_bytes;
         break;
      default:
         return false;
      }
      if (size_B == 0)
         continue;
      info->cbufs[info->num_cbufs++] = (struct nak_qmd_cbuf) {
         .index = c,
         .size = MIN2(align(size_B, align_B), NVK_MAX_CBUF_SIZE),
      };
   }
   return true;
}

/* The QMD NVK fills for a dispatch of a compute stage switch_nvk_compile_stage
 * made, with set 0 of set_bytes, its program at 0, no grid and its constant
 * buffers at 0. False where the stage reads a constant buffer other than the
 * root table, set 0 and its shader data.
 */
SWITCH_EXPORT bool
switch_nvk_compute_qmd(VkDevice _device, const struct switch_nvk_stage *stage,
                       uint32_t set_bytes, uint32_t qmd[64])
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   if (stage->owner == NULL)
      return false;
   const struct nvk_shader *shader =
      container_of((struct vk_shader *)stage->owner, struct nvk_shader, vk);
   const uint32_t qmd_size_B = nak_qmd_size_B(&pdev->info);
   if (shader->info.stage != MESA_SHADER_COMPUTE || qmd_size_B > 64 * 4)
      return false;

   struct nak_qmd_info info = {
      .smem_size = shader->info.cs.smem_size,
   };
   if (!compute_cbufs(pdev, shader, set_bytes, &info))
      return false;
   uint32_t words[NAK_MAX_QMD_DWORDS] = { 0 };
   nak_fill_qmd(&pdev->info, &shader->info, &info, words, qmd_size_B);
   memcpy(qmd, words, 64 * 4);
   return true;
}

static enum pipe_swizzle
pipe_swizzle_of(VkComponentSwizzle swizzle, enum pipe_swizzle own)
{
   switch (swizzle) {
   case VK_COMPONENT_SWIZZLE_ZERO: return PIPE_SWIZZLE_0;
   case VK_COMPONENT_SWIZZLE_ONE: return PIPE_SWIZZLE_1;
   case VK_COMPONENT_SWIZZLE_R: return PIPE_SWIZZLE_X;
   case VK_COMPONENT_SWIZZLE_G: return PIPE_SWIZZLE_Y;
   case VK_COMPONENT_SWIZZLE_B: return PIPE_SWIZZLE_Z;
   case VK_COMPONENT_SWIZZLE_A: return PIPE_SWIZZLE_W;
   default: return own;
   }
}

/* Word 0 of the texture header NVK writes for a 2D view of format with
 * components (nvk_image_view_init_no_alloc): its component sizes, data types
 * and sources, which no address, extent or view type reaches
 */
SWITCH_EXPORT bool
switch_nvk_view_header_word0(VkDevice _device, VkFormat format,
                             const VkComponentMapping *components,
                             bool storage, uint32_t *word0)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const enum pipe_format p_format = nvk_format_to_pipe_format(format);
   if (p_format == PIPE_FORMAT_NONE)
      return false;

   const struct nil_image_init_info init = {
      .dim = NIL_IMAGE_DIM_2D,
      .format = nil_format(p_format),
      .extent_px = { .width = 16, .height = 8, .depth = 1, .array_len = 1 },
      .levels = 1,
      .samples = 1,
   };
   struct nil_image image;
   if (!nil_image_init(&pdev->info, &image, &init))
      return false;
   const struct nil_view view = {
      .view_type = NIL_VIEW_TYPE_2D,
      .access = storage ? NIL_VIEW_ACCESS_STORAGE : NIL_VIEW_ACCESS_TEXTURE,
      .format = nil_format(p_format),
      .num_levels = 1,
      .array_len = 1,
      .swizzle = {
         pipe_swizzle_of(components->r, PIPE_SWIZZLE_X),
         pipe_swizzle_of(components->g, PIPE_SWIZZLE_Y),
         pipe_swizzle_of(components->b, PIPE_SWIZZLE_Z),
         pipe_swizzle_of(components->a, PIPE_SWIZZLE_W),
      },
   };
   const struct nil_descriptor desc =
      nil_image_view_descriptor(&pdev->info, &image, &view, 0);
   *word0 = desc.bits[0];
   return true;
}
