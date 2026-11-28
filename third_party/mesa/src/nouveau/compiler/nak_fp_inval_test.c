/* SPDX-License-Identifier: MIT */
#include "nir_builder.h"
#include "nir_range_analysis.h"
#include <stdio.h>
#include <stdlib.h>

static const struct nir_shader_compiler_options options = {0};

static void
require(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "fp_inval_test failed: %s\n", what);
      abort();
   }
}

static unsigned
count_alu_total(nir_shader *nir)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu)
               n++;
         }
      }
   }
   return n;
}

static unsigned
count_alu_op(nir_shader *nir, nir_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               n++;
         }
      }
   }
   return n;
}

static fp_class_mask
fresh_fp_class(nir_function_impl *impl, const nir_def *def)
{
   nir_fp_analysis_state fresh = nir_create_fp_analysis_state(impl);
   fp_class_mask mask = nir_analyze_fp_class(&fresh, def);
   nir_free_fp_analysis_state(&fresh);
   return mask;
}

static void
test_transitive_users_and_sibling_preserved(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "fp-inval-chain");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *sib = nir_load_ssbo(&b, 1, 32, zero, nir_imm_int(&b, 4));
   nir_def *sib_sat = nir_fsat(&b, sib);

   nir_def *a = nir_fneg(&b, x);
   nir_def *c = nir_fadd(&b, a, nir_imm_float(&b, 1.0f));
   nir_def *d = nir_fsat(&b, c);
   nir_def *e = nir_fmul(&b, d, nir_imm_float(&b, 2.0f));
   nir_store_ssbo(&b, e, zero, nir_imm_int(&b, 8));
   nir_store_ssbo(&b, sib_sat, zero, nir_imm_int(&b, 12));
   nir_validate_shader(b.shader, "fp-inval-chain-init");

   nir_fp_analysis_state state = nir_create_fp_analysis_state(b.impl);
   fp_class_mask old_e = nir_analyze_fp_class(&state, e);
   fp_class_mask sib_cls = nir_analyze_fp_class(&state, sib_sat);

   require(BITSET_TEST(state.bitset, x->index), "x cached");
   require(BITSET_TEST(state.bitset, a->index), "a cached");
   require(BITSET_TEST(state.bitset, c->index), "c cached");
   require(BITSET_TEST(state.bitset, d->index), "d cached");
   require(BITSET_TEST(state.bitset, e->index), "e cached");
   require(BITSET_TEST(state.bitset, sib->index), "sib cached");
   require(BITSET_TEST(state.bitset, sib_sat->index), "sib_sat cached");

   b.cursor = nir_before_def(a);
   nir_def *neg_one = nir_imm_float(&b, -1.0f);
   nir_invalidate_fp_analysis_users(&state, x);
   nir_def_rewrite_uses(x, neg_one);
   nir_grow_fp_analysis_state(&state);
   nir_validate_shader(b.shader, "fp-inval-chain-rewrite");

   require(!BITSET_TEST(state.bitset, x->index), "x cleared");
   require(!BITSET_TEST(state.bitset, a->index), "a cleared");
   require(!BITSET_TEST(state.bitset, c->index), "c cleared");
   require(!BITSET_TEST(state.bitset, d->index), "d cleared");
   require(!BITSET_TEST(state.bitset, e->index), "e cleared");
   require(BITSET_TEST(state.bitset, sib->index), "sib preserved");
   require(BITSET_TEST(state.bitset, sib_sat->index), "sib_sat preserved");

   fp_class_mask new_e = nir_analyze_fp_class(&state, e);
   fp_class_mask fresh_e = fresh_fp_class(b.impl, e);
   require(new_e == fresh_e, "recomputed e matches fresh analysis");
   require(new_e != old_e, "recomputed e tightened after rewrite");
   require(nir_analyze_fp_class(&state, sib_sat) == sib_cls, "sibling class unchanged");

   nir_free_fp_analysis_state(&state);
   ralloc_free(b.shader);
}

static void
test_fdot_neg_lookthrough(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "fp-inval-fdot");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *v2 = nir_load_ssbo(&b, 2, 32, zero, zero);
   nir_def *w2 = nir_load_ssbo(&b, 2, 32, zero, nir_imm_int(&b, 8));
   nir_def *neg_w2 = nir_fneg(&b, w2);
   nir_def *d2 = nir_build_alu2(&b, nir_op_fdot2, v2, neg_w2);

   nir_def *v4 = nir_load_ssbo(&b, 4, 32, zero, nir_imm_int(&b, 16));
   nir_def *w4 = nir_load_ssbo(&b, 4, 32, zero, nir_imm_int(&b, 32));
   nir_def *neg_w4 = nir_fneg(&b, w4);
   nir_def *d4 = nir_build_alu2(&b, nir_op_fdot4_replicated, v4, neg_w4);

   nir_store_ssbo(&b, d2, zero, nir_imm_int(&b, 48));
   nir_store_ssbo(&b, nir_channel(&b, d4, 0), zero, nir_imm_int(&b, 52));
   nir_validate_shader(b.shader, "fp-inval-fdot-init");

   nir_fp_analysis_state state = nir_create_fp_analysis_state(b.impl);
   fp_class_mask old_d2 = nir_analyze_fp_class(&state, d2);
   fp_class_mask old_d4 = nir_analyze_fp_class(&state, d4);
   require(old_d2 == FP_CLASS_UNKNOWN, "initial fdot2 unknown");
   require(old_d4 == FP_CLASS_UNKNOWN, "initial fdot4_replicated unknown");
   require(BITSET_TEST(state.bitset, neg_w2->index) && BITSET_TEST(state.bitset, w2->index),
           "fdot2 caches src1 and its fneg operand");
   require(BITSET_TEST(state.bitset, neg_w4->index) && BITSET_TEST(state.bitset, w4->index),
           "fdot4_replicated caches src1 and its fneg operand");

   nir_invalidate_fp_analysis_users(&state, w2);
   nir_def_rewrite_uses(w2, v2);
   nir_invalidate_fp_analysis_users(&state, w4);
   nir_def_rewrite_uses(w4, v4);
   nir_validate_shader(b.shader, "fp-inval-fdot-rewrite");

   require(!BITSET_TEST(state.bitset, d2->index), "fdot2 invalidated through fneg");
   require(!BITSET_TEST(state.bitset, d4->index), "fdot4_replicated invalidated through fneg");

   fp_class_mask new_d2 = nir_analyze_fp_class(&state, d2);
   fp_class_mask new_d4 = nir_analyze_fp_class(&state, d4);
   require(new_d2 == fresh_fp_class(b.impl, d2), "fdot2 matches fresh analysis");
   require(new_d4 == fresh_fp_class(b.impl, d4), "fdot4_replicated matches fresh analysis");
   require((new_d2 & FP_CLASS_ANY_POS) == 0, "fdot2(v, -v) is non-positive");
   require((new_d4 & FP_CLASS_ANY_POS) == 0, "fdot4_replicated(v, -v) is non-positive");

   nir_free_fp_analysis_state(&state);
   ralloc_free(b.shader);
}

static void
test_loop_phi_cycle(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "fp-inval-loop");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *init = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *cond = nir_ine_imm(&b, nir_load_ssbo(&b, 1, 32, zero, nir_imm_int(&b, 4)), 0);
   nir_block *pre = nir_cursor_current_block(b.cursor);

   nir_loop *loop = nir_push_loop(&b);
   nir_block *header = nir_loop_first_block(loop);
   nir_phi_instr *phi = nir_phi_instr_create(b.shader);
   nir_def_init(&phi->instr, &phi->def, 1, 32);
   b.cursor = nir_after_block(header);

   nir_def *next = nir_fadd(&b, &phi->def, nir_imm_float(&b, 1.0f));
   nir_break_if(&b, cond);
   nir_block *latch = nir_loop_last_block(loop);
   nir_pop_loop(&b, loop);

   nir_phi_instr_add_src(phi, pre, init);
   nir_phi_instr_add_src(phi, latch, next);
   nir_instr_insert_before_block(header, &phi->instr);

   nir_def *after = nir_fsat(&b, next);
   nir_store_ssbo(&b, after, zero, nir_imm_int(&b, 8));
   nir_validate_shader(b.shader, "fp-inval-loop");

   nir_fp_analysis_state state = nir_create_fp_analysis_state(b.impl);
   nir_analyze_fp_class(&state, init);
   nir_analyze_fp_class(&state, after);
   require(BITSET_TEST(state.bitset, init->index), "init cached");
   require(BITSET_TEST(state.bitset, phi->def.index), "phi cached");
   require(BITSET_TEST(state.bitset, next->index), "next cached");
   require(BITSET_TEST(state.bitset, after->index), "after cached");

   nir_invalidate_fp_analysis_users(&state, init);
   require(!BITSET_TEST(state.bitset, init->index), "init cleared");
   require(!BITSET_TEST(state.bitset, phi->def.index), "phi cleared");
   require(!BITSET_TEST(state.bitset, next->index), "next cleared");
   require(!BITSET_TEST(state.bitset, after->index), "after cleared");

   nir_analyze_fp_class(&state, init);
   nir_analyze_fp_class(&state, after);
   nir_invalidate_fp_analysis_users(&state, next);
   require(BITSET_TEST(state.bitset, init->index), "init preserved when invalidating inside loop");
   require(!BITSET_TEST(state.bitset, phi->def.index), "phi cleared via back-edge");
   require(!BITSET_TEST(state.bitset, next->index), "next cleared");
   require(!BITSET_TEST(state.bitset, after->index), "after cleared");

   nir_free_fp_analysis_state(&state);
   ralloc_free(b.shader);
}

static void
test_grow_preserves_entries(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "fp-inval-grow");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *s = nir_fsat(&b, x);

   nir_fp_analysis_state state = nir_create_fp_analysis_state(b.impl);
   fp_class_mask s_cls = nir_analyze_fp_class(&state, s);
   uint32_t initial_size = state.size;

   nir_def *cur = x;
   for (uint32_t i = 0; i < initial_size + 64; i++)
      cur = nir_fadd(&b, cur, nir_imm_float(&b, 1.0f));

   require(b.impl->ssa_alloc > initial_size, "ssa_alloc exceeded initial capacity");
   nir_grow_fp_analysis_state(&state);
   require(state.size >= b.impl->ssa_alloc, "state grew past ssa_alloc");
   require(BITSET_TEST(state.bitset, s->index), "existing bit preserved across grow");
   require(state.arr[s->index] == s_cls, "existing fp_class preserved across grow");
   require(!BITSET_TEST(state.bitset, cur->index), "newly allocated index starts clear");

   fp_class_mask cur_cls = nir_analyze_fp_class(&state, cur);
   require(cur_cls == fresh_fp_class(b.impl, cur), "grown state analyzes new def");

   uint32_t second_size = state.size;
   nir_def *cur2 = cur;
   for (uint32_t i = 0; i < second_size + 64; i++)
      cur2 = nir_fadd(&b, cur2, nir_imm_float(&b, 1.0f));
   nir_store_ssbo(&b, cur2, zero, nir_imm_int(&b, 4));
   nir_store_ssbo(&b, s, zero, nir_imm_int(&b, 8));
   nir_validate_shader(b.shader, "fp-inval-grow");

   fp_class_mask cur2_cls = nir_analyze_fp_class(&state, cur2);
   require(state.size >= b.impl->ssa_alloc, "analyze auto-grows state");
   require(BITSET_TEST(state.bitset, s->index) && state.arr[s->index] == s_cls,
           "existing entry preserved across auto-grow");
   require(cur2_cls == fresh_fp_class(b.impl, cur2), "auto-grown state matches fresh analysis");

   nir_free_fp_analysis_state(&state);
   ralloc_free(b.shader);
}

static void
test_algebraic_bands_chain(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "fp-inval-bands");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *v = nir_load_ssbo(&b, 1, 32, zero, zero);
   nir_def *a = nir_imm_float(&b, 1.125f);
   nir_def *c = nir_imm_float(&b, 0.25f);
   nir_def *fzero = nir_imm_float(&b, 0.0f);
   nir_def *fone = nir_imm_float(&b, 1.0f);

   for (unsigned i = 0; i < 256; i++) {
      v = nir_ffract(&b, nir_ffma(&b, v, a, c));
      if ((i & 3u) == 0u)
         v = nir_fadd(&b, v, fzero);
      if ((i & 7u) == 0u)
         v = nir_fmul(&b, v, fone);
      if ((i & 15u) == 0u)
         v = nir_fabs(&b, nir_fsat(&b, v));
   }

   nir_def *sat_v = nir_fsat(&b, v);
   nir_def *p = nir_vec2(&b, sat_v, sat_v);
   nir_def *dot = nir_build_alu2(&b, nir_op_fdot2, p, nir_fneg(&b, p));
   nir_store_ssbo(&b, nir_fadd(&b, v, dot), zero, nir_imm_int(&b, 4));
   nir_validate_shader(b.shader, "fp-inval-bands-before");

   unsigned before = count_alu_total(b.shader);
   bool progress = false;
   while (nir_opt_algebraic(b.shader)) {
      progress = true;
      nir_opt_copy_prop(b.shader);
      nir_opt_dce(b.shader);
   }
   nir_validate_shader(b.shader, "fp-inval-bands-after");

   require(progress, "algebraic pass made progress");
   require(count_alu_total(b.shader) < before, "algebraic pass simplified chain");
   require(count_alu_op(b.shader, nir_op_fabs) == 0, "fabs(fsat(v)) folded via fp_class");

   nir_fp_analysis_state reused = nir_create_fp_analysis_state(b.impl);
   nir_foreach_block(block, b.impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         fp_class_mask got = nir_analyze_fp_class(&reused, &alu->def);
         fp_class_mask want = fresh_fp_class(b.impl, &alu->def);
         require(got == want, "cached fp_class matches fresh analysis across shader");
      }
   }
   nir_free_fp_analysis_state(&reused);
   ralloc_free(b.shader);
}

int
main(void)
{
   test_transitive_users_and_sibling_preserved();
   test_fdot_neg_lookthrough();
   test_loop_phi_cycle();
   test_grow_preserves_entries();
   test_algebraic_bands_chain();
   puts("5 NIR fp_class narrow invalidation tests passed");
   return 0;
}
