/* SPDX-License-Identifier: MIT */
#include "nak_private.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>

const struct nak_constant_offset_info nak_const_offsets_base = {
   .interlock_ticket_buffer_cb = 0,
   .interlock_ticket_buffer_offset = 0x260,
};
const struct nak_constant_offset_info nak_const_offsets_turing_graphics = {0};

static void
require(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "%s\n", what);
      abort();
   }
}

struct section {
   unsigned ticket_reads, waits, releases;
   nir_def *wait_addr, *release_addr;
};

static void
count(nir_shader *nir, struct section *s)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_sysval_nv:
               s->ticket_reads += nir_intrinsic_base(intr) == NAK_SV_ORDERING_TICKET;
               break;
            case nir_intrinsic_load_global:
               s->waits++;
               s->wait_addr = intr->src[0].ssa;
               break;
            case nir_intrinsic_global_atomic:
               s->releases++;
               s->release_addr = intr->src[0].ssa;
               break;
            default:
               break;
            }
         }
      }
   }
}

/* A storage form's section as GM20B's ticket protocol lowers it: the warp
 * reads its ticket and waits once, and releases the word it waited on
 */
int
main(void)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "interlock");
   b.shader->info.fs.pixel_interlock_ordered = true;
   nir_begin_invocation_interlock(&b);
   nir_end_invocation_interlock(&b);

   const struct nak_compiler nak = {.sm = 53};
   require(nak_nir_lower_fs_interlock(b.shader, &nak), "the section was not lowered");
   nir_lower_vars_to_ssa(b.shader);
   nir_opt_copy_prop(b.shader);
   nir_opt_dce(b.shader);

   struct section s = {0};
   count(b.shader, &s);
   require(s.ticket_reads == 1, "the ticket read more than once");
   require(s.waits == 1, "the release waited for the ticket again");
   require(s.releases == 1, "not one release");
   require(s.wait_addr == s.release_addr, "the release found its ticket word again");
   ralloc_free(b.shader);
   puts("the interlock section reads its ticket once, waits once and releases the word it waited on");
   return 0;
}
