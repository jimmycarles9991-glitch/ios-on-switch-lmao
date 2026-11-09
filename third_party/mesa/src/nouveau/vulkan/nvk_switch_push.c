/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a render state's constant push words in its code. metal2vulkan
 * passes what a pipeline's attachments decide (each color channel's write
 * rule, nvk_switch_push.h) as push words so that one program serves many
 * pipelines; the program computes every rule and selects one per channel at
 * run time. With the words folded, NIR's constant folding keeps the rule the
 * state takes and drops the rest.
 */

#include "nvk_switch_push.h"

#include "nir_builder.h"

_Thread_local const struct switch_nvk_const_push
   *switch_nvk_const_push_current;

static bool
fold_load(nir_builder *b, nir_intrinsic_instr *load, void *data)
{
   const struct switch_nvk_const_push *push = data;
   if (load->intrinsic != nir_intrinsic_load_push_constant ||
       !nir_src_is_const(load->src[0]))
      return false;

   const unsigned bytes = load->def.bit_size / 8;
   const unsigned size = bytes * load->def.num_components;
   const uint64_t at = (uint64_t)nir_intrinsic_base(load) +
                       nir_src_as_uint(load->src[0]);
   if (at + size > 32 * 4)
      return false;
   for (uint64_t w = at / 4; w <= (at + size - 1) / 4; w++) {
      if (!(push->mask >> w & 1))
         return false;
   }

   nir_const_value values[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < load->def.num_components; c++) {
      uint64_t v = 0;
      for (unsigned i = 0; i < bytes; i++) {
         const uint64_t byte = at + c * bytes + i;
         v |= (uint64_t)(push->words[byte / 4] >> (8 * (byte % 4)) & 0xff)
              << (8 * i);
      }
      values[c] = nir_const_value_for_raw_uint(v, load->def.bit_size);
   }

   b->cursor = nir_before_instr(&load->instr);
   nir_def *imm = nir_build_imm(b, load->def.num_components,
                                load->def.bit_size, values);
   nir_def_replace(&load->def, imm);
   return true;
}

bool
switch_nvk_fold_const_push(nir_shader *nir,
                           const struct switch_nvk_const_push *push)
{
   if (push == NULL || push->mask == 0)
      return false;
   /* nir_lower_explicit_io leaves a load's offset as a sum of constants */
   bool progress = nir_opt_constant_folding(nir);
   return nir_shader_intrinsics_pass(nir, fold_load,
                                     nir_metadata_control_flow, (void *)push) ||
          progress;
}
