/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: what a compiled stage hands the raw GM20B path: the stage as its
 * record holds it, and a fragment stage's split as parts.
 */

#include "switch_compile.h"
#include "nvk_switch_stage.h"

#include "nvk_shader.h"

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

/* A constant buffer as the raw path's pack names it (raw_pack.h PackCbufType):
 * the nvk_cbuf_type, and a UBO descriptor's set-0 offset / 16 in bits 3-7 or
 * a descriptor set's number in bits 3-7 (set 0's code is the bare type); a
 * UBO elsewhere is 7, which no raw draw binds.
 */
uint8_t
switch_nvk_cbuf_code(const struct nvk_cbuf *cbuf)
{
   if (cbuf->type == NVK_CBUF_TYPE_DESC_SET)
      return cbuf->desc_set < 32 ? NVK_CBUF_TYPE_DESC_SET | cbuf->desc_set << 3 : 7;
   if (cbuf->type != NVK_CBUF_TYPE_UBO_DESC)
      return cbuf->type;
   if (cbuf->desc_set != 0 || cbuf->desc_offset % 16 || cbuf->desc_offset >= 512)
      return 7;
   return NVK_CBUF_TYPE_UBO_DESC | (cbuf->desc_offset / 16) << 3;
}

uint32_t
switch_nvk_cbuf_offset(const struct nvk_cbuf *cbuf)
{
   return cbuf->type == NVK_CBUF_TYPE_UBO_DESC ? cbuf->desc_offset : 0;
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
      out->cbuf_offsets[c] = switch_nvk_cbuf_offset(&shader->cbuf_map.cbufs[c]);
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

static void
split_program(const struct switch_nvk_program *p, struct switch_nvk_stage *out)
{
   out->code = p->code;
   out->code_bytes = p->code_bytes;
   out->data = p->data;
   out->data_bytes = p->data_bytes;
   out->cbuf_count = MIN2(p->cbuf_count, 16);
   memcpy(out->cbuf_types, p->cbuf_types, sizeof(out->cbuf_types));
   memcpy(out->cbuf_sets, p->cbuf_sets, sizeof(out->cbuf_sets));
   memcpy(out->cbuf_offsets, p->cbuf_offsets, sizeof(out->cbuf_offsets));
   out->num_gprs = p->info.num_gprs;
   out->bound_textures = p->info.bound_textures;
   qmd_inputs(&p->info, out);
}

SWITCH_EXPORT void
switch_compile_split_parts(const struct switch_nvk_split *split,
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
switch_compile_abi(uint32_t sizes[3])
{
   sizes[0] = sizeof(struct switch_nvk_stage_info);
   sizes[1] = sizeof(struct switch_nvk_stage);
   sizes[2] = sizeof(struct switch_nvk_split_parts);
}
