/* SPDX-License-Identifier: MIT */
#include "nak_private.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct nak_constant_offset_info nak_const_offsets_base = {0};
const struct nak_constant_offset_info nak_const_offsets_turing_graphics = {0};

uint16_t
nak_sysval_attr_addr(const struct nak_compiler *nak, gl_system_value value)
{
   (void)nak;
   if (value == SYSTEM_VALUE_FRAG_COORD)
      return NAK_ATTR_POSITION;
   if (value == SYSTEM_VALUE_POINT_COORD)
      return NAK_ATTR_POINT_SPRITE;
   abort();
}

uint16_t
nak_varying_attr_addr(const struct nak_compiler *nak, gl_varying_slot slot)
{
   (void)nak;
   if (slot == VARYING_SLOT_VAR0)
      return NAK_ATTR_GENERIC_START;
   abort();
}

static void
require(bool ok)
{
   if (!ok)
      abort();
}

static void
coordinates(bool raw, bool samples, bool point)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "depth-contract");
   b.shader->info.fs.uses_sample_shading = samples;
   nir_def *value = point ? nir_load_point_coord(&b) : nir_load_frag_coord(&b);
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = BITFIELD_MASK(value->num_components));
   struct nak_compiler compiler = {.sm = 53};
   struct nak_fs_key key = {.center_frag_depth = raw};
   nak_nir_lower_fs_inputs(b.shader, &compiler, &key);
   nir_opt_copy_prop(b.shader);
   nir_opt_dce(b.shader);
   unsigned seen = 0;
   const unsigned base = point ? NAK_ATTR_POINT_SPRITE : NAK_ATTR_POSITION;
   nir_foreach_block(block, b.impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_ipa_nv)
            continue;
         unsigned component = (nir_intrinsic_base(intrin) - base) / 4;
         require(component < (point ? 2u : 4u));
         require(!(seen & BITFIELD_BIT(component)));
         seen |= BITFIELD_BIT(component);
         uint32_t bits = nir_intrinsic_flags(intrin);
         struct nak_nir_ipa_flags flags;
         memcpy(&flags, &bits, sizeof(flags));
         const bool center = !samples || (raw && !point && component == 2);
         require(flags.interp_loc == (center ? NAK_INTERP_LOC_DEFAULT : NAK_INTERP_LOC_CENTROID));
         require(flags.interp_mode == NAK_INTERP_MODE_SCREEN_LINEAR);
      }
   }
   require(seen == (point ? 3u : 15u));
   ralloc_free(b.shader);
}

static void
varying(bool raw, bool force, bool explicit_sample)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "sample-correction");
   b.shader->info.fs.uses_sample_shading = true;
   nir_def *bary = explicit_sample
      ? nir_load_barycentric_at_sample(&b, 32, nir_load_sample_id(&b), .interp_mode = INTERP_MODE_NOPERSPECTIVE)
      : nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
   nir_def *value = nir_load_interpolated_input(&b, 1, 32, bary, nir_imm_int(&b, 0),
                                                .io_semantics = {.location = VARYING_SLOT_VAR0});
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), nir_imm_int(&b, 0), .write_mask = 1);
   struct nak_compiler compiler = {.sm = 53};
   struct nak_fs_key key = {.force_sample_shading = force, .center_frag_depth = raw};
   nak_nir_lower_fs_inputs(b.shader, &compiler, &key);
   nir_opt_dce(b.shader);
   unsigned seen = 0;
   nir_foreach_block(block, b.impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_ipa_nv)
            continue;
         require(nir_intrinsic_base(intrin) == NAK_ATTR_GENERIC_START);
         uint32_t bits = nir_intrinsic_flags(intrin);
         struct nak_nir_ipa_flags flags;
         memcpy(&flags, &bits, sizeof(flags));
         enum nak_interp_loc expected = explicit_sample ? NAK_INTERP_LOC_OFFSET :
                                        force && !raw ? NAK_INTERP_LOC_CENTROID : NAK_INTERP_LOC_DEFAULT;
         require(flags.interp_loc == expected);
         ++seen;
      }
   }
   require(seen == 1);
   ralloc_free(b.shader);
}

int
main(void)
{
   for (unsigned raw = 0; raw < 2; ++raw) {
      for (unsigned samples = 0; samples < 2; ++samples)
         for (unsigned point = 0; point < 2; ++point)
            coordinates(raw, samples, point);
      for (unsigned force = 0; force < 2; ++force)
         for (unsigned sample = 0; sample < 2; ++sample)
            varying(raw, force, sample);
   }
   puts("16 actual NIR interpolation cases passed");
   return 0;
}
