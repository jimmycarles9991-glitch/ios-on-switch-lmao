/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: see nvk_switch_xr_planes.h.
 */

#include "nvk_switch_xr_planes.h"

#include "nir_builder.h"

_Thread_local uint64_t switch_nvk_xr_plane_bindings_current;

static bool
joined_binding(nir_tex_instr *tex, uint64_t bindings)
{
   const int at = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (at < 0)
      return false;
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(tex->src[at].src));
   return var != NULL && var->data.descriptor_set == 0 && var->data.binding < 64 &&
          ((bindings >> var->data.binding) & 1);
}

/* The half nearest `x`, at `bit_size` */
static nir_def *
to_half(nir_builder *b, nir_def *x, unsigned bit_size)
{
   nir_def *h = nir_f2f16_rtne(b, x);
   return bit_size == 16 ? h : nir_f2f32(b, h);
}

/* A lane read as k / `codes` (the hardware's unorm read, a filter's weights summed over codes) on Metal's grid of 16
 * steps a code, half up (m2v lower/sampler/extended.rs: a colour lane's step 16 (c - 384), alpha's 16 b), less `zero`
 * codes, over `codes` x `scale`: each operation rounded on its own (the instructions are exact: no fused multiply-add),
 * every step an integer of f32
 */
static nir_def *
decode(nir_builder *b, nir_def *v, double codes, double zero, double scale, unsigned bit_size)
{
   nir_def *step = nir_ffloor(b, nir_fadd_imm(b, nir_fmul_imm(b, v, 16.0 * codes), 0.5));
   nir_def *x = nir_fmul_imm(b, nir_fadd_imm(b, step, -16.0 * zero), 1.0 / (16.0 * scale));
   return to_half(b, x, bit_size);
}

/* A colour lane: (c - 384) / 510 */
static nir_def *
colour(nir_builder *b, nir_def *v, unsigned bit_size)
{
   return decode(b, v, 1023.0, 384.0, 510.0, bit_size);
}

/* Alpha: the byte over 255 */
static nir_def *
alpha_of(nir_builder *b, nir_def *v, unsigned bit_size)
{
   return decode(b, v, 255.0, 0.0, 255.0, bit_size);
}

static bool
join(nir_builder *b, nir_instr *instr, void *data)
{
   const uint64_t bindings = *(const uint64_t *)data;
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if ((tex->backend_flags & SWITCH_NVK_TEX_ALPHA_PLANE) || tex->is_shadow || !joined_binding(tex, bindings))
      return false;
   switch (tex->op) {
   case nir_texop_tex:
   case nir_texop_txb:
   case nir_texop_txl:
   case nir_texop_txd:
   case nir_texop_txf:
   case nir_texop_tg4:
      break;
   default:
      return false;
   }
   if (nir_alu_type_get_base_type(tex->dest_type) != nir_type_float)
      return false;
   const unsigned bit_size = tex->def.bit_size, n = tex->def.num_components;
   tex->dest_type = nir_type_float32;
   tex->def.bit_size = 32;
   b->cursor = nir_after_instr(&tex->instr);
   const uint32_t math = b->fp_math_ctrl;
   b->fp_math_ctrl = math | nir_fp_exact;
   nir_def *lanes[NIR_MAX_VEC_COMPONENTS];
   if (tex->op == nir_texop_tg4 && tex->component == 3) {
      tex->backend_flags |= SWITCH_NVK_TEX_ALPHA_PLANE;
      tex->component = 0;
      for (unsigned i = 0; i < n; i++)
         lanes[i] = alpha_of(b, nir_channel(b, &tex->def, i), bit_size);
   } else if (tex->op == nir_texop_tg4) {
      for (unsigned i = 0; i < n; i++)
         lanes[i] = colour(b, nir_channel(b, &tex->def, i), bit_size);
   } else {
      nir_tex_instr *alpha = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
      alpha->backend_flags |= SWITCH_NVK_TEX_ALPHA_PLANE;
      nir_builder_instr_insert(b, &alpha->instr);
      for (unsigned i = 0; i < n; i++)
         lanes[i] = i < 3 ? colour(b, nir_channel(b, &tex->def, i), bit_size)
                          : alpha_of(b, nir_channel(b, &alpha->def, 0), bit_size);
   }
   b->fp_math_ctrl = math;
   nir_def *joined = nir_vec(b, lanes, n);
   nir_def_rewrite_uses_after(&tex->def, joined);
   return true;
}

bool
switch_nvk_xr_planes(nir_shader *nir, uint64_t bindings)
{
   if (!bindings || nir->info.stage != MESA_SHADER_FRAGMENT)
      return false;
   return nir_shader_instructions_pass(nir, join, nir_metadata_control_flow, &bindings);
}
