/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: what NVK's stage compile reads of its physical device. The device
 * path takes it from the nvk_physical_device, the offline compile
 * (nvk_switch_offline.c) from a frozen table, so both run the same lowering.
 */
#ifndef NVK_SWITCH_TARGET_H
#define NVK_SWITCH_TARGET_H

#include <stdint.h>

#include "nir.h"
#include "vulkan/vulkan_core.h"

struct nak_compiler;
struct nak_fs_key;
struct nv_device_info;
struct nvk_cbuf_map;
struct nvk_physical_device;
struct nvk_shader;
struct vk_descriptor_set_layout;
struct vk_graphics_pipeline_state;
struct vk_pipeline_robustness_state;

struct switch_nvk_target {
   const struct nv_device_info *info;
   struct nak_compiler *nak;
   uint64_t debug_flags; /* NVK_DEBUG_* */
   uint32_t min_ssbo_alignment;
};

struct switch_nvk_target
switch_nvk_target_of(const struct nvk_physical_device *pdev);

struct spirv_to_nir_options
switch_nvk_spirv_options(const struct switch_nvk_target *t,
                         const struct vk_pipeline_robustness_state *rs);
void switch_nvk_preprocess(const struct switch_nvk_target *t, nir_shader *nir);
void switch_nvk_fs_key(struct nak_fs_key *key,
                       const struct vk_graphics_pipeline_state *state);
void switch_nvk_lower(const struct switch_nvk_target *t, nir_shader *nir,
                      VkShaderCreateFlagsEXT shader_flags,
                      const struct vk_pipeline_robustness_state *rs,
                      uint32_t set_layout_count,
                      struct vk_descriptor_set_layout * const *set_layouts,
                      struct nvk_cbuf_map *cbuf_map_out);
void switch_nvk_varyings(nir_shader *nir);
void switch_nvk_lower_late(nir_shader *nir,
                           VkShaderCreateFlagsEXT shader_flags);
VkResult switch_nvk_compile_nir(const struct switch_nvk_target *t,
                                nir_shader *nir,
                                VkShaderCreateFlagsEXT shader_flags,
                                const struct vk_pipeline_robustness_state *rs,
                                const struct nak_fs_key *fs_key,
                                struct nvk_shader *shader);

#endif /* NVK_SWITCH_TARGET_H */
