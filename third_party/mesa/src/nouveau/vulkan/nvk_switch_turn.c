/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: see nvk_switch_turn.h.
 */

#include "nvk_switch_turn.h"

#include <string.h>

#include "nir_builder.h"
#include "nvk_cmd_buffer.h"
#include "util/u_dynarray.h"

_Thread_local bool switch_nvk_turned_current;

struct turn {
   nir_def *swap, *sx, *sy, *tx, *ty, *ox, *oy;
};

static nir_def *
root_word(nir_builder *b, uint32_t offset)
{
   return nir_ldc_nv(b, 1, 32, nir_imm_int(b, 0), nir_imm_int(b, 0),
                     .align_mul = 4, .align_offset = 0, .base = offset);
}

static struct turn
load_turn(nir_builder *b, uint32_t root_offset)
{
   return (struct turn){
      .swap = nir_ine_imm(b, root_word(b, root_offset), 0),
      .sx = root_word(b, root_offset + 4),
      .sy = root_word(b, root_offset + 8),
      .tx = root_word(b, root_offset + 12),
      .ty = root_word(b, root_offset + 16),
      .ox = root_word(b, root_offset + 20),
      .oy = root_word(b, root_offset + 24),
   };
}

/* The frame's (x, y) of the plane's: a signed swap and an offset */
static nir_def *
frame_xy(nir_builder *b, const struct turn *t, nir_def *x, nir_def *y,
         nir_def *offset_x, nir_def *offset_y)
{
   nir_def *fx = nir_fmul(b, t->sx, nir_bcsel(b, t->swap, y, x));
   nir_def *fy = nir_fmul(b, t->sy, nir_bcsel(b, t->swap, x, y));
   if (offset_x != NULL) {
      fx = nir_fadd(b, fx, offset_x);
      fy = nir_fadd(b, fy, offset_y);
   }
   return nir_vec2(b, fx, fy);
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
is_ddx(nir_intrinsic_op op)
{
   return op == nir_intrinsic_ddx || op == nir_intrinsic_ddx_fine ||
          op == nir_intrinsic_ddx_coarse;
}

static nir_intrinsic_op
other_axis(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_ddx: return nir_intrinsic_ddy;
   case nir_intrinsic_ddy: return nir_intrinsic_ddx;
   case nir_intrinsic_ddx_fine: return nir_intrinsic_ddy_fine;
   case nir_intrinsic_ddy_fine: return nir_intrinsic_ddx_fine;
   case nir_intrinsic_ddx_coarse: return nir_intrinsic_ddy_coarse;
   default: return nir_intrinsic_ddx_coarse;
   }
}

/* Whether every use of `d` is an fwidth's |d| + |other|, `other` the other
 * axis's derivative of the same value (appended to `partners`)
 */
static bool
fwidth_partners(nir_intrinsic_instr *d, struct util_dynarray *partners)
{
   nir_foreach_use_including_if(use, &d->def) {
      if (nir_src_is_if(use))
         return false;
      nir_instr *u = nir_src_use_instr(use);
      if (u->type != nir_instr_type_alu || nir_instr_as_alu(u)->op != nir_op_fabs)
         return false;
      nir_alu_instr *abs = nir_instr_as_alu(u);
      nir_foreach_use_including_if(sum_use, &abs->def) {
         if (nir_src_is_if(sum_use))
            return false;
         nir_instr *s = nir_src_use_instr(sum_use);
         if (s->type != nir_instr_type_alu || nir_instr_as_alu(s)->op != nir_op_fadd)
            return false;
         nir_alu_instr *add = nir_instr_as_alu(s);
         const unsigned self = &add->src[0].src == sum_use ? 0 : 1;
         nir_alu_instr *other_abs = nir_src_as_alu(add->src[1 - self].src);
         if (other_abs == NULL || other_abs->op != nir_op_fabs)
            return false;
         nir_intrinsic_instr *other = nir_src_as_intrinsic(other_abs->src[0].src);
         if (other == NULL || other->intrinsic != other_axis(d->intrinsic) ||
             !nir_srcs_equal(other->src[0], d->src[0]))
            return false;
         for (unsigned c = 0; c < add->def.num_components; c++) {
            if (abs->src[0].swizzle[add->src[self].swizzle[c]] !=
                other_abs->src[0].swizzle[add->src[1 - self].swizzle[c]])
               return false;
         }
         util_dynarray_append_typed(partners, nir_intrinsic_instr *, other);
      }
   }
   return true;
}

static bool
in_list(const struct util_dynarray *list, const void *x)
{
   util_dynarray_foreach(list, void *, at) {
      if (*at == x)
         return true;
   }
   return false;
}

/* The derivatives to turn: all but the fwidth pairs both of whose halves are
 * nothing but an fwidth's
 */
static void
derivatives_to_turn(struct util_dynarray *all, struct util_dynarray *out,
                    void *mem)
{
   struct util_dynarray kept, partners;
   util_dynarray_init(&kept, mem);
   util_dynarray_init(&partners, mem);
   util_dynarray_foreach(all, nir_intrinsic_instr *, d) {
      util_dynarray_clear(&partners);
      if (fwidth_partners(*d, &partners))
         util_dynarray_append_typed(&kept, nir_intrinsic_instr *, *d);
   }
   for (bool changed = true; changed;) {
      changed = false;
      util_dynarray_foreach(&kept, nir_intrinsic_instr *, d) {
         if (*d == NULL)
            continue;
         util_dynarray_clear(&partners);
         fwidth_partners(*d, &partners);
         util_dynarray_foreach(&partners, nir_intrinsic_instr *, p) {
            if (!in_list(&kept, *p)) {
               *d = NULL;
               changed = true;
               break;
            }
         }
      }
   }
   util_dynarray_foreach(all, nir_intrinsic_instr *, d) {
      if (!in_list(&kept, *d))
         util_dynarray_append_typed(out, nir_intrinsic_instr *, *d);
   }
}

static bool
is_image_access(const nir_intrinsic_instr *intrin)
{
   const char *name = nir_intrinsic_infos[intrin->intrinsic].name;
   return strncmp(name, "image_", 6) == 0 || strncmp(name, "bindless_image_", 15) == 0;
}

/* `use` reaches nothing but image accesses' coordinates (through moves,
 * vectors and float-to-integer conversions)
 */
static bool
addresses_only(nir_src *use, unsigned depth)
{
   if (nir_src_is_if(use) || depth > 8)
      return false;
   nir_instr *u = nir_src_use_instr(use);
   if (u->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(u);
      return is_image_access(intrin) && use == &intrin->src[1];
   }
   if (u->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(u);
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_f2i32:
   case nir_op_f2u32:
   case nir_op_f2i16:
   case nir_op_f2u16:
   case nir_op_ffloor:
   case nir_op_ftrunc:
   case nir_op_i2i32:
   case nir_op_u2u32:
      break;
   default:
      return false;
   }
   nir_foreach_use_including_if(next, &alu->def) {
      if (!addresses_only(next, depth + 1))
         return false;
   }
   return true;
}

static nir_def *
turned_derivative(nir_builder *b, const struct turn *t, nir_intrinsic_instr *d)
{
   nir_instr *x = nir_instr_clone(b->shader, &d->instr);
   nir_instr *y = nir_instr_clone(b->shader, &d->instr);
   nir_instr_as_intrinsic(x)->intrinsic = is_ddx(d->intrinsic) ? d->intrinsic : other_axis(d->intrinsic);
   nir_instr_as_intrinsic(y)->intrinsic = is_ddx(d->intrinsic) ? other_axis(d->intrinsic) : d->intrinsic;
   nir_builder_instr_insert(b, x);
   nir_builder_instr_insert(b, y);
   nir_def *dx = &nir_instr_as_intrinsic(x)->def, *dy = &nir_instr_as_intrinsic(y)->def;
   if (is_ddx(d->intrinsic))
      return nir_fmul(b, t->sx, nir_bcsel(b, t->swap, dy, dx));
   return nir_fmul(b, t->sy, nir_bcsel(b, t->swap, dx, dy));
}

static void
turn_position(nir_builder *b, const struct turn *t, nir_intrinsic_instr *load,
              void *mem)
{
   struct util_dynarray uses;
   util_dynarray_init(&uses, mem);
   nir_foreach_use_including_if(use, &load->def) {
      if (!addresses_only(use, 0))
         util_dynarray_append_typed(&uses, nir_src *, use);
   }
   if (util_dynarray_num_elements(&uses, nir_src *) == 0)
      return;
   b->cursor = nir_after_instr(&load->instr);
   nir_def *def = &load->def;
   nir_def *xy = frame_xy(b, t, nir_channel(b, def, 0), nir_channel(b, def, 1), t->tx, t->ty);
   nir_def *turned = def->num_components == 2 ? xy :
      nir_vector_insert_imm(b, nir_vector_insert_imm(b, def, nir_channel(b, xy, 0), 0),
                            nir_channel(b, xy, 1), 1);
   util_dynarray_foreach(&uses, nir_src *, use)
      nir_src_rewrite(*use, turned);
}

static void
turn_unit_xy(nir_builder *b, const struct turn *t, nir_intrinsic_instr *load)
{
   b->cursor = nir_after_instr(&load->instr);
   nir_def *def = &load->def;
   nir_def *xy = frame_xy(b, t, nir_channel(b, def, 0), nir_channel(b, def, 1), t->ox, t->oy);
   nir_def_rewrite_uses_after(def, xy);
}

/* An interpolation offset in the frame's pixels as the plane's: the inverse of
 * the signed swap
 */
static nir_def *
plane_offset(nir_builder *b, const struct turn *t, nir_def *o)
{
   nir_def *sx = nir_fmul(b, t->sx, nir_channel(b, o, 0));
   nir_def *sy = nir_fmul(b, t->sy, nir_channel(b, o, 1));
   return nir_vec2(b, nir_bcsel(b, t->swap, sy, sx), nir_bcsel(b, t->swap, sx, sy));
}

static void
turn_offset(nir_builder *b, const struct turn *t, nir_src *offset)
{
   b->cursor = nir_before_instr(nir_src_use_instr(offset));
   nir_src_rewrite(offset, plane_offset(b, t, offset->ssa));
}

/* An interpolation at a sample as one at that sample's offset in the plane: the
 * root table's sample locations are the frame's (the hardware's are turned),
 * read as nvk_nir_lower_descriptors' lower_interp_at_sample reads them
 */
static void
turn_at_sample(nir_builder *b, const struct turn *t, nir_intrinsic_instr *interp)
{
   b->cursor = nir_before_instr(&interp->instr);
   nir_def *loc = nir_ldc_nv(b, 1, 64, nir_imm_int(b, 0), nir_imm_int(b, 0), .align_mul = 8,
                             .align_offset = 0,
                             .base = nvk_root_descriptor_offset(draw.sample_locations));
   loc = nir_ushr(b, loc, nir_imul_imm(b, interp->src[1].ssa, 8));
   nir_def *x = nir_iand_imm(b, nir_u2u32(b, loc), 0xf);
   nir_def *y = nir_iand_imm(b, nir_ushr_imm(b, nir_u2u32(b, loc), 4), 0xf);
   nir_def *frame = nir_fadd_imm(b, nir_fmul_imm(b, nir_i2f32(b, nir_vec2(b, x, y)), 1.0 / 16.0), -0.5);
   interp->intrinsic = nir_intrinsic_interp_deref_at_offset;
   nir_src_rewrite(&interp->src[1], plane_offset(b, t, frame));
}

bool
switch_nvk_turn_fragment(nir_shader *nir, uint32_t root_offset)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   void *mem = ralloc_context(NULL);
   struct util_dynarray derivatives, turned_derivatives, positions, units, offsets, samples;
   util_dynarray_init(&samples, mem);
   util_dynarray_init(&derivatives, mem);
   util_dynarray_init(&turned_derivatives, mem);
   util_dynarray_init(&positions, mem);
   util_dynarray_init(&units, mem);
   util_dynarray_init(&offsets, mem);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_load_frag_coord:
         case nir_intrinsic_load_frag_coord_xy:
            util_dynarray_append_typed(&positions, nir_intrinsic_instr *, intrin);
            break;
         case nir_intrinsic_load_point_coord:
            util_dynarray_append_typed(&units, nir_intrinsic_instr *, intrin);
            break;
         case nir_intrinsic_interp_deref_at_sample:
            util_dynarray_append_typed(&samples, nir_intrinsic_instr *, intrin);
            break;
         case nir_intrinsic_interp_deref_at_offset:
         case nir_intrinsic_load_barycentric_at_offset:
            util_dynarray_append_typed(&offsets, nir_src *,
                                 &intrin->src[intrin->intrinsic == nir_intrinsic_load_barycentric_at_offset ? 0 : 1]);
            break;
         default:
            if (is_derivative(intrin->intrinsic))
               util_dynarray_append_typed(&derivatives, nir_intrinsic_instr *, intrin);
            break;
         }
      }
   }
   derivatives_to_turn(&derivatives, &turned_derivatives, mem);

   nir_builder b = nir_builder_at(nir_before_impl(impl));
   const struct turn t = load_turn(&b, root_offset);
   util_dynarray_foreach(&positions, nir_intrinsic_instr *, load)
      turn_position(&b, &t, *load, mem);
   util_dynarray_foreach(&units, nir_intrinsic_instr *, load)
      turn_unit_xy(&b, &t, *load);
   util_dynarray_foreach(&offsets, nir_src *, offset)
      turn_offset(&b, &t, *offset);
   util_dynarray_foreach(&samples, nir_intrinsic_instr *, interp)
      turn_at_sample(&b, &t, *interp);
   util_dynarray_foreach(&turned_derivatives, nir_intrinsic_instr *, d) {
      b.cursor = nir_after_instr(&(*d)->instr);
      nir_def *turned = turned_derivative(&b, &t, *d);
      nir_def_rewrite_uses(&(*d)->def, turned);
      nir_instr_remove(&(*d)->instr);
   }
   ralloc_free(mem);
   /* the turn's loads stay where nothing reads them: dead-code elimination
    * drops them, so an untouched program compiles as before
    */
   return nir_progress(true, impl, nir_metadata_control_flow);
}
