/* SPDX-License-Identifier: MIT */
#include "nak_private.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>

static const struct nir_shader_compiler_options options = {0};

static void
require(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "%s\n", what);
      abort();
   }
}

static unsigned
count_alu(nir_shader *nir, nir_op op, unsigned bit_size)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            n += alu->op == op && (!bit_size || alu->def.bit_size == bit_size);
         }
      }
   }
   return n;
}

static nir_intrinsic_instr *
find_intrinsic(nir_shader *nir, nir_intrinsic_op op)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic == op)
               return nir_instr_as_intrinsic(instr);
         }
      }
   }
   abort();
}

/* metal2vulkan's tests of |x|'s bits against K, and their integer answers */
enum { MAG_LT_K, MAG_GE_K, K_LT_MAG, K_GE_MAG, MAG_EQ_K, MAG_NE_K, TESTS };

static nir_def *
integer_test(nir_builder *b, unsigned test, nir_def *mag, nir_def *k)
{
   switch (test) {
   case MAG_LT_K: return nir_ult(b, mag, k);
   case MAG_GE_K: return nir_uge(b, mag, k);
   case K_LT_MAG: return nir_ult(b, k, mag);
   case K_GE_MAG: return nir_uge(b, k, mag);
   case MAG_EQ_K: return nir_ieq(b, mag, k);
   default: return nir_ine(b, mag, k);
   }
}

static bool
integer_answer(unsigned test, uint32_t mag, uint32_t k)
{
   switch (test) {
   case MAG_LT_K: return mag < k;
   case MAG_GE_K: return mag >= k;
   case K_LT_MAG: return k < mag;
   case K_GE_MAG: return k >= mag;
   case MAG_EQ_K: return mag == k;
   default: return mag != k;
   }
}

/* nak_nir_lower_float_tests rewrites the test as a float compare, which NIR's
 * constant folding then evaluates: it must give the integer test's answer
 */
static void
float_test(unsigned test, uint32_t k, uint32_t x, bool ftz)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "float-test");
   if (ftz)
      b.shader->info.float_controls_execution_mode = FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32;
   nir_def *mag = nir_iand_imm(&b, nir_imm_int(&b, x), 0x7fffffff);
   nir_def *answer = integer_test(&b, test, mag, nir_imm_int(&b, k));
   nir_def *zero = nir_imm_int(&b, 0);
   nir_store_ssbo(&b, nir_b2i32(&b, answer), zero, zero);
   require(nak_nir_lower_float_tests(b.shader), "an integer float test left as it was");
   nir_opt_dce(b.shader);
   require(count_alu(b.shader, nir_op_iand, 0) == 0, "the magnitude mask kept");
   nir_opt_constant_folding(b.shader);
   nir_src stored = find_intrinsic(b.shader, nir_intrinsic_store_ssbo)->src[0];
   if (!nir_src_is_const(stored) || nir_src_as_uint(stored) != integer_answer(test, x & 0x7fffffff, k)) {
      fprintf(stderr, "test %u K %08x x %08x ftz %u\n", test, k, x, ftz);
      abort();
   }
   ralloc_free(b.shader);
}

static nir_def *
nan_guarded_texel(nir_builder *b, nir_def *x)
{
   nir_def *nan = nir_ult(b, nir_imm_int(b, 0x7f800000), nir_iand_imm(b, x, 0x7fffffff));
   return nir_bcsel(b, nan, x, nir_f2f32(b, nir_f2f16_rtne(b, x)));
}

static nir_def *
not_a_half(nir_builder *b, nir_def *x)
{
   return nir_fmul_imm(b, x, 3.0);
}

/* NAK's late loop on SM53: its rules and its narrowing of half phis */
static void
late_rules(nir_shader *nir)
{
   const struct nak_compiler nak = {.sm = 53};
   while (nak_nir_lower_algebraic_late(nir, &nak) | nak_nir_narrow_half_phis(nir)) {
      nir_opt_constant_folding(nir);
      nir_opt_copy_prop(nir);
      nir_opt_dce(nir);
   }
}

/* f2f32(f2f16_rtz(c ? constant : arm(x))), as nvk_switch_round.c leaves a
 * half target's output, after the late rules
 */
static nir_shader *
half_target_output(nir_def *(*arm)(nir_builder *, nir_def *), float constant)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "half-output");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *c = nir_ine_imm(&b, nir_load_ssbo(&b, 1, 32, zero, nir_imm_int(&b, 4)), 0);
   nir_def *v = nir_bcsel(&b, c, nir_imm_float(&b, constant), arm(&b, x));
   nir_store_ssbo(&b, nir_f2f32(&b, nir_f2f16_rtz(&b, v)), zero, nir_imm_int(&b, 8));
   late_rules(b.shader);
   return b.shader;
}

static unsigned
count_phis(nir_shader *nir, unsigned bit_size)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_phi(phi, block)
            n += phi->def.bit_size == bit_size;
      }
   }
   return n;
}

/* A phi of two values, halves held widened or floats that are no half, read
 * through `use`, after the late loop
 */
static nir_shader *
phi_of(bool halves, nir_op use)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "half-phi");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *h = nir_f2f16_rtne(&b, x);
   nir_push_if(&b, nir_ine_imm(&b, nir_load_ssbo(&b, 1, 32, zero, nir_imm_int(&b, 4)), 0));
   nir_def *then_value = halves ? nir_f2f32(&b, h) : nir_fmul_imm(&b, x, 3.0);
   nir_push_else(&b, NULL);
   nir_def *else_value = halves ? nir_f2f32(&b, nir_fneg(&b, h)) : nir_fmul_imm(&b, x, 5.0);
   nir_pop_if(&b, NULL);
   nir_def *phi = nir_if_phi(&b, then_value, else_value);
   nir_store_ssbo(&b, nir_build_alu1(&b, use, phi), zero, nir_imm_int(&b, 8));
   late_rules(b.shader);
   return b.shader;
}

static void
half_phis(void)
{
   nir_shader *halves = phi_of(true, nir_op_f2f16_rtne);
   require(count_phis(halves, 16) == 1 && count_phis(halves, 32) == 0, "a phi of halves left 32-bit");
   require(count_alu(halves, nir_op_f2f32, 0) == 0, "a half widened for its phi");
   require(count_alu(halves, nir_op_f2f16_rtne, 0) == 1, "a half narrowed again after its phi");
   ralloc_free(halves);

   nir_shader *output = phi_of(false, nir_op_f2f16_rtz);
   require(count_phis(output, 32) == 1 && count_alu(output, nir_op_f2f16_rtz, 0) == 1,
           "a half target's rounding moved onto a phi's float sources");
   ralloc_free(output);
}

/* textureWriteRoundingMode 2: metal2vulkan narrows a half target's output to nearest even itself
 * (pipeline/color_writes.rs HALF_NEAREST); nvk_switch_round.c's pair on that half folds away
 * (f2f16_rtz(f2f32(a@16)) -> a) and leaves its rounding
 */
static void
nearest_even_output_survives(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "mode-2-output");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *rounded = nir_f2f32(&b, nir_f2f16_rtne(&b, x));
   nir_store_ssbo(&b, nir_f2f32(&b, nir_f2f16_rtz(&b, rounded)), zero, nir_imm_int(&b, 8));
   late_rules(b.shader);
   require(count_alu(b.shader, nir_op_f2f16_rtz, 0) == 0, "a nearest-even half rounded toward zero again");
   require(count_alu(b.shader, nir_op_f2f16_rtne, 0) == 1, "the translator's nearest-even rounding lost");
   ralloc_free(b.shader);
}

static void
half_target_rounding(void)
{
   nir_shader *texel = half_target_output(nan_guarded_texel, 0.0f);
   require(count_alu(texel, nir_op_f2f16_rtz, 0) == 0, "a texel's output kept its rounding");
   require(count_alu(texel, nir_op_f2f16_rtne, 0) == 1, "a texel not narrowed once");
   require(count_alu(texel, nir_op_ult, 0) == 0, "a texel's NaN guard kept");
   require(count_alu(texel, nir_op_bcsel, 16) == 1, "the select not made of halves");
   ralloc_free(texel);

   nir_shader *wide = half_target_output(not_a_half, 0.0f);
   require(count_alu(wide, nir_op_f2f16_rtz, 0) == 1 && count_alu(wide, nir_op_bcsel, 32) == 1,
           "a float that is not a half narrowed through its select");
   ralloc_free(wide);

   nir_shader *odd = half_target_output(nan_guarded_texel, 0.3f);
   require(count_alu(odd, nir_op_f2f16_rtz, 0) == 1, "a constant that is not a half rounded");
   ralloc_free(odd);
}

/* nak_nir_strip_half_rounding: an output to a half target reads the float the
 * pair rounds, any other output keeps it
 */
static bool
strips(uint8_t half_targets)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "strip");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *y = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_fs_out_nv(&b, nir_f2f32(&b, nir_f2f16_rtz(&b, y)), .base = NAK_FS_OUT_COLOR1 + 8);
   const bool progress = nak_nir_strip_half_rounding(b.shader, half_targets);
   nir_intrinsic_instr *out = find_intrinsic(b.shader, nir_intrinsic_fs_out_nv);
   const bool reads_y = nir_scalar_resolved(out->src[0].ssa, 0).def == y;
   require(progress == reads_y, "progress and the output disagree");
   ralloc_free(b.shader);
   return reads_y;
}

int
main(void)
{
   static const uint32_t bounds[] = {0x00800000, 0x38800000, 0x477fe000, 0x7f800000};
   static const uint32_t xs[] = {
      0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x00800001, 0x387fffff,
      0x38800000, 0x38800001, 0x477fe000, 0x477fefff, 0x477ff000, 0x3f800000, 0xbf800000,
      0x7f7fffff, 0x7f800000, 0xff800000, 0x7f800001, 0x7fc00000, 0xffffffff,
   };
   unsigned n = 0;
   for (unsigned test = 0; test < TESTS; test++)
      for (unsigned k = 0; k < ARRAY_SIZE(bounds); k++)
         for (unsigned x = 0; x < ARRAY_SIZE(xs); x++)
            for (unsigned ftz = 0; ftz < 2; ftz++, n++)
               float_test(test, bounds[k], xs[x], ftz);
   half_target_rounding();
   nearest_even_output_survives();
   half_phis();
   require(strips(1u << 1), "a half target's output kept its rounding");
   require(!strips(1u << 0), "another target's output lost its rounding");
   printf("%u float tests, 3 half target outputs, 2 phis and 2 strips passed\n", n);
   return 0;
}
