/* SPDX-License-Identifier: MIT */
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>

static void
contract(unsigned lanes, bool exact)
{
   const nir_shader_compiler_options options = {
      .float_mul_add16 = nir_float_muladd_support_has_ffma | nir_float_muladd_support_fuse,
   };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "half-fma-contract");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *a = nir_load_ssbo(&b, lanes, 16, zero, nir_imm_int(&b, 0));
   nir_def *c = nir_load_ssbo(&b, lanes, 16, zero, nir_imm_int(&b, 16));
   nir_def *d = nir_load_ssbo(&b, lanes, 16, zero, nir_imm_int(&b, 32));
   b.fp_math_ctrl = nir_fp_exact;
   nir_def *product = nir_fmul(&b, a, c);
   nir_store_ssbo(&b, product, zero, nir_imm_int(&b, 48), .write_mask = BITFIELD_MASK(lanes));
   b.fp_math_ctrl = exact ? nir_fp_exact : nir_fp_fast_math;
   nir_def *fused = nir_ffma_weak(&b, a, c, d);
   nir_store_ssbo(&b, fused, zero, nir_imm_int(&b, 64), .write_mask = BITFIELD_MASK(lanes));
   while (nir_opt_algebraic(b.shader)) {
      nir_opt_copy_prop(b.shader);
      nir_opt_dce(b.shader);
   }
   unsigned fma = 0, mul = 0, add = 0;
   nir_foreach_block(block, b.impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         fma += alu->op == nir_op_ffma;
         mul += alu->op == nir_op_fmul;
         add += alu->op == nir_op_fadd;
      }
   }
   if ((exact && (fma != 1 || mul != 1 || add != 0)) ||
       (!exact && (fma != 0 || mul == 0 || add != 1))) {
      fprintf(stderr, "lanes=%u exact=%u fma=%u mul=%u add=%u\n", lanes, exact, fma, mul, add);
      abort();
   }
   ralloc_free(b.shader);
}

int
main(void)
{
   for (unsigned lanes = 1; lanes <= 4; lanes *= 2)
      for (unsigned exact = 0; exact < 2; ++exact)
         contract(lanes, exact);
   puts("6 actual NIR half FMA contract cases passed");
   return 0;
}
