/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: see nvk_switch_round.h.
 */

#include "nvk_switch_round.h"

#include "nir_builder.h"

_Thread_local uint32_t switch_nvk_half_targets_current;

static bool
round_output(nir_builder *b, nir_intrinsic_instr *store, void *data)
{
   const uint32_t targets = *(const uint32_t *)data;
   if (store->intrinsic != nir_intrinsic_store_deref)
      return false;
   nir_deref_instr *deref = nir_src_as_deref(store->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var == NULL || var->data.mode != nir_var_shader_out ||
       var->data.location < FRAG_RESULT_DATA0 ||
       !(targets >> (var->data.location - FRAG_RESULT_DATA0) & 1) ||
       glsl_get_base_type(glsl_without_array(var->type)) != GLSL_TYPE_FLOAT)
      return false;

   b->cursor = nir_before_instr(&store->instr);
   nir_def *value = store->src[1].ssa;
   nir_src_rewrite(&store->src[1], nir_f2f32(b, nir_f2f16_rtne(b, value)));
   return true;
}

bool
switch_nvk_round_half_outputs(nir_shader *nir, uint32_t targets)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT || targets == 0)
      return false;
   return nir_shader_intrinsics_pass(nir, round_output,
                                     nir_metadata_control_flow, &targets);
}
