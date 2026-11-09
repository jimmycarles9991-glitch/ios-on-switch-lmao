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
#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_sampler.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_round.h"

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

   switch_nvk_half_targets_current = info->half_targets;
   struct vk_shader *shader = NULL;
   VkResult result =
      vk_pipeline_compile_library_stage(&dev->vk, info->pipeline_flags,
                                        info->pipeline_pnext, &stage,
                                        info->layout, &state, &shader);
   switch_nvk_half_targets_current = 0;
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
   const struct nvk_sampler *sampler = b->immutable_samplers[element];
   const struct nvk_descriptor_table *table = &dev->samplers;
   const uint64_t addr = nvk_descriptor_table_base_address(
      (struct nvk_descriptor_table *)table) +
      (uint64_t)sampler->planes[0].desc_index * table->desc_size;
   if (!nvk_mem_arena_is_mapped(&table->arena) || table->desc_size != 32)
      return false;
   memcpy(header, nvk_mem_arena_map(&table->arena, addr, 32), 32);
   return true;
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
