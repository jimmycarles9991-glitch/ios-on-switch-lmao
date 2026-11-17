/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

#include "nvk_switch_ubo_fold.h"

#include "nvk_shader.h"

#include "nir_builder.h"
#include "nir_range_analysis.h"

_Thread_local const struct switch_nvk_ubo_fold *switch_nvk_ubo_fold_current;

struct fold_ctx {
   const struct nvk_cbuf_map *map;
   const struct switch_nvk_ubo_fold *fold;
};

static bool
is_folded_cbuf(const struct fold_ctx *ctx, uint64_t c)
{
   if (c >= ctx->map->cbuf_count)
      return false;
   const struct nvk_cbuf *cbuf = &ctx->map->cbufs[c];
   return cbuf->type == NVK_CBUF_TYPE_UBO_DESC && cbuf->desc_set == 0 &&
          cbuf->desc_offset == ctx->fold->desc;
}

static bool
reads_folded_cbuf(const struct fold_ctx *ctx, nir_def *index)
{
   if (index->num_components != 1)
      return false;
   const nir_scalar c = nir_scalar_resolved(index, 0);
   return nir_scalar_is_const(c) && is_folded_cbuf(ctx, nir_scalar_as_uint(c));
}

static uint64_t
held_bits(const struct switch_nvk_ubo_fold *fold, uint64_t at,
          unsigned bytes, uint64_t *value)
{
   uint64_t v = 0, mask = 0;
   for (unsigned i = 0; i < bytes; i++) {
      const uint64_t byte = at + i;
      if (byte >= 64 || !(fold->mask >> byte & 1))
         continue;
      v |= (uint64_t)fold->bytes[byte] << (8 * i);
      mask |= 0xffull << (8 * i);
   }
   *value = v;
   return mask;
}

static bool
fold_load(nir_builder *b, nir_intrinsic_instr *load, void *data)
{
   const struct fold_ctx *ctx = data;
   if (load->intrinsic != nir_intrinsic_load_ubo &&
       load->intrinsic != nir_intrinsic_ldc_nv)
      return false;
   if (load->def.bit_size % 8 != 0 ||
       !reads_folded_cbuf(ctx, load->src[0].ssa))
      return false;
   const nir_scalar offset = nir_scalar_resolved(load->src[1].ssa, 0);
   if (!nir_scalar_is_const(offset))
      return false;
   int64_t at = (int64_t)nir_scalar_as_uint(offset);
   if (load->intrinsic == nir_intrinsic_ldc_nv)
      at += nir_intrinsic_base(load);
   if (at < 0)
      return false;

   const unsigned count = load->def.num_components;
   const unsigned bit_size = load->def.bit_size, bytes = bit_size / 8;
   const nir_component_mask_t read = nir_def_components_read(&load->def);
   nir_const_value values[NIR_MAX_VEC_COMPONENTS] = {0};
   nir_component_mask_t held = 0;
   for (unsigned c = 0; c < count; c++) {
      uint64_t v;
      const uint64_t mask = held_bits(ctx->fold, at + c * bytes, bytes, &v);
      values[c] = nir_const_value_for_raw_uint(v, bit_size);
      const uint64_t used = nir_def_bits_used(nir_get_scalar(&load->def, c));
      if (!(used & ~mask))
         held |= 1u << c;
   }
   if (!(held & read))
      return false;

   b->cursor = nir_after_instr(&load->instr);
   const nir_component_mask_t kept = read & ~held;
   if (!kept) {
      nir_def_replace(&load->def, nir_build_imm(b, count, bit_size, values));
      return true;
   }
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < count; c++) {
      if (!(kept >> c & 1)) {
         comps[c] = nir_build_imm(b, 1, bit_size, &values[c]);
      } else {
         comps[c] = nir_channel(b, &load->def, c);
      }
   }
   nir_def_rewrite_uses_after(&load->def, nir_vec(b, comps, count));
   return true;
}

static bool
maps_folded_buffer(const struct fold_ctx *ctx)
{
   for (uint32_t c = 0; c < ctx->map->cbuf_count; c++) {
      if (is_folded_cbuf(ctx, c))
         return true;
   }
   return false;
}

bool
switch_nvk_fold_ubo(nir_shader *nir, const struct nvk_cbuf_map *map,
                    const struct switch_nvk_ubo_fold *fold)
{
   if (map == NULL || fold == NULL || fold->mask == 0)
      return false;
   struct fold_ctx ctx = {map, fold};
   if (!maps_folded_buffer(&ctx))
      return false;
   /* An offset made of folded push words, or of held bytes, is a sum of
    * constants until folded. Each round folds a read component at most once.
    */
   bool progress = false;
   for (;;) {
      progress |= nir_opt_constant_folding(nir);
      if (!nir_shader_intrinsics_pass(nir, fold_load,
                                      nir_metadata_control_flow, &ctx))
         return progress;
      progress = true;
   }
}
