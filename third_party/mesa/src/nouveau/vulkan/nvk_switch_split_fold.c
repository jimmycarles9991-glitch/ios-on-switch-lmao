/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a shader folded to constants by NIR's own folder, for the split's
 * proof (nvk_switch_split_proof.c). Memory reads come from a model (constant
 * buffers and global memory); every other value the folder cannot compute,
 * an input, a system value or a texture fetch, becomes a hash of its
 * operation and its now constant operands, so two shaders that compute the
 * same thing fold to the same constants whatever order they do it in.
 */

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX

#include "nvk_switch_split_fold.h"

#include "nir_builder.h"
#include "nak_private.h"
#include "util/half_float.h"
#include "util/u_dynarray.h"


static uint64_t
mix(uint64_t h, uint64_t v)
{
   h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
   h ^= h >> 31;
   h *= 0xbf58476d1ce4e5b9ull;
   h ^= h >> 29;
   return h;
}

static uint64_t
leaf_value(uint64_t h, unsigned bit_size)
{
   const float unit = (float)(h >> 40) / (float)(1u << 24);
   switch (bit_size) {
   case 1:
      return h & 1;
   case 8:
      return h & 0xff;
   case 16:
      return _mesa_float_to_half(unit);
   case 32: {
      union { float f; uint32_t u; } v = {.f = unit};
      return v.u;
   }
   default: {
      union { double d; uint64_t u; } v = {.d = unit};
      return v.u;
   }
   }
}

static bool
srcs_const(nir_src *src, void *data)
{
   return nir_src_is_const(*src);
}

struct src_hash {
   uint64_t h;
};

static bool
hash_src(nir_src *src, void *data)
{
   struct src_hash *sh = data;
   for (unsigned c = 0; c < src->ssa->num_components; c++)
      sh->h = mix(sh->h, nir_src_comp_as_uint(*src, c));
   sh->h = mix(sh->h, src->ssa->bit_size);
   return true;
}

static void
replace(nir_builder *b, nir_def *def, const uint64_t *values)
{
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < def->num_components; c++)
      v[c] = nir_const_value_for_raw_uint(values[c], def->bit_size);
   b->cursor = nir_before_instr(nir_def_instr(def));
   nir_def_replace(def, nir_build_imm(b, def->num_components, def->bit_size, v));
}

static void
replace_by_hash(nir_builder *b, nir_def *def, uint64_t h)
{
   uint64_t values[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < def->num_components; c++)
      values[c] = leaf_value(mix(h, c), def->bit_size);
   replace(b, def, values);
}

static uint64_t
load_bytes(const struct switch_fold_model *m, bool global, uint64_t space,
           uint64_t at, unsigned bytes, bool *ok)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < bytes; i++) {
      const int byte = m->read(m->ctx, global, space, at + i);
      if (byte < 0)
         *ok = false;
      v |= (uint64_t)(byte & 0xff) << (8 * i);
   }
   return v;
}

/* NAK's global access (nak_nir_lower_load_store): an address, a uniform
 * address and a signed BASE from source `addr`
 */
static uint64_t
nv_address(nir_intrinsic_instr *intrin, unsigned addr)
{
   return nir_src_as_uint(intrin->src[addr]) +
          nir_src_as_uint(intrin->src[addr + 1]) +
          (int64_t)nir_intrinsic_base(intrin);
}

static bool
fold_memory(nir_builder *b, nir_intrinsic_instr *intrin,
            const struct switch_fold_model *m, bool *ok)
{
   const unsigned bytes = intrin->def.bit_size / 8;
   uint64_t values[NIR_MAX_VEC_COMPONENTS];
   bool global = false;
   uint64_t space = 0, at = 0;
   switch (intrin->intrinsic) {
   case nir_intrinsic_ldc_nv:
      space = nir_src_as_uint(intrin->src[0]);
      at = nir_intrinsic_base(intrin) + nir_src_as_uint(intrin->src[1]);
      break;
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
      global = true;
      at = nir_src_as_uint(intrin->src[0]);
      break;
   case nir_intrinsic_load_global_nv:
      global = true;
      at = nv_address(intrin, 0);
      if (!nir_src_as_bool(intrin->src[2])) {
         const uint64_t zero[NIR_MAX_VEC_COMPONENTS] = {0};
         replace(b, &intrin->def, zero);
         return true;
      }
      break;
   default:
      return false;
   }
   if (m->check_alignment && nir_intrinsic_has_align_mul(intrin) &&
       (at - nir_intrinsic_align_offset(intrin)) % nir_intrinsic_align_mul(intrin))
      *ok = false;
   for (unsigned c = 0; c < intrin->def.num_components; c++) {
      values[c] = intrin->def.bit_size == 1
                     ? load_bytes(m, global, space, at + 4 * c, 4, ok) != 0
                     : load_bytes(m, global, space, at + c * bytes, bytes, ok);
   }
   replace(b, &intrin->def, values);
   return true;
}

static void
store_global(nir_intrinsic_instr *store, const struct switch_fold_model *m,
             bool *ok)
{
   nir_def *value = store->src[0].ssa;
   const bool nv = store->intrinsic == nir_intrinsic_store_global_nv;
   const uint64_t at = nv ? nv_address(store, 1) : nir_src_as_uint(store->src[1]);
   const unsigned bytes = MAX2(value->bit_size / 8, 1);
   const unsigned mask =
      nv ? BITFIELD_MASK(value->num_components) : nir_intrinsic_write_mask(store);
   u_foreach_bit(c, mask) {
      const uint64_t v = nir_src_comp_as_uint(store->src[0], c);
      for (unsigned i = 0; i < bytes; i++)
         *ok &= m->write(m->ctx, at + c * bytes + i, v >> (8 * i));
   }
   nir_instr_remove(&store->instr);
}

/* A value the model gives: a memory read, a store to its memory, the
 * prologue's one invocation
 */
static bool
fold_value(nir_builder *b, nir_intrinsic_instr *intrin,
           const struct switch_fold_model *m, bool *ok)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_sysval_nv: {
      const unsigned sv = nir_intrinsic_base(intrin);
      uint64_t value;
      if (sv >= NAK_SV_TID_X && sv <= NAK_SV_TID_Z)
         value = m->local_id[sv - NAK_SV_TID_X];
      else if (sv >= NAK_SV_CTAID_X && sv <= NAK_SV_CTAID_Z)
         value = m->workgroup_id[sv - NAK_SV_CTAID_X];
      else
         return false;
      replace(b, &intrin->def, &value);
      return true;
   }
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_global_nv:
      store_global(intrin, m, ok);
      return true;
   case nir_intrinsic_load_workgroup_id:
   case nir_intrinsic_load_local_invocation_id: {
      const uint32_t *id = intrin->intrinsic == nir_intrinsic_load_workgroup_id
                             ? m->workgroup_id : m->local_id;
      const uint64_t value[NIR_MAX_VEC_COMPONENTS] = {id[0], id[1], id[2]};
      replace(b, &intrin->def, value);
      return true;
   }
   default:
      return fold_memory(b, intrin, m, ok);
   }
}

static void
fold_leaf_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                    const struct switch_fold_model *m)
{
   struct src_hash sh = {.h = mix(m->sample, intrin->intrinsic)};
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intrin->intrinsic];
   for (unsigned i = 0; i < info->num_indices; i++)
      sh.h = mix(sh.h, intrin->const_index[i]);
   nir_foreach_src(&intrin->instr, hash_src, &sh);
   replace_by_hash(b, &intrin->def, sh.h);
}

static void
fold_tex(nir_builder *b, nir_tex_instr *tex, const struct switch_fold_model *m)
{
   struct src_hash sh = {.h = mix(m->sample, 0x7e7)};
   sh.h = mix(sh.h, tex->op);
   sh.h = mix(sh.h, tex->texture_index);
   sh.h = mix(sh.h, tex->sampler_index);
   sh.h = mix(sh.h, tex->dest_type);
   sh.h = mix(sh.h, tex->component);
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      sh.h = mix(sh.h, tex->src[i].src_type);
      hash_src(&tex->src[i].src, &sh);
   }
   replace_by_hash(b, &tex->def, sh.h);
}

/* bcsel of a constant condition: the operand it picks, per component */
static bool
fold_bcsel(nir_builder *b, nir_alu_instr *alu)
{
   if ((alu->op != nir_op_bcsel && alu->op != nir_op_b32csel) ||
       !nir_src_is_const(alu->src[0].src))
      return false;
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   b->cursor = nir_before_instr(&alu->instr);
   for (unsigned c = 0; c < alu->def.num_components; c++) {
      const bool cond =
         nir_src_comp_as_uint(alu->src[0].src, alu->src[0].swizzle[c]) != 0;
      const nir_alu_src *pick = &alu->src[cond ? 1 : 2];
      comps[c] = nir_channel(b, pick->src.ssa, pick->swizzle[c]);
   }
   nir_def_replace(&alu->def, nir_vec(b, comps, alu->def.num_components));
   return true;
}

static bool
substitute_values(nir_shader *s, const struct switch_fold_model *m, bool *ok)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_create(impl);
   bool progress = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_undef: {
            const uint64_t zero[NIR_MAX_VEC_COMPONENTS] = {0};
            replace(&b, &nir_instr_as_undef(instr)->def, zero);
            progress = true;
            break;
         }
         case nir_instr_type_alu:
            progress |= fold_bcsel(&b, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            /* Resolve control flow before observing memory effects. */
            if (block->cf_node.parent->type != nir_cf_node_function)
               break;
            if (nir_foreach_src(instr, srcs_const, NULL))
               progress |= fold_value(&b, nir_instr_as_intrinsic(instr), m, ok);
            break;
         default:
            break;
         }
      }
   }
   nir_progress(progress, impl, nir_metadata_none);
   return progress;
}

static bool
is_leaf(nir_instr *instr)
{
   if (instr->type == nir_instr_type_tex)
      return true;
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   switch (nir_instr_as_intrinsic(instr)->intrinsic) {
   case nir_intrinsic_load_sysval_nv: {
      const unsigned sv = nir_intrinsic_base(nir_instr_as_intrinsic(instr));
      return !((sv >= NAK_SV_TID_X && sv <= NAK_SV_TID_Z) ||
               (sv >= NAK_SV_CTAID_X && sv <= NAK_SV_CTAID_Z));
   }
   case nir_intrinsic_ldc_nv:
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
   case nir_intrinsic_load_global_nv:
   case nir_intrinsic_load_workgroup_id:
   case nir_intrinsic_load_local_invocation_id:
      return false;
   default:
      return nir_intrinsic_infos[nir_instr_as_intrinsic(instr)->intrinsic].has_dest;
   }
}

/* Only leaves whose operands were constant when the pass began: NIR's
 * constant folder rewrites an operation with constant operands (a txb of
 * bias 0 becomes a tex), so a leaf is named after it had its chance.
 */
static bool
substitute_leaves(nir_shader *s, const struct switch_fold_model *m)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_create(impl);
   struct util_dynarray ready;
   util_dynarray_init(&ready, NULL);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (is_leaf(instr) && nir_foreach_src(instr, srcs_const, NULL))
            util_dynarray_append(&ready, instr);
      }
   }
   util_dynarray_foreach(&ready, nir_instr *, instr) {
      if ((*instr)->type == nir_instr_type_tex)
         fold_tex(&b, nir_instr_as_tex(*instr), m);
      else
         fold_leaf_intrinsic(&b, nir_instr_as_intrinsic(*instr), m);
   }
   const bool progress = ready.size > 0;
   util_dynarray_fini(&ready);
   nir_progress(progress, impl, nir_metadata_none);
   return progress;
}

bool
switch_fold(nir_shader *s, const struct switch_fold_model *m)
{
   const nir_shader_compiler_options *own = s->options;
   nir_shader_compiler_options options = *own;
   options.max_unroll_iterations = 1 << 16;
   options.max_unroll_iterations_aggressive = 1 << 16;
   s->options = &options;

   bool ok = true;
   for (unsigned round = 0; round < 4096 && ok; round++) {
      bool progress = substitute_values(s, m, &ok);
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_copy_prop(s);
      progress |= substitute_leaves(s, m);
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_dead_cf(s);
      progress |= nir_opt_remove_phis(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_loop_unroll(s);
      if (!progress)
         break;
   }
   s->options = own;
   return ok;
}

#endif
