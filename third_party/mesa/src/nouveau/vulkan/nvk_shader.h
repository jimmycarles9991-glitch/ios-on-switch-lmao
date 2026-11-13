/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_SHADER_H
#define NVK_SHADER_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "vk_pipeline_cache.h"

#include "nak.h"
#include "nir_defines.h"

#include "vk_shader.h"

struct nak_shader_bin;
struct nvk_device;
struct nvk_physical_device;
struct nvk_pipeline_compilation_ctx;
struct switch_nvk_split;
struct vk_descriptor_set_layout;
struct vk_graphics_pipeline_state;
struct vk_pipeline_cache;
struct vk_pipeline_layout;
struct vk_pipeline_robustness_state;
struct vk_shader_module;

#define GF100_SHADER_HEADER_SIZE (20 * 4)
#define TU102_SHADER_HEADER_SIZE (32 * 4)
#define NVC0_MAX_SHADER_HEADER_SIZE TU102_SHADER_HEADER_SIZE

#define NVK_SHADER_STAGE_VTGM_BITS \
   (VK_SHADER_STAGE_VERTEX_BIT | \
    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | \
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | \
    VK_SHADER_STAGE_GEOMETRY_BIT | \
    VK_SHADER_STAGE_TASK_BIT_EXT | \
    VK_SHADER_STAGE_MESH_BIT_EXT)

#define NVK_SHADER_STAGE_GRAPHICS_BITS \
   (NVK_SHADER_STAGE_VTGM_BITS | VK_SHADER_STAGE_FRAGMENT_BIT)

static inline mesa_shader_stage
nvk_last_vtgm_shader_stage(VkShaderStageFlags stages)
{
   stages &= ~VK_SHADER_STAGE_FRAGMENT_BIT;
   stages = 1 << (util_last_bit(stages) - 1);
   return vk_to_mesa_shader_stage(stages);
}

static inline uint32_t
nvk_cbuf_binding_for_stage(mesa_shader_stage stage, bool has_task_shader)
{
   if (stage == MESA_SHADER_MESH && !has_task_shader)
      return MESA_SHADER_VERTEX;
   else if (stage == MESA_SHADER_MESH)
      return MESA_SHADER_TESS_EVAL;
   else if (stage == MESA_SHADER_TASK)
      return MESA_SHADER_VERTEX;

   return stage;
}

enum ENUM_PACKED nvk_cbuf_type {
   NVK_CBUF_TYPE_INVALID = 0,
   NVK_CBUF_TYPE_ROOT_DESC,
   NVK_CBUF_TYPE_SHADER_DATA,
   NVK_CBUF_TYPE_DESC_SET,
   NVK_CBUF_TYPE_DYNAMIC_UBO,
   NVK_CBUF_TYPE_UBO_DESC,
};

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_cbuf {
   enum nvk_cbuf_type type;
   uint8_t desc_set;
   uint8_t dynamic_idx;
   uint8_t _pad;
   uint32_t desc_offset;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_cbuf) == 8, "This struct has no holes");

struct nvk_cbuf_map {
   uint32_t cbuf_count;
   struct nvk_cbuf cbufs[16];
};

uint16_t
nvk_max_shader_push_dw(const struct nvk_physical_device *pdev,
                       mesa_shader_stage stage, bool last_vtgm);

struct nvk_shader {
   struct vk_shader vk;

   struct nak_shader_info info;
   struct nvk_cbuf_map cbuf_map;

   /* Only relevant for fragment shaders */
   bool sample_shading_enable;
   float min_sample_shading;

   struct nak_shader_bin *nak;
   const char *asm_str;
   const char *nir_str;
   const void *code_ptr;
   uint32_t code_size;

   const void *data_ptr;
   uint32_t data_size;

   uint32_t upload_size;
   uint64_t upload_addr;

   /* Address of the shader header (or start of the shader code) for compute
    * shaders.
    *
    * Prior to Volta, this is relative to the start of the shader heap. On
    * Volta and later, it's an absolute address.
    */
   uint64_t hdr_addr;

   /* Address of the GS shader header (or 0 if not present) for mesh
    * shaders.
    */
   uint64_t gs_hdr_addr;

   /* Address of the start of the shader data section */
   uint64_t data_addr;

   uint16_t push_dw_count;
   uint16_t vtgm_push_dw_count;
   uint32_t *push_dw;

   /* The raw path's split of a fragment program (nvk_switch_split.h) */
   struct switch_nvk_split *switch_split;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_shader, vk.base, VkShaderEXT,
                               VK_OBJECT_TYPE_SHADER_EXT);

/* The tile coalescer's conflict detection for an interlocked fragment shader
 * that `per_sample` runs one pass per sample (SET_HYBRID_ANTI_ALIAS_CONTROL
 * passes = samples), whether the pipeline's sample shading or a SampleId read
 * asks for it: sample, never pixel. Upstream's note had pixel detection fault
 * the GPU under the pipeline's sample shading; on GM20B m2v's per-sample
 * fetch, a SampleId read under pixel detection, is the draw whose channel
 * dies with DATA_ERROR INVALID_A_A_COMBINATION03 (gpu.m2v.msaa_fetch). Sample
 * detection orders each sample's invocations, not the pixel's.
 */
static inline enum nak_fs_interlock_mode
nvk_fs_interlock_mode(enum nak_fs_interlock_mode mode, bool per_sample)
{
   return per_sample && mode == NAK_FS_INTERLOCK_MODE_PIXEL ?
          NAK_FS_INTERLOCK_MODE_SAMPLE : mode;
}

extern const struct vk_device_shader_ops nvk_device_shader_ops;

#include "nvk_switch_target.h"

uint64_t
nvk_physical_device_compiler_flags(const struct nvk_physical_device *pdev);

nir_address_format
nvk_ubo_addr_format(const struct nv_device_info *info,
                    const struct vk_pipeline_robustness_state *rs);
nir_address_format
nvk_ssbo_addr_format(const struct vk_pipeline_robustness_state *rs);

bool
nvk_nir_lower_descriptors(nir_shader *nir,
                          const struct nv_device_info *info,
                          bool edb_buffer_views,
                          VkShaderCreateFlagsEXT shader_flags,
                          const struct vk_pipeline_robustness_state *rs,
                          uint32_t set_layout_count,
                          struct vk_descriptor_set_layout * const *set_layouts,
                          struct nvk_cbuf_map *cbuf_map_out);

bool nvk_nir_lower_mesh_shader(nir_shader *nir, VkShaderCreateFlagsEXT shader_flags);
bool nvk_nir_lower_task_shader(nir_shader *nir);

VkResult
nvk_compile_nir_shader(struct nvk_device *dev, nir_shader *nir,
                       const VkAllocationCallbacks *alloc,
                       struct nvk_shader **shader_out);

VkResult
nvk_shader_fill_push(struct nvk_device *dev, struct nvk_shader *shader,
                     const VkAllocationCallbacks *pAllocator);

uint32_t mesa_to_nv9097_shader_type(mesa_shader_stage stage, bool has_task_shader);
uint32_t nvk_pipeline_bind_group(mesa_shader_stage stage, bool has_task_shader);

#endif
