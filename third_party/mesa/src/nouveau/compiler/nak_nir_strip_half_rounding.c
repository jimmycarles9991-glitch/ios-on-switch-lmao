/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

/* A float output to a 16-bit float colour target (bit j of half_targets for
 * attachment j) arrives as f2f32(f2f16_rtz(x)) (NVK's nvk_switch_round.c).
 * The late rules fold the pair into x where x was a half all along; where
 * they did not, it goes, as GM20B's ROP converts x toward zero itself, which
 * is what Apple's GPU stores at textureWriteRoundingMode 0 (an M4 Max, all
 * 47,613 floats whose two roundings differ; metal2vulkan's
 * pipeline/color_writes.rs has the same of an M3 Max and an M4).
 */

#include "nak_private.h"
#include "nir_builder.h"

static bool
strip_output(nir_builder *b, nir_intrinsic_instr *out, void *data)
{
   const uint8_t half_targets = *(const uint8_t *)data;
   if (out->intrinsic != nir_intrinsic_fs_out_nv)
      return false;

   const unsigned base = nir_intrinsic_base(out);
   if (base >= NAK_FS_OUT_SAMPLE_MASK || !(half_targets >> (base / 16) & 1))
      return false;

   nir_scalar wide = nir_scalar_resolved(out->src[0].ssa, 0);
   if (!nir_scalar_is_alu(wide) || nir_scalar_alu_op(wide) != nir_op_f2f32)
      return false;
   nir_scalar narrow =
      nir_scalar_chase_movs(nir_scalar_chase_alu_src(wide, 0));
   if (!nir_scalar_is_alu(narrow) ||
       nir_scalar_alu_op(narrow) != nir_op_f2f16_rtz)
      return false;
   nir_scalar x = nir_scalar_chase_alu_src(narrow, 0);

   b->cursor = nir_before_instr(&out->instr);
   nir_src_rewrite(&out->src[0], nir_channel(b, x.def, x.comp));
   return true;
}

bool
nak_nir_strip_half_rounding(nir_shader *nir, uint8_t half_targets)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT || half_targets == 0)
      return false;
   return nir_shader_intrinsics_pass(nir, strip_output,
                                     nir_metadata_control_flow, &half_targets);
}
