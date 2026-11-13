/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

/* SM50 has seven predicates. A comparison against a constant that CSE leaves
 * in one block for uses in many others (a switch on a uniform mode becomes
 * one compare per case, reused wherever the mode is tested) stays live in a
 * predicate across those blocks, and past seven NAK spills predicates to
 * GPRs. Each using block gets its own copy of the comparison instead: one
 * ISETP/FSETP with an immediate where a spill reload would also cost one,
 * without the predicate or a GPR held in between. Only in a function where
 * more booleans are live into some block than SM50 has predicates: elsewhere
 * the copies cost instructions and save nothing. Runs before
 * nak_nir_rematerialize_load_const, which then gives each copy's constant to
 * its block.
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "util/hash_table.h"

#define SM50_PREDICATES 7

struct remat_ctx {
   struct hash_table *remap;
   nir_block *block;
   nir_builder b;
};

static bool
is_cheap_compare(nir_def *def)
{
   if (def->bit_size != 1 || nir_def_instr_type(def) != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_def_as_alu(def);
   switch (alu->op) {
   case nir_op_ieq:
   case nir_op_ine:
   case nir_op_ilt:
   case nir_op_ige:
   case nir_op_ult:
   case nir_op_uge:
   case nir_op_flt:
   case nir_op_fge:
   case nir_op_feq:
   case nir_op_fneu:
      return nir_src_is_const(alu->src[0].src) ||
             nir_src_is_const(alu->src[1].src);
   default:
      return false;
   }
}

static nir_def *
block_copy(struct remat_ctx *ctx, nir_def *def)
{
   struct hash_entry *entry = _mesa_hash_table_search(ctx->remap, def);
   if (entry != NULL)
      return entry->data;

   nir_alu_instr *copy =
      nir_alu_instr_clone(ctx->b.shader, nir_def_as_alu(def));
   nir_builder_instr_insert(&ctx->b, &copy->instr);
   _mesa_hash_table_insert(ctx->remap, def, &copy->def);
   return &copy->def;
}

static bool
rematerialize_src(nir_src *src, void *_ctx)
{
   struct remat_ctx *ctx = _ctx;

   if (is_cheap_compare(src->ssa) && nir_def_block(src->ssa) != ctx->block)
      nir_src_rewrite(src, block_copy(ctx, src->ssa));

   return true;
}

static unsigned
most_booleans_live_in(nir_function_impl *impl)
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
      unsigned booleans = 0;
      U_SPARSE_BITSET_FOREACH_SET(&block->live_in, i) {
         if (defs[i] != NULL && defs[i]->bit_size == 1)
            booleans++;
      }
      most = MAX2(most, booleans);
   }

   free(defs);
   return most;
}

static bool
rematerialize_compares_impl(nir_function_impl *impl)
{
   if (most_booleans_live_in(impl) <= SM50_PREDICATES)
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
nak_nir_rematerialize_compares(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function_impl(impl, nir)
      progress |= rematerialize_compares_impl(impl);

   return progress;
}
