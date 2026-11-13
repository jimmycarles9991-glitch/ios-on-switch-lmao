/* SPDX-License-Identifier: MIT */
/* nvk_switch_turn.c over hand-built fragment programs: a turned program's
 * position, with the root table's turn words of each rotation folded in, is
 * the frame's position of the plane's pixel (Metal's store map,
 * raw/runtime/raw_rotation.h); fwidth and an image access's coordinate are
 * left as they are, a lone derivative is not.
 */
#include "nvk_switch_turn.h"

#include "nir_builder.h"

#include <stdio.h>
#include <stdlib.h>

#define ROOT_TURN 0x268
#define SINK 0x1000

static const nir_shader_compiler_options options = {0};
static int failures;

static void
check(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

static uint32_t
bits(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

/* raw_rotation.h turnWords for a frame of w x h */
static void
turn_words(unsigned turn, float w, float h, uint32_t out[8])
{
   float sx = 1, sy = 1, tx = 0, ty = 0;
   uint32_t swap = 0;
   if (turn == 1 || turn == 4) {
      swap = 1, sy = -1, ty = h;
   } else if (turn == 2) {
      sx = -1, tx = w, sy = -1, ty = h;
   } else if (turn == 3) {
      swap = 1, sx = -1, tx = w;
   }
   out[0] = swap;
   out[1] = bits(sx);
   out[2] = bits(sy);
   out[3] = bits(tx);
   out[4] = bits(ty);
   out[5] = bits(sx < 0 ? 1.0f : 0.0f);
   out[6] = bits(sy < 0 ? 1.0f : 0.0f);
   out[7] = 0;
}

static unsigned
count(nir_shader *s, nir_intrinsic_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(instr)->intrinsic == op)
               n++;
         }
      }
   }
   return n;
}

static void
clean(nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_dce);
   } while (progress);
}

/* The program's frame position at plane position (x, y) under `words` */
static void
position_at(nir_shader *s, float x, float y, const uint32_t words[8], float out[2])
{
   nir_foreach_function_impl(impl, s) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            b.cursor = nir_before_instr(instr);
            if (intrin->intrinsic == nir_intrinsic_load_frag_coord) {
               nir_def_replace(&intrin->def, nir_imm_vec4(&b, x, y, 0.5f, 1.0f));
            } else if (intrin->intrinsic == nir_intrinsic_ldc_nv) {
               const unsigned at = (nir_intrinsic_base(intrin) - ROOT_TURN) / 4;
               nir_def_replace(&intrin->def, nir_imm_int(&b, at < 8 ? words[at] : 0));
            }
         }
      }
   }
   clean(s);
   out[0] = out[1] = -1;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic ||
                nir_instr_as_intrinsic(instr)->intrinsic != nir_intrinsic_store_global)
               continue;
            nir_intrinsic_instr *store = nir_instr_as_intrinsic(instr);
            if (!nir_src_is_const(store->src[0]))
               continue;
            out[0] = nir_src_comp_as_float(store->src[0], 0);
            out[1] = nir_src_comp_as_float(store->src[0], 1);
         }
      }
   }
}

static nir_shader *
position_program(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "turn-position");
   nir_def *position = nir_load_frag_coord(&b);
   nir_store_global(&b, nir_channels(&b, position, 0x3), nir_imm_int64(&b, SINK), .align_mul = 4);
   return b.shader;
}

static void
test_position(void)
{
   const unsigned w = 96, h = 64;
   for (unsigned turn = 1; turn <= 4; turn++) {
      const bool quarter = turn != 2;
      const unsigned pw = quarter ? h : w, ph = quarter ? w : h;
      uint32_t words[8];
      turn_words(turn, w, h, words);
      for (unsigned px = 0; px < pw; px += 7) {
         for (unsigned py = 0; py < ph; py += 5) {
            nir_shader *s = position_program();
            switch_nvk_turn_fragment(s, ROOT_TURN);
            float got[2];
            position_at(s, px + 0.5f, py + 0.5f, words, got);
            /* Metal's store: frame texel (x, y) lands in the plane at (h-1-y, x), (w-1-x, h-1-y) or (y, w-1-x) */
            unsigned x, y;
            if (turn == 2) {
               x = w - 1 - px, y = h - 1 - py;
            } else if (turn == 3) {
               x = w - 1 - py, y = px;
            } else {
               x = py, y = h - 1 - px;
            }
            char what[96];
            snprintf(what, sizeof(what), "rotation %u plane (%u, %u): frame (%g, %g), want (%u.5, %u.5)", turn,
                     px, py, got[0], got[1], x, y);
            check(got[0] == x + 0.5f && got[1] == y + 0.5f, what);
            ralloc_free(s);
         }
      }
   }
}

static nir_shader *
derivative_program(bool fwidth)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "turn-derivative");
   nir_def *v = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .base = 0, .range = 8);
   nir_def *out = fwidth ? nir_fadd(&b, nir_fabs(&b, nir_ddx(&b, v)), nir_fabs(&b, nir_ddy(&b, v)))
                         : nir_vec2(&b, nir_channel(&b, nir_ddx(&b, v), 0), nir_channel(&b, nir_ddy(&b, v), 1));
   nir_store_global(&b, out, nir_imm_int64(&b, SINK), .align_mul = 4);
   return b.shader;
}

static void
test_derivatives(void)
{
   nir_shader *fw = derivative_program(true);
   switch_nvk_turn_fragment(fw, ROOT_TURN);
   clean(fw);
   check(count(fw, nir_intrinsic_ldc_nv) == 0, "fwidth reads no turn word");
   check(count(fw, nir_intrinsic_ddx) == 1 && count(fw, nir_intrinsic_ddy) == 1, "fwidth keeps its derivatives");
   ralloc_free(fw);

   nir_shader *d = derivative_program(false);
   switch_nvk_turn_fragment(d, ROOT_TURN);
   clean(d);
   check(count(d, nir_intrinsic_ldc_nv) == 3, "a lone derivative reads swap, sx and sy");
   ralloc_free(d);
}

static void
test_image_address(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "turn-fetch");
   nir_def *p = nir_load_frag_coord(&b);
   nir_def *coord = nir_vec4(&b, nir_f2i32(&b, nir_channel(&b, p, 0)), nir_f2i32(&b, nir_channel(&b, p, 1)),
                             nir_imm_int(&b, 0), nir_imm_int(&b, 0));
   nir_def *texel = nir_image_load(&b, 4, 32, nir_imm_int(&b, 0), coord, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                                   .image_dim = GLSL_SAMPLER_DIM_2D);
   nir_store_global(&b, texel, nir_imm_int64(&b, SINK), .align_mul = 4);
   switch_nvk_turn_fragment(b.shader, ROOT_TURN);
   clean(b.shader);
   check(count(b.shader, nir_intrinsic_ldc_nv) == 0, "a fetch's address stays the plane's");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   test_position();
   test_derivatives();
   test_image_address();
   glsl_type_singleton_decref();
   if (failures)
      return 1;
   puts("turned positions of every rotation, fwidth, a lone derivative and a fetch's address passed");
   return 0;
}
