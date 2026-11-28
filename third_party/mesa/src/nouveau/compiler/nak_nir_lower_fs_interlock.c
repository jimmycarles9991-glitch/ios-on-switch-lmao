/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "nak_private.h"
#include "nir_builder.h"

/**
 * For interlock, we have up to 256 ordering tickets per GPC.
 * The hardware has a "ticket dispenser" that assigns tickets for each warp.
 * Tickets can be read from the special register ORDERING_TICKET, are uniform
 * across a warp and have the following layout:
 *
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Ticket id   |   Increment   |        Expected value         |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Where:
 * - Ticket id: identifies the ticket you must wait on (GPC-specific)
 * - Increment: How much the ticket value should be incremented once the
 *              warp is unlocked
 * - Expected value: The value of the ticket before processing can start
 *
 * At the start of the interlock section the warp elects a leader and waits
 * until the assigned address in the ticket buffer reaches the expected value.
 * When ending the interlock section, the interlock ticket value will be
 * incremented atomically, to allow the warps holding the next tickets to proceed.
 *
 * Ticket buffer
 * =============
 *
 * The ticket buffer is not simply incremented by 1 as you may expect,
 * but it's incremented by (2 + ticket_increment) << 16.  This happens
 * because the ticket buffer uses the following layout:
 *
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               |   Progress    |         Ticket value          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * The ticket dispenser assigns tickets having the same id and expected value
 * for a tile of non-conflicting fragment warps, those can run in parallel.
 * When each is done, it increments the "Progress" subfield.  The trick is
 * that a single warp ending the locking section does not increment
 * the ticket value, only when every warp is done the "progress" addition
 * overflows into the ticket value, allowing the next tile of warps
 * to start processing the locked section.  It's imperative that all warps
 * wait on the ticket AND increment the buffer afterwards.
 *
 * Early bail-out
 * ==============
 *
 * This scheme fails if the shader performs a discard the whole warp as
 * if no invocation is active during the leader election, no thread will
 * wait for the spin-lock or, more importantly, increment the ticket buffer.
 * To solve this problem we instrument every discard in the shader and
 * "bail-out" early if every thread is going to be discarded.  Bailing-out
 * just means waiting for the ticket and updating the ticket, as if we are
 * exiting the critical section.
 *
 * Two edge-cases to keep in mind: the discards can happen in divergent
 * code-paths, we must keep around a bit-mask of the non-discarded threads
 * to check if the current discard could kill the warp.
 *
 * We must not double-increment the ticket buffer by "bailing-out" AFTER
 * the ticket has been incremented, to do this we allocate a variable that
 * is set whenever the ticket is incremented and we check it before bailing out.
 * The variable is always optimized away because the `end_invocation_interlock`
 * intrinsic cannot be placed inside of flow control.
 */
struct interlock_data {
   nir_variable *alive_mask_var;
   nir_variable *ticket_updated_var;
   nir_variable *ticket_addr_var;
   nir_variable *ticket_increment_var;
   const struct nak_compiler *nak;
   bool load_spin;
};

/* The warp's ticket word in the ticket buffer, the increment its release adds
 * and the value its wait waits for. The ticket and the GPC are the warp's, so
 * every lane may compute them.
 */
static nir_def *
interlock_ticket_word(nir_builder *b, const struct interlock_data *tdata,
                      nir_def **increment, nir_def **value)
{
   nir_def *gpc_id = nir_load_sysval_nv(b, 32, .base = NAK_SV_AFFINITY,
                                        .access = ACCESS_CAN_REORDER);
   gpc_id = nir_ubitfield_extract_imm(b, gpc_id, 0, 8);

   nir_def *ordering_ticket =
      nir_load_sysval_nv(b, 32, .base = NAK_SV_ORDERING_TICKET);
   nir_def *ticket_id = nir_ubitfield_extract_imm(b, ordering_ticket, 0, 8);
   *increment = nir_iadd_imm(b, nir_iand_imm(b, ordering_ticket, 0xfe00), 0x200);
   *value = nir_ushr_imm(b, ordering_ticket, 16);

   const struct nak_constant_offset_info *offsets =
      nak_const_offsets(tdata->nak, true);
   nir_def *base = nir_ldc_nv(
      b, 1, 64, nir_imm_int(b, offsets->interlock_ticket_buffer_cb),
      nir_imm_int(b, offsets->interlock_ticket_buffer_offset),
      .align_mul = 8, .align_offset = 0);
   nir_def *offset = nir_iadd(b, nir_imul_imm(b, gpc_id, 0x100), ticket_id);
   offset = nir_imul_imm(b, offset, sizeof(uint32_t));
   return nir_iadd(b, base, nir_u2u64(b, offset));
}

/* SPIR-V ends an interlock behind a device-scope release barrier on image,
 * SSBO and global memory (spirv_to_nir), which NAK gives as MEMBAR.GL. On
 * GM20B the release's MEMBAR.VC orders those writes ahead of the ticket's
 * atomic add, as NVIDIA's compiler orders them, with no MEMBAR.GL.
 */
static void
remove_release_barrier_before(nir_instr *instr)
{
   nir_instr *prev = nir_instr_prev(instr);
   if (prev == NULL || prev->type != nir_instr_type_intrinsic)
      return;

   nir_intrinsic_instr *barrier = nir_instr_as_intrinsic(prev);
   if (barrier->intrinsic == nir_intrinsic_barrier &&
       nir_intrinsic_execution_scope(barrier) == SCOPE_NONE &&
       nir_intrinsic_memory_scope(barrier) == SCOPE_DEVICE &&
       nir_intrinsic_memory_semantics(barrier) == NIR_MEMORY_RELEASE)
      nir_instr_remove(prev);
}

static bool
nak_nir_lower_fs_interlock_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                                  UNUSED void *data)
{
   const struct interlock_data *tdata = data;
   struct nir_variable *alive_mask_var = tdata->alive_mask_var;
   struct nir_variable *ticket_updated_var = tdata->ticket_updated_var;

   bool is_demote;
   bool ticket_wait;
   bool ticket_update;

   switch (intrin->intrinsic) {
   case nir_intrinsic_demote_if:
   case nir_intrinsic_demote:
      is_demote = true;
      ticket_wait = true;
      ticket_update = true;

      b->cursor = nir_before_instr(&intrin->instr);
      break;
   case nir_intrinsic_begin_invocation_interlock:
      is_demote = false;
      ticket_wait = true;
      ticket_update = false;

      b->cursor = nir_instr_remove(&intrin->instr);
      break;
   case nir_intrinsic_end_invocation_interlock:
      /* The ticket's value moves on only once every warp of the tile has
       * released, this one included, so the value the begin waited for is
       * still there: the release need not wait again.
       */
      is_demote = false;
      ticket_wait = false;
      ticket_update = true;

      if (tdata->nak->sm >= 50 && tdata->nak->sm < 70)
         remove_release_barrier_before(&intrin->instr);
      b->cursor = nir_instr_remove(&intrin->instr);
      break;
   default:
      return false;
   }

   if (intrin->intrinsic == nir_intrinsic_demote_if) {
      nir_push_if(b, intrin->src[0].ssa);
   }

   /* First we need to know the amount of threads alive to only execute the
    * following block in the last thread */
   nir_def *current_invocation = nir_load_subgroup_invocation(b);
   nir_def *helper_invocation = nir_is_helper_invocation(b, 32);
   nir_def *curr_alive_mask =
      nir_ballot(b, 1, 32, nir_ieq_imm(b, helper_invocation, 0));
   nir_def *bail_early = NULL;

   if (is_demote) {
      /* If the ticket has already been updated, no need to bail */
      bail_early = nir_inot(b, nir_load_var(b, ticket_updated_var));

      nir_def *prev_alive_mask = nir_load_var(b, alive_mask_var);

      /* Alive mask after the discard */
      nir_def *new_alive_mask =
         nir_iand(b, prev_alive_mask, nir_inot(b, curr_alive_mask));
      nir_store_var(b, alive_mask_var, new_alive_mask, 0x1);

      /* Bail only if we discard every fragment */
      bail_early = nir_iand(b, bail_early, nir_ieq_imm(b, new_alive_mask, 0));
   }

   /* A demote may come before the begin, so it finds its ticket word itself */
   nir_def *ticket_addr = NULL, *ticket_increment = NULL, *ticket_value = NULL;
   if (intrin->intrinsic == nir_intrinsic_begin_invocation_interlock) {
      ticket_addr = interlock_ticket_word(b, tdata, &ticket_increment,
                                          &ticket_value);
      nir_store_var(b, tdata->ticket_addr_var, ticket_addr, 0x1);
      nir_store_var(b, tdata->ticket_increment_var, ticket_increment, 0x1);
   } else if (intrin->intrinsic == nir_intrinsic_end_invocation_interlock) {
      ticket_addr = nir_load_var(b, tdata->ticket_addr_var);
      ticket_increment = nir_load_var(b, tdata->ticket_increment_var);
   }

   /* Elect a (non-killed) leader */
   nir_def *active_thread_count = nir_ufind_msb(b, curr_alive_mask);
   nir_def *is_leader = nir_ieq(b, current_invocation, active_thread_count);

   /* For demotes, avoid double-incrementing */
   if (is_demote) {
      assert(bail_early != NULL);
      is_leader = nir_iand(b, bail_early, is_leader);
   }

   nir_push_if(b, is_leader);
   {
      if (is_demote) {
         ticket_addr = interlock_ticket_word(b, tdata, &ticket_increment,
                                             &ticket_value);
      }
      nir_def *interlock_ticket_buffer_addr = ticket_addr;

      if (ticket_wait) {
         /* The tile coalescer only hands out the tickets; the warp itself
          * waits until the ticket word reaches its expected value. Any read
          * of the word in L2, where the releases' atomic adds land, sees it:
          * before Volta, an atomic load (ACCESS_ATOMIC) is LDG.E.CG, which
          * bypasses the SM's L1 that another SM's release never invalidates.
          * The section's own accesses are strong system-scope loads and
          * stores (from_nir get_load_mem_order), ordered after this one by
          * the branch on its value. An atomic add of 0 would read the same
          * value but queue on L2's atomic unit beside every release.
          */
         nir_variable *backoff_var = NULL;
         if (tdata->load_spin) {
            backoff_var = nir_local_variable_create(b->impl, glsl_uint_type(),
                                                    "fsi_backoff");
            nir_store_var(b, backoff_var, nir_imm_int(b, 32), 0x1);
         }

         nir_loop *loop = nir_push_loop(b);
         {
            nir_def *current_gpc_ticket =
               tdata->load_spin
                  ? nir_load_global(b, 1, 32, interlock_ticket_buffer_addr,
                                    .align_mul = 4,
                                    .access = ACCESS_COHERENT |
                                              ACCESS_VOLATILE | ACCESS_ATOMIC)
                  : nir_global_atomic(b, 32, interlock_ticket_buffer_addr,
                                      nir_imm_int(b, 0),
                                      .atomic_op = nir_atomic_op_iadd);

            nir_break_if(b, nir_ieq(b, nir_ushr_imm(b, current_gpc_ticket, 16),
                                    ticket_value));

            if (tdata->load_spin) {
               nir_def *delay = nir_load_var(b, backoff_var);
               nir_def *start =
                  nir_load_sysval_nv(b, 32, .base = NAK_SV_CLOCK_LO);
               nir_ssa_bar_nv(b, start);

               nir_loop *spin = nir_push_loop(b);
               {
                  nir_def *now =
                     nir_load_sysval_nv(b, 32, .base = NAK_SV_CLOCK_LO);
                  nir_ssa_bar_nv(b, now);
                  nir_break_if(b, nir_uge(b, nir_isub(b, now, start), delay));
               }
               nir_pop_loop(b, spin);

               nir_def *next_delay =
                  nir_umin(b, nir_ishl_imm(b, delay, 1), nir_imm_int(b, 256));
               nir_store_var(b, backoff_var, next_delay, 0x1);
            }
         }
         nir_pop_loop(b, loop);
      }

      if (ticket_update) {
         /* GM20B: NVIDIA's compiler orders every release, a demote's
          * included, with MEMBAR.VC: the section's writes reach L2 before
          * the ticket's atomic add
          */
         if (tdata->nak->sm >= 50 && tdata->nak->sm < 70)
            nir_membar_vc_nv(b);
         else
            nir_barrier(b, .execution_scope = SCOPE_INVOCATION);
         nir_global_atomic(b, 32, interlock_ticket_buffer_addr,
                           ticket_increment,
                           .atomic_op = nir_atomic_op_iadd);

         nir_store_var(b, ticket_updated_var, nir_imm_bool(b, true), 0x1);
      }

      if (is_demote) {
         nir_jump(b, nir_jump_halt);
      }
   }
   nir_pop_if(b, NULL);

   if (intrin->intrinsic == nir_intrinsic_demote_if) {
      nir_pop_if(b, NULL);
   }

   return true;
}

bool
nak_nir_lower_fs_interlock(nir_shader *nir, const void *data)
{
   const struct nak_compiler *nak = data;

   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   const struct shader_info *info = &nir->info;
   const bool interlock_enabled =
      info->fs.pixel_interlock_ordered || info->fs.pixel_interlock_unordered ||
      info->fs.sample_interlock_ordered || info->fs.sample_interlock_unordered;

   /* We don't want to instrument discards if interlock is not used */
   if (!interlock_enabled)
      return false;

   nir_foreach_function_impl(impl, nir) {
      nir_variable *alive_mask_var =
         nir_local_variable_create(impl, glsl_int_type(), "alive_mask");
      nir_variable *ticket_updated_var = nir_local_variable_create(
         impl, glsl_bool_type(), "fsi_ticket_updated");
      nir_variable *ticket_addr_var = nir_local_variable_create(
         impl, glsl_uint64_t_type(), "fsi_ticket_addr");
      nir_variable *ticket_increment_var = nir_local_variable_create(
         impl, glsl_uint_type(), "fsi_ticket_increment");

      nir_builder b = nir_builder_at(nir_before_impl(impl));

      /* Initialize ticket_updated */
      nir_store_var(&b, ticket_updated_var, nir_imm_bool(&b, false), 0x1);

      /* Query the initial alive_mask */
      nir_def *helper_invocation = nir_is_helper_invocation(&b, 32);
      nir_def *alive_ballot =
         nir_ballot(&b, 1, 32, nir_ieq_imm(&b, helper_invocation, 0));
      nir_store_var(&b, alive_mask_var, alive_ballot, 0x1);

      const struct interlock_data pass_data = {
         .nak = nak,
         .load_spin = nak->sm >= 50 && nak->sm < 70,
         .alive_mask_var = alive_mask_var,
         .ticket_updated_var = ticket_updated_var,
         .ticket_addr_var = ticket_addr_var,
         .ticket_increment_var = ticket_increment_var,
      };

      nir_function_intrinsics_pass(impl, nak_nir_lower_fs_interlock_intrin,
                                   nir_metadata_none, (void *)&pass_data);
   }

   return true;
}
