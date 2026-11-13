/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

/* SM50 has no uniform registers: a value computed from constant-buffer
 * words, such as a mode field shifted out of a uniform or a half parameter
 * widened to f32, takes a full GPR in every thread for as long as it is live.
 * Core Animation's generic merge state reads its parameters once and tests
 * them across thousands of blocks, so 41 such values were live through most
 * of the program and set its register count. A short chain from constant
 * buffer words (ldc_nv with sources of the same kind, constants and ALU
 * operations on them) is cheap to compute again: each block that uses one
 * gets its own copy, and the value dies in that block. Copies stay out of a
 * loop the original is not in. A copy is an instruction in every block that
 * uses the chain, paid for only by the warps the freed registers add, so this
 * runs only where more than PRESSURE_GPRS values are live into some block (20
 * warps per SM or fewer).
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "util/hash_table.h"

#define PRESSURE_GPRS 96
#define CHAIN_INSTRS 6

struct remat_ctx {
   struct hash_table *remap;
   nir_block *block;
   nir_builder b;
};

static nir_cf_node *
innermost_loop(nir_cf_node *node)
{
   for (; node != NULL; node = node->parent) {
      if (node->type == nir_cf_node_loop)
         return node;
   }
   return NULL;
}

static bool
is_uniform_leaf(nir_instr *instr)
{
   if (instr->type == nir_instr_type_load_const)
      return true;
   return instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_ldc_nv;
}

/* The instructions computing `def` again from constant-buffer words would
 * take, or more than `budget` when it is not such a chain.
 */
static unsigned
chain_cost(nir_def *def, unsigned budget)
{
   nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_load_const)
      return 0;
   if (budget == 0)
      return 1;

   unsigned cost = 1;
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         cost += chain_cost(alu->src[i].src.ssa, budget - 1);
         if (cost > budget)
            return cost;
      }
      return cost;
   }
   if (is_uniform_leaf(instr)) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      for (unsigned i = 0; i < 2; i++)
         cost += chain_cost(intr->src[i].ssa, budget - 1);
      return cost;
   }
   return budget + 1;
}

static bool
rematerializable(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu && !is_uniform_leaf(instr))
      return false;
   if (instr->type == nir_instr_type_load_const)
      return false;
   return chain_cost(def, CHAIN_INSTRS) <= CHAIN_INSTRS;
}

static nir_def *
block_copy(struct remat_ctx *ctx, nir_def *def)
{
   if (nir_def_instr(def)->type == nir_instr_type_load_const)
      return def;

   struct hash_entry *entry = _mesa_hash_table_search(ctx->remap, def);
   if (entry != NULL)
      return entry->data;

   nir_instr *copy = nir_instr_clone(ctx->b.shader, nir_def_instr(def));
   if (copy->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(copy);
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         nir_def *src = alu->src[i].src.ssa;
         alu->src[i].src = nir_src_for_ssa(block_copy(ctx, src));
      }
   } else {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(copy);
      for (unsigned i = 0; i < 2; i++)
         intr->src[i] = nir_src_for_ssa(block_copy(ctx, intr->src[i].ssa));
   }
   nir_builder_instr_insert(&ctx->b, copy);
   nir_def *copied = nir_instr_def(copy);
   _mesa_hash_table_insert(ctx->remap, def, copied);
   return copied;
}

static bool
rematerialize_src(nir_src *src, void *_ctx)
{
   struct remat_ctx *ctx = _ctx;
   nir_block *def_block = nir_def_block(src->ssa);

   if (def_block != ctx->block &&
       innermost_loop(&def_block->cf_node) ==
          innermost_loop(&ctx->block->cf_node) &&
       rematerializable(src->ssa))
      nir_src_rewrite(src, block_copy(ctx, src->ssa));

   return true;
}

static unsigned
most_gprs_live_in(nir_function_impl *impl)
{
   nir_metadata_require(impl, nir_metadata_live_defs);

   nir_def **defs = calloc(impl->ssa_alloc, sizeof(*defs));
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def != NULL)
            defs[def->index] = def;
      }
   }

   unsigned most = 0;
   nir_foreach_block(block, impl) {
      unsigned gprs = 0;
      U_SPARSE_BITSET_FOREACH_SET(&block->live_in, i) {
         nir_def *def = defs[i];
         if (def != NULL && def->bit_size > 1)
            gprs += def->num_components * DIV_ROUND_UP(def->bit_size, 32);
      }
      most = MAX2(most, gprs);
   }

   free(defs);
   return most;
}

static bool
rematerialize_uniforms_impl(nir_function_impl *impl)
{
   if (most_gprs_live_in(impl) <= PRESSURE_GPRS)
      return nir_no_progress(impl);

   struct remat_ctx ctx = {
      .remap = _mesa_pointer_hash_table_create(NULL),
      .b = nir_builder_create(impl),
   };

   bool progress = false;
   nir_foreach_block(block, impl) {
      _mesa_hash_table_clear(ctx.remap, NULL);
      ctx.block = block;

      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_phi)
            continue;

         ctx.b.cursor = nir_before_instr(instr);
         nir_foreach_src(instr, rematerialize_src, &ctx);
      }

      ctx.b.cursor = nir_after_block_before_jump(block);

      nir_if *nif = nir_block_get_following_if(block);
      if (nif != NULL)
         rematerialize_src(&nif->condition, &ctx);

      for (unsigned i = 0; i < ARRAY_SIZE(block->successors); i++) {
         nir_block *succ = block->successors[i];
         if (succ == NULL)
            continue;

         nir_foreach_phi(phi, succ) {
            nir_foreach_phi_src(src, phi) {
               if (src->pred == block)
                  rematerialize_src(&src->src, &ctx);
            }
         }
      }

      if (ctx.remap->entries > 0)
         progress = true;
   }

   _mesa_hash_table_destroy(ctx.remap, NULL);

   return nir_progress(progress, impl, nir_metadata_control_flow);
}

bool
nak_nir_rematerialize_uniforms(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function_impl(impl, nir)
      progress |= rematerialize_uniforms_impl(impl);

   return progress;
}
