/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the NIR side of a split fragment program (nvk_switch_split.h).
 *
 * A value is per-draw when its instruction is a constant, an undef, a read of
 * a constant buffer the prologue can read as the draw binds it (the root
 * table's push words at a constant offset, a set-0 uniform buffer, the
 * shader data) at a per-draw offset, or ALU of per-draw values. Phis and every
 * other intrinsic are per-pixel. The frontier is each per-draw ALU value a
 * per-pixel instruction or an if reads, but moves, vectors, negations and
 * absolute values stay where they are: NAK folds them into the operand that
 * reads the constant buffer.
 */

#include "nvk_switch_split_private.h"

#include "nvk_switch_capture.h"

#include "nir_builder.h"

static bool
def_per_draw(const struct split *s, const nir_def *def)
{
   return BITSET_TEST(s->per_draw, def->index);
}

static bool
src_per_draw(nir_src *src, void *data)
{
   return def_per_draw(data, src->ssa);
}

static bool
ldc_per_draw(struct split *s, nir_intrinsic_instr *ldc)
{
   if (!nir_src_is_const(ldc->src[0]) || !def_per_draw(s, ldc->src[1].ssa))
      return false;
   const uint32_t cbuf = nir_src_as_uint(ldc->src[0]);
   if (cbuf >= s->map->cbuf_count)
      return false;

   const struct nvk_cbuf *c = &s->map->cbufs[cbuf];
   switch (c->type) {
   case NVK_CBUF_TYPE_SHADER_DATA:
      return true;
   case NVK_CBUF_TYPE_UBO_DESC:
      return switch_nvk_cbuf_code(c) != 7;
   case NVK_CBUF_TYPE_ROOT_DESC: {
      if (!nir_src_is_const(ldc->src[1]))
         return false;
      const uint64_t at = nir_intrinsic_base(ldc) + nir_src_as_uint(ldc->src[1]);
      const uint32_t bytes = ldc->def.num_components * ldc->def.bit_size / 8;
      return at >= PUSH_START &&
             at + bytes <= PUSH_START + SWITCH_SPLIT_PUSH_BYTES;
   }
   default:
      return false;
   }
}

static bool
instr_per_draw(struct split *s, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      return true;
   case nir_instr_type_alu:
      return nir_foreach_src(instr, src_per_draw, s);
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      return intrin->intrinsic == nir_intrinsic_ldc_nv && ldc_per_draw(s, intrin);
   }
   default:
      return false;
   }
}

static bool
stays_in_fragment(const nir_instr *instr)
{
   if (instr->type != nir_instr_type_alu)
      return true;
   const nir_op op = nir_instr_as_alu(instr)->op;
   return op == nir_op_mov || nir_op_is_vec(op) || op == nir_op_fneg ||
          op == nir_op_fabs || op == nir_op_ineg;
}

static bool
read_per_pixel(const struct split *s, nir_def *def)
{
   nir_foreach_use_including_if(src, def) {
      if (nir_src_is_if(src))
         return true;
      nir_def *user = nir_instr_def(nir_src_use_instr(src));
      if (user == NULL || !def_per_draw(s, user))
         return true;
   }
   return false;
}

static bool
add_frontier(struct split *s, nir_def *def)
{
   if (BITSET_TEST(s->frontier, def->index))
      return true;
   const unsigned bytes = slot_bytes(def);
   const nir_component_mask_t read = nir_def_components_read(def);
   s->frontier_bytes = ALIGN_POT(s->frontier_bytes, bytes);
   s->slot[def->index] = s->frontier_bytes;
   s->frontier_bytes += util_last_bit(read) * bytes;
   s->frontier_values++;
   BITSET_SET(s->frontier, def->index);
   return s->frontier_bytes <= SWITCH_SPLIT_MAX_FRONTIER_BYTES;
}

static bool
choose_frontier(struct split *s, nir_def *def);

static bool
choose_src(nir_src *src, void *data)
{
   return choose_frontier(data, src->ssa);
}

/* A per-draw value per-pixel code reads: the frontier holds it, or it stays
 * and what it reads is chosen in turn.
 */
static bool
choose_frontier(struct split *s, nir_def *def)
{
   if (stays_in_fragment(nir_def_instr(def)))
      return nir_foreach_src(nir_def_instr(def), choose_src, s);
   return add_frontier(s, def);
}

static void
classify(struct split *s)
{
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def != NULL && instr_per_draw(s, instr))
            BITSET_SET(s->per_draw, def->index);
      }
   }
}

static bool
select_frontier(struct split *s)
{
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def != NULL && def_per_draw(s, def) && read_per_pixel(s, def) &&
             !choose_frontier(s, def))
            return false;
      }
   }
   return s->frontier_values > 0;
}

static bool
mark_computed_src(nir_src *src, void *data)
{
   struct split *s = data;
   BITSET_SET(s->computed, src->ssa->index);
   return true;
}

static void
mark_computed(struct split *s)
{
   memcpy(s->computed, s->frontier,
          BITSET_WORDS(s->impl->ssa_alloc) * sizeof(BITSET_WORD));
   nir_foreach_block_reverse(block, s->impl) {
      nir_foreach_instr_reverse(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def == NULL || !BITSET_TEST(s->computed, def->index))
            continue;
         nir_foreach_src(instr, mark_computed_src, s);
         if (instr->type == nir_instr_type_alu)
            s->computed_instrs++;
      }
   }
}

static void
read_frontier(struct split *s, nir_builder *b, nir_def *def)
{
   const nir_component_mask_t read = nir_def_components_read(def);
   const unsigned bytes = slot_bytes(def);
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < def->num_components; c++) {
      if (!(read & BITFIELD_BIT(c))) {
         comps[c] = nir_undef(b, 1, def->bit_size);
         continue;
      }
      nir_def *v = nir_ldc_nv(b, 1, bytes * 8,
                              nir_imm_int(b, s->layout->frontier_cbuf),
                              nir_imm_int(b, 0),
                              .base = s->slot[def->index] + c * bytes,
                              .align_mul = bytes);
      if (def->bit_size == 1)
         v = nir_ine_imm(b, v, 0);
      else if (def->bit_size < 32)
         v = nir_u2uN(b, v, def->bit_size);
      comps[c] = v;
   }
   nir_def_rewrite_uses(def, nir_vec(b, comps, def->num_components));
}

static void
rewrite_fragment(struct split *s, void *mem)
{
   nir_def **defs = ralloc_array(mem, nir_def *, s->frontier_values);
   unsigned n = 0;
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def != NULL && BITSET_TEST(s->frontier, def->index))
            defs[n++] = def;
      }
   }
   nir_builder b = nir_builder_create(s->impl);
   for (unsigned i = 0; i < n; i++) {
      b.cursor = nir_after_instr(nir_def_instr(defs[i]));
      read_frontier(s, &b, defs[i]);
   }
   nir_progress(true, s->impl, nir_metadata_control_flow);
   nir_opt_dce(s->fs);
   nir_validate_shader(s->fs, "switch split fragment");
}

/* NVK's lowering leaves constant buffer indices as moves of vectors of
 * constants, which NAK folds later; folded now, the reads are seen as reads
 * of the buffers they are.
 */
static void
fold_constants(nir_shader *fs)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, fs, nir_opt_constant_folding);
      NIR_PASS(progress, fs, nir_opt_copy_prop);
      NIR_PASS(progress, fs, nir_opt_cse);
      NIR_PASS(progress, fs, nir_opt_dce);
   } while (progress);
}

bool
switch_nvk_split_nir(const nir_shader *fs, const struct nvk_cbuf_map *map,
                     struct switch_nvk_split_nir *out)
{
   memset(out, 0, sizeof(*out));
   if (map->cbuf_count >= ARRAY_SIZE(map->cbufs))
      return false;

   void *mem = ralloc_context(NULL);
   struct split s = {
      .fs = nir_shader_clone(NULL, fs),
      .map = map,
      .layout = &out->layout,
   };
   fold_constants(s.fs);
   s.impl = nir_shader_get_entrypoint(s.fs);
   nir_index_ssa_defs(s.impl);
   const unsigned words = BITSET_WORDS(s.impl->ssa_alloc);
   s.per_draw = rzalloc_array(mem, BITSET_WORD, words);
   s.frontier = rzalloc_array(mem, BITSET_WORD, words);
   s.computed = rzalloc_array(mem, BITSET_WORD, words);
   s.slot = rzalloc_array(mem, uint32_t, s.impl->ssa_alloc);

   classify(&s);
   bool split = select_frontier(&s);
   if (split) {
      mark_computed(&s);
      split = switch_split_lay_out_cbufs(&s);
      out->layout.frontier_cbuf = map->cbuf_count;
      out->layout.frontier_bytes = ALIGN_POT(s.frontier_bytes, 16);
      out->layout.frontier_values = s.frontier_values;
      out->layout.prologue_instrs = s.computed_instrs;
   }
   if (split) {
      out->prologue = switch_split_build_prologue(&s);
      rewrite_fragment(&s, mem);
      out->fs = s.fs;
   } else {
      ralloc_free(s.fs);
   }
   ralloc_free(mem);
   return out->fs != NULL;
}
