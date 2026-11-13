/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 */

/* metal2vulkan's storage form begins the ordered pixel interlock at the top
 * of the entry point and ends it at the bottom, so the whole fragment body
 * runs inside the critical section and every overlapping fragment of a pixel
 * waits for all of it. Only the accesses to memory other invocations see (the
 * fetched attachments, any storage) need the order. This pass, run before
 * nak_nir_lower_fs_interlock turns begin and end into the ticket wait and
 * update, moves each top-level ordered load down to just ahead of its first
 * use, begins the section at the first instruction that is or depends on an
 * ordered access, and ends it after the last ordered access. The memory
 * model's acquire and release barriers move with the begin and the end.
 * Nothing else moves, so no value lives longer than it did.
 *
 * Begin and end stay at the top level of the function, outside any control
 * flow, as the lowering requires: an if or a loop that holds an ordered access
 * or depends on one keeps the section open over all of it. Nothing crosses a
 * terminate.
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "util/u_dynarray.h"

#define DEPENDS 1u /* reads what an ordered access read, or is one */
#define ORDERED 2u /* an access other invocations' order applies to */

/* A memory barrier the section's accesses carry at its begin (acquire) or
 * end (release), from the Vulkan memory model: it moves with them
 */
static nir_memory_semantics
fence_semantics(nir_instr *instr)
{
   if (instr->type != nir_instr_type_intrinsic ||
       instr->block->cf_node.parent->type != nir_cf_node_function)
      return 0;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_barrier ||
       nir_intrinsic_execution_scope(intr) != SCOPE_NONE)
      return 0;
   nir_memory_semantics sem = nir_intrinsic_memory_semantics(intr);
   return sem == NIR_MEMORY_ACQUIRE || sem == NIR_MEMORY_RELEASE ? sem : 0;
}

static bool
is_derivative(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy_coarse:
      return true;
   default:
      return false;
   }
}

static bool
is_terminate(nir_instr *instr)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
   return op == nir_intrinsic_terminate || op == nir_intrinsic_terminate_if;
}

static bool
is_ordered(nir_instr *instr)
{
   if (instr->type != nir_instr_type_intrinsic || fence_semantics(instr))
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (nir_intrinsic_infos[intr->intrinsic].flags & NIR_INTRINSIC_CAN_REORDER)
      return false;
   if (is_derivative(intr->intrinsic))
      return false;

   switch (intr->intrinsic) {
   case nir_intrinsic_begin_invocation_interlock:
   case nir_intrinsic_end_invocation_interlock:
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_is_helper_invocation:
   case nir_intrinsic_ballot:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_ieq:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_store_output:
      return false;
   default:
      return true;
   }
}

static bool
src_depends(nir_src *src, void *depends)
{
   if (nir_def_instr(src->ssa)->pass_flags & DEPENDS)
      *(bool *)depends = true;
   return true;
}

static bool
mark_depends(nir_instr *instr, bool control)
{
   if (instr->pass_flags & DEPENDS)
      return false;

   bool depends = control;
   if (is_ordered(instr)) {
      instr->pass_flags |= ORDERED;
      depends = true;
   }
   nir_foreach_src(instr, src_depends, &depends);
   if (depends)
      instr->pass_flags |= DEPENDS;
   return depends;
}

static bool
mark_cf_list(struct exec_list *list, bool control)
{
   bool progress = false;
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         nir_block *block = nir_cf_node_as_block(node);
         nir_foreach_instr(instr, block)
            progress |= mark_depends(instr, control);
         break;
      }
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         bool c = control ||
                  (nir_def_instr(nif->condition.ssa)->pass_flags & DEPENDS);
         progress |= mark_cf_list(&nif->then_list, c);
         progress |= mark_cf_list(&nif->else_list, c);
         if (c) {
            nir_block *after = nir_cf_node_as_block(nir_cf_node_next(node));
            nir_foreach_phi(phi, after)
               progress |= mark_depends(&phi->instr, true);
         }
         break;
      }
      case nir_cf_node_loop:
         progress |= mark_cf_list(&nir_cf_node_as_loop(node)->body, control);
         break;
      default:
         UNREACHABLE("a function's control flow");
      }
   }
   return progress;
}

static bool
node_holds(nir_cf_node *node, unsigned flag)
{
   /* An if on a dependent condition makes the phis after it dependent */
   if (flag == DEPENDS && node->type == nir_cf_node_if &&
       (nir_def_instr(nir_cf_node_as_if(node)->condition.ssa)->pass_flags &
        DEPENDS))
      return true;

   nir_foreach_block_in_cf_node(block, node) {
      nir_foreach_instr(instr, block) {
         if ((instr->pass_flags & flag) || is_terminate(instr))
            return true;
      }
   }
   return false;
}

static nir_intrinsic_instr *
find_top_level(nir_function_impl *impl, nir_intrinsic_op op)
{
   foreach_list_typed(nir_cf_node, node, node, &impl->body) {
      if (node->type != nir_cf_node_block)
         continue;
      nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic == op)
            return nir_instr_as_intrinsic(instr);
      }
   }
   return NULL;
}

/* Where the section begins: before the first top-level instruction after
 * `begin` that is or depends on an ordered access, or before the first
 * top-level if or loop holding one, or `end`.
 */
static nir_cursor
section_start(nir_instr *begin, nir_instr *end)
{
   for (nir_instr *instr = nir_instr_next(begin); instr != NULL;
        instr = nir_instr_next(instr)) {
      if (instr == end || (instr->pass_flags & DEPENDS) || is_terminate(instr))
         return nir_before_instr(instr);
   }

   nir_cf_node *node = nir_cf_node_next(&begin->block->cf_node);
   while (node != NULL) {
      if (node->type != nir_cf_node_block) {
         if (node_holds(node, DEPENDS))
            return nir_after_cf_node(nir_cf_node_prev(node));
      } else {
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            if (instr == end || (instr->pass_flags & DEPENDS) ||
                is_terminate(instr))
               return nir_before_instr(instr);
         }
      }
      node = nir_cf_node_next(node);
   }
   UNREACHABLE("end follows begin at the top level");
}

/* Where the section ends: after the last top-level ordered access before
 * `end`, or after the top-level if or loop holding the last one.
 */
static nir_cursor
section_end(nir_instr *begin, nir_instr *end)
{
   for (nir_instr *instr = nir_instr_prev(end); instr != NULL;
        instr = nir_instr_prev(instr)) {
      if (instr == begin || (instr->pass_flags & ORDERED) ||
          is_terminate(instr))
         return nir_after_instr(instr);
   }

   nir_cf_node *node = nir_cf_node_prev(&end->block->cf_node);
   while (node != NULL) {
      if (node->type != nir_cf_node_block) {
         if (node_holds(node, ORDERED))
            return nir_after_phis(nir_cf_node_as_block(nir_cf_node_next(node)));
      } else {
         nir_foreach_instr_reverse(instr, nir_cf_node_as_block(node)) {
            if (instr == begin || (instr->pass_flags & ORDERED) ||
                is_terminate(instr))
               return nir_after_instr(instr);
         }
      }
      node = nir_cf_node_prev(node);
   }
   UNREACHABLE("begin precedes end at the top level");
}

static bool
src_is_not(nir_src *src, void *def)
{
   return src->ssa != def;
}

static bool
instr_uses(nir_instr *instr, nir_def *def)
{
   return !nir_foreach_src(instr, src_is_not, def);
}

static bool
node_uses(nir_cf_node *node, nir_def *def)
{
   if (node->type == nir_cf_node_if &&
       nir_cf_node_as_if(node)->condition.ssa == def)
      return true;
   nir_foreach_block_in_cf_node(block, node) {
      nir_foreach_instr(instr, block) {
         if (instr_uses(instr, def))
            return true;
      }
   }
   nir_foreach_phi(phi, nir_cf_node_as_block(nir_cf_node_next(node))) {
      if (instr_uses(&phi->instr, def))
         return true;
   }
   return false;
}

static bool
stops_sinking(nir_instr *instr, nir_def *def, nir_instr *end)
{
   return instr == end || instr_uses(instr, def) ||
          (instr->pass_flags & ORDERED) || is_terminate(instr);
}

/* A top-level ordered load moves down to just ahead of its first use, or of
 * the next ordered access, so the section can begin there
 */
static bool
sink_load(nir_instr *load, nir_instr *end)
{
   nir_def *def = nir_instr_def(load);
   for (nir_instr *instr = nir_instr_next(load); instr != NULL;
        instr = nir_instr_next(instr)) {
      if (!stops_sinking(instr, def, end))
         continue;
      if (instr == nir_instr_next(load))
         return false;
      nir_instr_move(nir_before_instr(instr), load);
      return true;
   }

   for (nir_cf_node *node = nir_cf_node_next(&load->block->cf_node);
        node != NULL; node = nir_cf_node_next(node)) {
      if (node->type != nir_cf_node_block) {
         if (node_uses(node, def) || node_holds(node, ORDERED)) {
            nir_instr_move(nir_after_cf_node(nir_cf_node_prev(node)), load);
            return true;
         }
         continue;
      }
      nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
         if (instr->type == nir_instr_type_phi)
            continue;
         if (stops_sinking(instr, def, end)) {
            nir_instr_move(nir_before_instr(instr), load);
            return true;
         }
      }
   }
   UNREACHABLE("end follows the load at the top level");
}

static bool
sink_loads(nir_function_impl *impl, nir_instr *end)
{
   struct util_dynarray loads;
   util_dynarray_init(&loads, NULL);
   foreach_list_typed(nir_cf_node, node, node, &impl->body) {
      if (node->type != nir_cf_node_block)
         continue;
      nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
         if ((instr->pass_flags & ORDERED) && nir_instr_def(instr) != NULL)
            util_dynarray_append(&loads, instr);
      }
   }
   bool progress = false;
   util_dynarray_foreach_reverse(&loads, nir_instr *, load)
      progress |= sink_load(*load, end);
   util_dynarray_fini(&loads);
   return progress;
}

static bool
shrink_interlock_impl(nir_function_impl *impl)
{
   nir_intrinsic_instr *begin =
      find_top_level(impl, nir_intrinsic_begin_invocation_interlock);
   nir_intrinsic_instr *end =
      find_top_level(impl, nir_intrinsic_end_invocation_interlock);
   if (begin == NULL || end == NULL)
      return nir_no_progress(impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block)
         instr->pass_flags = 0;
   }
   while (mark_cf_list(&impl->body, false)) {
   }

   bool progress = sink_loads(impl, &end->instr);
   nir_cursor start = section_start(&begin->instr, &end->instr);
   if (!nir_cursors_equal(start, nir_after_instr(&begin->instr))) {
      nir_instr_move(start, &begin->instr);
      progress = true;
   }

   nir_cursor stop = section_end(&begin->instr, &end->instr);
   if (!nir_cursors_equal(stop, nir_before_instr(&end->instr))) {
      nir_instr_move(stop, &end->instr);
      progress = true;
   }

   if (!progress)
      return nir_no_progress(impl);

   struct util_dynarray fences;
   util_dynarray_init(&fences, NULL);
   foreach_list_typed(nir_cf_node, node, node, &impl->body) {
      if (node->type != nir_cf_node_block)
         continue;
      nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
         if (fence_semantics(instr))
            util_dynarray_append(&fences, instr);
      }
   }
   nir_instr *acquired = &begin->instr;
   util_dynarray_foreach(&fences, nir_instr *, fence) {
      if (fence_semantics(*fence) == NIR_MEMORY_ACQUIRE) {
         nir_instr_move(nir_after_instr(acquired), *fence);
         acquired = *fence;
      } else {
         nir_instr_move(nir_before_instr(&end->instr), *fence);
      }
   }
   util_dynarray_fini(&fences);

   return nir_progress(true, impl, nir_metadata_control_flow);
}

bool
nak_nir_shrink_interlock(nir_shader *nir)
{
   const struct shader_info *info = &nir->info;
   if (!info->fs.pixel_interlock_ordered &&
       !info->fs.pixel_interlock_unordered &&
       !info->fs.sample_interlock_ordered &&
       !info->fs.sample_interlock_unordered)
      return false;

   bool progress = false;
   nir_foreach_function_impl(impl, nir)
      progress |= shrink_interlock_impl(impl);

   return progress;
}
