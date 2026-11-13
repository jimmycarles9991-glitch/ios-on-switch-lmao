#include "nvk_switch_ir_internal.h"

static struct switch_nvk_ir_value *
value(struct switch_nvk_ir_builder *s, uint32_t id)
{
   return id < s->program->values ? &s->values[id] : NULL;
}

static bool
numeric_type(struct switch_nvk_ir_builder *s, uint32_t id,
              unsigned *components, unsigned *bits)
{
   if (id >= s->program->types || !s->types[id].type ||
       !glsl_type_is_vector_or_scalar(s->types[id].type))
      return false;
   *components = glsl_get_vector_elements(s->types[id].type);
   *bits = glsl_get_bit_size(s->types[id].type);
   return *components && *components <= 4;
}

static unsigned
float_controls(struct switch_nvk_ir_builder *s, const struct glsl_type *type)
{
   return type && glsl_type_is_float_16_32_64(type) &&
      (s->preserve_float_widths & glsl_get_bit_size(type)) ? nir_fp_preserve_sz_inf_nan : 0;
}

bool
switch_nvk_ir_instruction(struct switch_nvk_ir_builder *s,
                          const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   if (r->opcode == SpvOpControlBarrier || r->opcode == SpvOpMemoryBarrier)
      return switch_nvk_ir_barrier(s, r);
   unsigned components = 0, bits = 0;
   struct switch_nvk_ir_value *out = value(s, r->result);
   if (out) {
      if (r->type >= s->program->types) return false;
      out->type = r->type;
      out->shape = s->types[r->type].type;
   }
   switch (r->opcode) {
   case SpvOpSampledImage: case SpvOpImage: case SpvOpImageFetch:
   case SpvOpImageSampleExplicitLod: case SpvOpImageSampleDrefExplicitLod:
   case SpvOpImageQuerySize: case SpvOpImageQuerySizeLod:
   case SpvOpImageQueryLevels: case SpvOpImageQuerySamples:
   case SpvOpImageRead: case SpvOpImageWrite:
      return switch_nvk_ir_image(s, r);
   default: break;
   }
   if (r->opcode == SpvOpAccessChain) {
      if (r->flags || r->auxiliary || !out || r->count < 1 || !value(s, a[0]) || !value(s, a[0])->deref)
         return false;
      nir_deref_instr *deref = value(s, a[0])->deref;
      for (unsigned i = 1; i < r->count; i++) {
         if (!value(s, a[i]) || !value(s, a[i])->def)
            return false;
         nir_def *index = value(s, a[i])->def;
         if (index->num_components != 1 || index->bit_size == 1)
            return false;
         if (glsl_type_is_struct(deref->type)) {
            if (nir_def_instr_type(index) != nir_instr_type_load_const ||
                nir_src_as_uint(nir_src_for_ssa(index)) >= glsl_get_length(deref->type))
               return false;
            deref = nir_build_deref_struct(&s->b, deref, nir_src_as_uint(nir_src_for_ssa(index)));
         } else if (glsl_type_is_array(deref->type) || glsl_type_is_vector(deref->type)) {
            deref = nir_build_deref_array(&s->b, deref, index);
         } else {
            return false;
         }
      }
      if (deref->type != out->shape)
         return false;
      out->deref = deref;
      return true;
   }
   if (r->opcode == SpvOpLoad || r->opcode == SpvOpStore) {
      bool load = r->opcode == SpvOpLoad;
      bool visible = r->flags & 8, available = r->flags & 16;
      if ((r->flags & ~31u) || (load ? available : visible) ||
          ((visible || available) && !(r->flags & 4)) ||
          r->count != (load ? 1 : 2) + (visible || available) ||
          !value(s, a[0]) || !value(s, a[0])->deref)
         return false;
      nir_deref_instr *deref = value(s, a[0])->deref;
      if ((r->flags & 4) && !nir_deref_mode_is_one_of(deref,
            nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_shared | nir_var_mem_global))
         return false;
      if (r->auxiliary) {
         if (!util_is_power_of_two_nonzero(r->auxiliary)) return false;
         if (nir_deref_mode_is_one_of(deref, nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_shared | nir_var_mem_global))
            deref = nir_alignment_deref_cast(&s->b, deref, r->auxiliary, 0);
      }
      enum gl_access_qualifier access = 0;
      if (r->flags & 1) access |= ACCESS_VOLATILE;
      if (r->flags & 2) access |= ACCESS_NON_TEMPORAL;
      if (load) {
         if (!out || out->shape != deref->type) return false;
         if (visible && !switch_nvk_ir_access_barrier(s, deref, a[1], true)) return false;
         return switch_nvk_ir_load(s, deref, access, out);
      } else {
         if (!value(s, a[1]) || !switch_nvk_ir_store(s, deref, access, value(s, a[1]))) return false;
         return !available || switch_nvk_ir_access_barrier(s, deref, a[2], false);
      }
   }
   if (r->opcode == SpvOpCompositeConstruct || r->opcode == SpvOpCompositeExtract ||
       r->opcode == SpvOpCompositeInsert)
      return switch_nvk_ir_composite(s, r);
   if (r->opcode == SpvOpUndef)
      return out && !r->count && !r->flags && !r->auxiliary &&
         switch_nvk_ir_undef(s, out, out->shape);
   if (r->opcode == SpvOpCopyObject) {
      if (!out || r->count != 1 || r->flags || r->auxiliary || !value(s, a[0]) ||
          !out->shape || out->shape != value(s, a[0])->shape)
         return false;
      *out = *value(s, a[0]);
      out->type = r->type;
      return true;
   }
   if (!out || !numeric_type(s, r->type, &components, &bits))
      return false;
   if (r->opcode == SpvOpArrayLength) {
      if (r->flags || r->count != 1 || !value(s, a[0]) || !value(s, a[0])->deref)
         return false;
      nir_deref_instr *base = value(s, a[0])->deref;
      if (!glsl_type_is_struct(base->type) || r->auxiliary >= glsl_get_length(base->type))
         return false;
      const struct glsl_type *member = glsl_get_struct_field(base->type, r->auxiliary);
      if (!glsl_type_is_array(member) || glsl_get_length(member) || components != 1 || bits != 32)
         return false;
      nir_deref_instr *array = nir_build_deref_struct(&s->b, base, r->auxiliary);
      out->def = nir_deref_buffer_array_length(&s->b, bits, &array->def);
      return true;
   }
   if (r->opcode == SpvOpVectorShuffle) {
      if (r->flags || r->auxiliary || r->count != components + 2 || !value(s, a[0]) || !value(s, a[0])->def ||
          !value(s, a[1]) || !value(s, a[1])->def)
         return false;
      nir_def *left = value(s, a[0])->def, *right = value(s, a[1])->def;
      if (left->bit_size != bits || right->bit_size != bits)
         return false;
      nir_def *lanes[4];
      for (unsigned i = 0; i < components; i++) {
         unsigned lane = a[2 + i];
         if (lane >= left->num_components + right->num_components) return false;
         lanes[i] = lane < left->num_components ? nir_channel(&s->b, left, lane) :
            nir_channel(&s->b, right, lane - left->num_components);
      }
      out->def = nir_vec(&s->b, lanes, components);
      return true;
   }
   if (r->count > 4 || (r->flags & ~1u) || (r->opcode != SpvOpExtInst && r->auxiliary))
      return false;
   nir_def *src[4];
   for (unsigned i = 0; i < r->count; i++) {
      if (!value(s, a[i]) || !(src[i] = value(s, a[i])->def))
         return false;
   }
   const unsigned saved = s->b.fp_math_ctrl;
   s->b.fp_math_ctrl = float_controls(s, out->shape);
   for (unsigned i = 0; i < r->count; i++)
      s->b.fp_math_ctrl |= float_controls(s, value(s, a[i])->shape);
   out->def = r->opcode == SpvOpExtInst ?
      switch_nvk_ir_glsl(&s->b, r->auxiliary, src, r->count, components, bits, r->flags & 1) :
      switch_nvk_ir_alu(&s->b, r->opcode, src, r->count, components, bits, r->flags & 1);
   s->b.fp_math_ctrl = saved;
   return out->def && out->def->num_components == components && out->def->bit_size == bits;
}
