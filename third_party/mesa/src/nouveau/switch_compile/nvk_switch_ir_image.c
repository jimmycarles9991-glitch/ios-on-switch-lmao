// SPDX-License-Identifier: MIT
#include "nvk_switch_ir_internal.h"

static struct switch_nvk_ir_value *
value(struct switch_nvk_ir_builder *s, uint32_t id)
{
   return id < s->program->values ? &s->values[id] : NULL;
}

static bool
numeric(nir_def *v, unsigned components)
{
   return v && v->bit_size == 32 && v->num_components == components;
}

static bool
argument_type(struct switch_nvk_ir_builder *s, uint32_t id, bool floating)
{
   struct switch_nvk_ir_value *v = value(s, id);
   return v && v->shape && (floating ? glsl_type_is_float(v->shape) : glsl_type_is_integer(v->shape));
}

static nir_deref_instr *
image_deref(struct switch_nvk_ir_builder *s, struct switch_nvk_ir_value *v)
{
   if (!v || !v->deref || !v->shape || v->type >= s->program->types ||
       (s->types[v->type].resource != SpvOpTypeImage &&
        s->types[v->type].resource != SpvOpTypeSampledImage)) return NULL;
   const struct glsl_type *type = v->shape;
   if (glsl_type_is_sampler(type) && !glsl_type_is_bare_sampler(type))
      type = glsl_sampler_type_to_texture(type);
   if (!glsl_type_is_image(type) && !glsl_type_is_texture(type)) return NULL;
   return nir_build_deref_cast(&s->b, &v->deref->def,
      glsl_type_is_image(type) ? nir_var_image : nir_var_uniform, type, 0);
}

static bool
query(uint32_t opcode)
{
   return opcode == SpvOpImageQuerySize || opcode == SpvOpImageQuerySizeLod ||
      opcode == SpvOpImageQueryLevels || opcode == SpvOpImageQuerySamples;
}

static bool
storage(struct switch_nvk_ir_builder *s, const struct switch_nvk_ir_record *r,
        nir_deref_instr *image, nir_def **args)
{
   const bool write = r->opcode == SpvOpImageWrite;
   const bool size = r->opcode == SpvOpImageQuerySize || r->opcode == SpvOpImageQuerySizeLod;
   const bool read = r->opcode == SpvOpImageRead;
   const bool q = query(r->opcode);
   if ((!read && !write && !q) || (r->flags & ~7u) ||
       ((r->flags & 4) && !(r->flags & 2)) || (!(r->flags & 4) && r->auxiliary) ||
       (q && (r->flags || r->auxiliary))) return false;
   nir_def *coord = NULL, *lod = nir_imm_int(&s->b, 0), *sample = nir_undef(&s->b, 1, 32);
   enum glsl_sampler_dim dim = glsl_get_sampler_dim(image->type);
   unsigned coords = glsl_get_sampler_dim_coordinate_components(dim) + glsl_sampler_type_is_array(image->type);
   const bool ms = dim == GLSL_SAMPLER_DIM_MS;
   if (q) {
      if (r->count != (r->opcode == SpvOpImageQuerySizeLod ? 2 : 1)) return false;
      if (r->count == 2) {
         if (!numeric(args[1], 1) || ms) return false;
         lod = args[1];
      }
      if (r->opcode == SpvOpImageQuerySamples && !ms) return false;
      if (r->opcode == SpvOpImageQueryLevels && (ms || dim == GLSL_SAMPLER_DIM_BUF)) return false;
   } else {
      unsigned base = write ? 3 : 2;
      if (r->count != base + 8 || !args[1] || args[1]->bit_size != 32 || args[1]->num_components < coords)
         return false;
      nir_def **ops = args + base;
      if (ops[0] || ops[2] || ops[3] || ops[4] || ops[5] || ops[7] ||
          (ops[1] && (!numeric(ops[1], 1) || ms)) ||
          (ms ? !numeric(ops[6], 1) : ops[6] != NULL)) return false;
      coord = nir_pad_vec4(&s->b, nir_trim_vector(&s->b, args[1], coords));
      if (ops[1]) lod = ops[1];
      if (ops[6]) sample = ops[6];
      if (write && (!args[2] || args[2]->bit_size != 32 || args[2]->num_components != 4)) return false;
   }
   struct switch_nvk_ir_value *out = value(s, r->result);
   if (!write && (!out || !out->shape || !glsl_type_is_vector_or_scalar(out->shape) ||
                 glsl_get_bit_size(out->shape) != 32)) return false;
   if (write && r->result != UINT32_MAX) return false;
   if (read && glsl_get_vector_elements(out->shape) != 4) return false;
   const struct glsl_type *data_type = write ? value(s, switch_nvk_ir_args(s, r)[2])->shape : out->shape;
   if (!q && glsl_get_base_type(data_type) != glsl_get_sampler_result_type(image->type)) return false;
   if (q && !glsl_type_is_integer(out->shape)) return false;
   unsigned result_components = size ? glsl_get_sampler_dim_coordinate_components(dim) +
      glsl_sampler_type_is_array(image->type) - (dim == GLSL_SAMPLER_DIM_CUBE) : q ? 1 : 4;
   if (!write && glsl_get_vector_elements(out->shape) != result_components) return false;
   if ((r->flags & 4) && read && !switch_nvk_ir_access_barrier(s, image, r->auxiliary, true)) return false;
   nir_intrinsic_op op = write ? nir_intrinsic_image_deref_store : read ? nir_intrinsic_image_deref_load :
      size ? nir_intrinsic_image_deref_size : r->opcode == SpvOpImageQueryLevels ?
      nir_intrinsic_image_deref_levels : nir_intrinsic_image_deref_samples;
   nir_intrinsic_instr *instr = nir_intrinsic_instr_create(s->b.shader, op);
   instr->src[0] = nir_src_for_ssa(&image->def);
   nir_intrinsic_set_image_dim(instr, dim);
   nir_intrinsic_set_image_array(instr, glsl_sampler_type_is_array(image->type));
   const struct switch_nvk_ir_value *source = value(s, switch_nvk_ir_args(s, r)[0]);
   nir_intrinsic_set_format(instr, s->types[source->type].image_format);
   nir_intrinsic_set_access(instr, r->flags & 1 ? ACCESS_VOLATILE : 0);
   if (size) instr->src[1] = nir_src_for_ssa(lod);
   if (!q) {
      instr->src[1] = nir_src_for_ssa(coord);
      instr->src[2] = nir_src_for_ssa(sample);
      instr->src[write ? 4 : 3] = nir_src_for_ssa(lod);
      if (write) {
         instr->src[3] = nir_src_for_ssa(args[2]);
         instr->num_components = 4;
         nir_intrinsic_set_src_type(instr, nir_get_nir_type_for_glsl_type(data_type));
      } else nir_intrinsic_set_dest_type(instr, nir_get_nir_type_for_glsl_type(data_type));
   }
   if (!write) {
      if (!nir_intrinsic_infos[op].dest_components) instr->num_components = result_components;
      nir_def_init(&instr->instr, &instr->def, nir_intrinsic_dest_components(instr), 32);
   }
   nir_builder_instr_insert(&s->b, &instr->instr);
   if (!write) out->def = nir_trim_vector(&s->b, &instr->def, result_components);
   return !(r->flags & 4) || !write || switch_nvk_ir_access_barrier(s, image, r->auxiliary, false);
}

static bool
texture(struct switch_nvk_ir_builder *s, const struct switch_nvk_ir_record *r,
        struct switch_nvk_ir_value *source, nir_deref_instr *image, nir_def **args)
{
   const bool q = query(r->opcode), fetch = r->opcode == SpvOpImageFetch;
   const bool shadow = r->opcode == SpvOpImageSampleDrefExplicitLod;
   const bool sampling = r->opcode == SpvOpImageSampleExplicitLod || shadow;
   struct switch_nvk_ir_value *out = value(s, r->result);
   if ((!q && !fetch && !sampling) || r->flags || r->auxiliary || !out ||
       !out->shape || !glsl_type_is_vector_or_scalar(out->shape) || glsl_get_bit_size(out->shape) != 32)
      return false;
   enum glsl_sampler_dim dim = glsl_get_sampler_dim(image->type);
   const bool array = glsl_sampler_type_is_array(image->type), ms = dim == GLSL_SAMPLER_DIM_MS;
   unsigned coordinate_components = glsl_get_sampler_dim_coordinate_components(dim) + array;
   nir_tex_src srcs[10];
   unsigned count = 0;
#define SRC(T, V) do { srcs[count++] = (nir_tex_src) { .src_type = nir_tex_src_##T, .src = nir_src_for_ssa(V) }; } while (0)
   SRC(texture_deref, &image->def);
   nir_texop op;
   if (q) {
      if (r->count != (r->opcode == SpvOpImageQuerySizeLod ? 2 : 1) || !glsl_type_is_integer(out->shape)) return false;
      op = r->opcode == SpvOpImageQueryLevels ? nir_texop_query_levels :
         r->opcode == SpvOpImageQuerySamples ? nir_texop_texture_samples : nir_texop_txs;
      if (op == nir_texop_texture_samples && !ms) return false;
      if (op == nir_texop_query_levels && (ms || dim == GLSL_SAMPLER_DIM_BUF)) return false;
      if (op == nir_texop_txs) {
         if (r->count == 2 && (!numeric(args[1], 1) || ms || dim == GLSL_SAMPLER_DIM_BUF)) return false;
         if (!ms && dim != GLSL_SAMPLER_DIM_BUF) SRC(lod, r->count == 2 ? args[1] : nir_imm_int(&s->b, 0));
      }
      coordinate_components = 0;
   } else {
      unsigned base = sampling ? 3 : 2;
      if (r->count != base + 8 || !args[1] || args[1]->bit_size != 32 ||
          args[1]->num_components < coordinate_components) return false;
      nir_def **ops = args + base;
      if (ops[0] || (ops[4] && ops[5]) || (ops[2] != NULL) != (ops[3] != NULL)) return false;
      if (sampling) {
         if (!source->sampler || ms || dim == GLSL_SAMPLER_DIM_BUF ||
             (ops[1] != NULL) == (ops[2] != NULL) || (ops[7] && !ops[2]) || ops[6] ||
             (shadow ? !numeric(args[2], 1) : args[2] != NULL)) return false;
         nir_deref_instr *sampler = nir_build_deref_cast(&s->b, &source->sampler->def,
            nir_var_uniform, glsl_bare_sampler_type(), 0);
         SRC(sampler_deref, &sampler->def);
         op = ops[2] ? nir_texop_txd : nir_texop_txl;
         if (shadow) SRC(comparator, args[2]);
      } else {
         if (dim == GLSL_SAMPLER_DIM_CUBE || ops[2] || ops[3] || ops[5] || ops[7] ||
             (ms ? !numeric(ops[6], 1) || ops[1] : ops[6] != NULL) ||
             (!ms && dim != GLSL_SAMPLER_DIM_BUF && !ops[1])) return false;
         op = ms ? nir_texop_txf_ms : nir_texop_txf;
      }
      if (ops[1]) { if (!numeric(ops[1], 1)) return false; SRC(lod, ops[1]); }
      if (ops[6]) SRC(ms_index, ops[6]);
      if (ops[7]) { if (!numeric(ops[7], 1)) return false; SRC(min_lod, ops[7]); }
      if (ops[2]) {
         unsigned gradients = coordinate_components - array;
         if (!numeric(ops[2], gradients) || !numeric(ops[3], gradients)) return false;
         SRC(ddx, ops[2]); SRC(ddy, ops[3]);
      }
      if (ops[4] || ops[5]) {
         nir_def *offset = ops[4] ? ops[4] : ops[5];
         if (!numeric(offset, coordinate_components - array) || dim == GLSL_SAMPLER_DIM_CUBE) return false;
         SRC(offset, offset);
      }
      SRC(coord, nir_trim_vector(&s->b, args[1], coordinate_components));
   }
   if (!q && glsl_get_base_type(out->shape) != glsl_get_sampler_result_type(image->type)) return false;
   if (shadow && (!glsl_type_is_float(out->shape) || glsl_get_vector_elements(out->shape) != 1)) return false;
   nir_tex_instr *instr = nir_tex_instr_create(s->b.shader, count);
   instr->op = op;
   instr->sampler_dim = dim;
   instr->is_array = array;
   instr->is_shadow = shadow;
   instr->is_new_style_shadow = shadow;
   instr->coord_components = coordinate_components;
   instr->dest_type = q ? nir_type_int32 : nir_get_nir_type_for_glsl_type(out->shape);
   memcpy(instr->src, srcs, count * sizeof(*srcs));
   if (nir_tex_instr_dest_size(instr) != glsl_get_vector_elements(out->shape)) return false;
   nir_def_init(&instr->instr, &instr->def, nir_tex_instr_dest_size(instr), 32);
   nir_builder_instr_insert(&s->b, &instr->instr);
   out->def = &instr->def;
   return true;
#undef SRC
}

bool
switch_nvk_ir_image(struct switch_nvk_ir_builder *s, const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   if (!r->count || r->count > 11) return false;
   struct switch_nvk_ir_value *source = value(s, a[0]), *out = value(s, r->result);
   nir_deref_instr *image = image_deref(s, source);
   if (!image) return false;
   if (r->opcode == SpvOpSampledImage) {
      struct switch_nvk_ir_value *sampler = r->count == 2 ? value(s, a[1]) : NULL;
      if (r->flags || r->auxiliary || !out || !sampler || !sampler->shape || !sampler->deref ||
          !glsl_type_is_bare_sampler(sampler->shape) || !glsl_type_is_texture(image->type) ||
          out->shape != glsl_texture_type_to_sampler(image->type, false)) return false;
      out->deref = image;
      out->sampler = sampler->deref;
      return true;
   }
   if (r->opcode == SpvOpImage) {
      if (r->count != 1 || r->flags || r->auxiliary || !out || !source->sampler || out->shape != image->type) return false;
      out->deref = image;
      return true;
   }
   nir_def *args[11] = {0};
   for (unsigned i = 1; i < r->count; i++) {
      if (a[i] == UINT32_MAX) continue;
      struct switch_nvk_ir_value *v = value(s, a[i]);
      if (!v || !v->def) return false;
      args[i] = v->def;
   }
   const bool sampling = r->opcode == SpvOpImageSampleExplicitLod ||
                         r->opcode == SpvOpImageSampleDrefExplicitLod;
   if (query(r->opcode)) {
      if (r->count == 2 && !argument_type(s, a[1], false)) return false;
   } else {
      unsigned base = sampling || r->opcode == SpvOpImageWrite ? 3 : 2;
      if (r->count != base + 8 || !argument_type(s, a[1], sampling)) return false;
      if (sampling && a[2] != UINT32_MAX && !argument_type(s, a[2], true)) return false;
      for (unsigned i = 0; i < 8; i++) {
         if (a[base + i] == UINT32_MAX) continue;
         bool floating = sampling && (i < 4 || i == 7);
         if (!argument_type(s, a[base + i], floating)) return false;
      }
   }
   return glsl_type_is_image(image->type) ? storage(s, r, image, args) : texture(s, r, source, image, args);
}
