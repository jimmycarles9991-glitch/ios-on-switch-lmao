/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

/* SM53 computes on halves, which metal2vulkan carries from block to block
 * widened to f32. A 32-bit phi becomes a 16-bit one where every use narrows
 * it to nearest even (each source is narrowed in its own block, where a
 * widened half folds back), or where every source is a widened half or a
 * half constant (one widening after the phi in place of one a source). NIR's
 * nir_opt_phi_precision would also narrow a phi whose one use is a 16-bit
 * colour target's rounding toward zero (nvk_switch_round.c), which costs a
 * conversion a source where the value is no half; this pass leaves it to
 * nak_nir_strip_half_rounding.c.
 */

#include "nak_private.h"
#include "nir_builder.h"

static bool
only_narrowed(nir_phi_instr *phi)
{
   bool used = false;
   nir_foreach_use_including_if(use, &phi->def) {
      if (nir_src_is_if(use))
         return false;
      nir_instr *user = nir_src_use_instr(use);
      if (user->type != nir_instr_type_alu ||
          nir_instr_as_alu(user)->op != nir_op_f2f16_rtne)
         return false;
      used = true;
   }
   return used;
}

static bool
half_constant(nir_def *def)
{
   nir_load_const_instr *lc = nir_def_as_load_const_or_null(def);
   if (lc == NULL)
      return false;
   for (unsigned i = 0; i < def->num_components; i++) {
      if (!_mesa_float_is_half(lc->value[i].f32))
         return false;
   }
   return true;
}

static nir_alu_instr *
widening(nir_def *def)
{
   nir_alu_instr *alu = nir_def_as_alu_or_null(def);
   if (alu == NULL || alu->op != nir_op_f2f32 ||
       alu->src[0].src.ssa->bit_size != 16)
      return NULL;
   return alu;
}

static bool
only_halves(nir_phi_instr *phi)
{
   nir_foreach_phi_src(src, phi) {
      if (!half_constant(src->src.ssa) && widening(src->src.ssa) == NULL)
         return false;
   }
   return true;
}

static bool
narrow_phi(nir_builder *b, nir_phi_instr *phi, UNUSED void *data)
{
   if (phi->def.bit_size != 32)
      return false;
   const bool narrowed_uses = only_narrowed(phi);
   if (!narrowed_uses && !only_halves(phi))
      return false;

   nir_phi_instr *half = nir_phi_instr_create(b->shader);
   nir_def_init(&half->instr, &half->def, phi->def.num_components, 16);
   nir_foreach_phi_src(src, phi) {
      nir_def *value = src->src.ssa;
      nir_alu_instr *wide = widening(value);
      b->cursor = nir_after_instr_and_phis(nir_def_instr(value));
      nir_def *h = wide != NULL && !narrowed_uses
                      ? nir_ssa_for_alu_src(b, wide, 0)
                      : nir_f2f16_rtne(b, value);
      nir_phi_instr_add_src(half, src->pred, h);
   }
   b->cursor = nir_after_instr(&phi->instr);
   nir_builder_instr_insert(b, &half->instr);

   if (narrowed_uses) {
      nir_foreach_use(use, &phi->def) {
         nir_alu_instr *alu = nir_instr_as_alu(nir_src_use_instr(use));
         alu->op = nir_op_mov;
         alu->fp_math_ctrl =
            nir_op_valid_fp_math_ctrl(alu->op, alu->fp_math_ctrl);
      }
      nir_def_rewrite_uses(&phi->def, &half->def);
   } else {
      b->cursor = nir_after_phis(phi->instr.block);
      nir_def_rewrite_uses(&phi->def, nir_f2f32(b, &half->def));
   }
   return true;
}

bool
nak_nir_narrow_half_phis(nir_shader *nir)
{
   return nir_shader_phi_pass(nir, narrow_phi, nir_metadata_control_flow,
                              NULL);
}
