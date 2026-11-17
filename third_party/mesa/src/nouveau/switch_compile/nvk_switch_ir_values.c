#include "nvk_switch_ir_internal.h"

static bool
children(struct switch_nvk_ir_builder *s, struct switch_nvk_ir_value *out)
{
   if (!out->shape || (!glsl_type_is_array(out->shape) && !glsl_type_is_struct(out->shape)))
      return false;
   uint32_t count = glsl_get_length(out->shape);
   if (!count || count > (1u << 22) - s->aggregate_elements)
      return false;
   s->aggregate_elements += count;
   out->elements = rzalloc_array(s->b.shader, struct switch_nvk_ir_value, count);
   if (!out->elements)
      return false;
   out->element_count = count;
   return true;
}

static const struct glsl_type *
child_type(const struct glsl_type *shape, unsigned index)
{
   return glsl_type_is_struct(shape) ? glsl_get_struct_field(shape, index) :
                                       glsl_get_array_element(shape);
}

static nir_deref_instr *
child_deref(nir_builder *b, nir_deref_instr *deref, unsigned index)
{
   return glsl_type_is_struct(deref->type) ? nir_build_deref_struct(b, deref, index) :
                                           nir_build_deref_array_imm(b, deref, index);
}

static bool
load(struct switch_nvk_ir_builder *s, nir_deref_instr *deref,
     enum gl_access_qualifier access, struct switch_nvk_ir_value *out, unsigned depth)
{
   if (depth > 64)
      return false;
   out->shape = deref->type;
   if (glsl_type_is_image(out->shape) || glsl_type_is_texture(out->shape) || glsl_type_is_sampler(out->shape)) {
      if (access || !nir_deref_mode_is_one_of(deref, nir_var_uniform | nir_var_image)) return false;
      out->deref = deref;
      out->sampler = s->types[out->type].resource == SpvOpTypeSampledImage ? deref : NULL;
      return true;
   }
   if (glsl_type_is_vector_or_scalar(out->shape)) {
      out->def = nir_load_deref_with_access(&s->b, deref, access);
      return true;
   }
   if (!children(s, out))
      return false;
   for (unsigned i = 0; i < out->element_count; i++) {
      if (!load(s, child_deref(&s->b, deref, i), access, &out->elements[i], depth + 1))
         return false;
   }
   return true;
}

bool
switch_nvk_ir_load(struct switch_nvk_ir_builder *s, nir_deref_instr *deref,
                  enum gl_access_qualifier access, struct switch_nvk_ir_value *out)
{
   return load(s, deref, access, out, 0);
}

bool
switch_nvk_ir_store(struct switch_nvk_ir_builder *s, nir_deref_instr *deref,
                   enum gl_access_qualifier access, const struct switch_nvk_ir_value *src)
{
   if (src->shape != deref->type)
      return false;
   if (src->def) {
      nir_store_deref_with_access(&s->b, deref, src->def,
                                 BITFIELD_MASK(src->def->num_components), access);
      return true;
   }
   if (!src->elements)
      return false;
   for (unsigned i = 0; i < src->element_count; i++) {
      if (!switch_nvk_ir_store(s, child_deref(&s->b, deref, i), access, &src->elements[i]))
         return false;
   }
   return true;
}

enum initializer { INITIAL_ZERO, INITIAL_PHI, INITIAL_UNDEF };

static bool
initialize(struct switch_nvk_ir_builder *s, struct switch_nvk_ir_value *out,
           const struct glsl_type *shape, enum initializer initial, unsigned depth)
{
   if (!shape || depth > 64)
      return false;
   out->shape = shape;
   if (glsl_type_is_vector_or_scalar(shape)) {
      unsigned n = glsl_get_vector_elements(shape), bits = glsl_get_bit_size(shape);
      if (initial == INITIAL_PHI) {
         nir_phi_instr *p = nir_phi_instr_create(s->b.shader);
         nir_def_init(&p->instr, &p->def, n, bits);
         nir_builder_instr_insert(&s->b, &p->instr);
         out->def = &p->def;
      } else if (initial == INITIAL_UNDEF) {
         out->def = nir_undef(&s->b, n, bits);
      } else {
         out->def = nir_imm_zero(&s->b, n, bits);
      }
      return true;
   }
   if (!children(s, out))
      return false;
   for (unsigned i = 0; i < out->element_count; i++) {
      if (!initialize(s, &out->elements[i], child_type(shape, i), initial, depth + 1))
         return false;
   }
   return true;
}

bool
switch_nvk_ir_zero(struct switch_nvk_ir_builder *s,
                  struct switch_nvk_ir_value *out, const struct glsl_type *shape)
{
   return initialize(s, out, shape, INITIAL_ZERO, 0);
}

bool
switch_nvk_ir_undef(struct switch_nvk_ir_builder *s,
                   struct switch_nvk_ir_value *out, const struct glsl_type *shape)
{
   return initialize(s, out, shape, INITIAL_UNDEF, 0);
}

bool
switch_nvk_ir_phi(struct switch_nvk_ir_builder *s,
                 struct switch_nvk_ir_value *out, const struct glsl_type *shape)
{
   return initialize(s, out, shape, INITIAL_PHI, 0);
}

bool
switch_nvk_ir_phi_source(struct switch_nvk_ir_value *out,
                        const struct switch_nvk_ir_value *src, nir_block *block)
{
   if (src->shape != out->shape || src->element_count != out->element_count)
      return false;
   if (out->def) {
      if (!src->def)
         return false;
      nir_phi_src *incoming = nir_phi_instr_add_src(nir_def_as_phi(out->def), block, src->def);
      list_addtail(&incoming->src.use_link, &src->def->uses);
      return true;
   }
   for (unsigned i = 0; i < out->element_count; i++) {
      if (!switch_nvk_ir_phi_source(&out->elements[i], &src->elements[i], block))
         return false;
   }
   return out->element_count != 0;
}

static bool
extract(struct switch_nvk_ir_builder *s, struct switch_nvk_ir_value *out,
        const struct switch_nvk_ir_value *src, const uint32_t *indices, unsigned count)
{
   for (unsigned i = 0; i < count; i++) {
      unsigned index = indices[i];
      if (src->elements && index < src->element_count) {
         src = &src->elements[index];
      } else if (src->def && glsl_type_is_vector(src->shape) &&
                 index < src->def->num_components && i + 1 == count) {
         out->def = nir_channel(&s->b, src->def, index);
         return out->shape == glsl_get_scalar_type(src->shape);
      } else {
         return false;
      }
   }
   if (out->shape != src->shape)
      return false;
   uint32_t type = out->type;
   *out = *src;
   out->type = type;
   return true;
}

static bool
insert(struct switch_nvk_ir_builder *s, struct switch_nvk_ir_value *out,
       const struct switch_nvk_ir_value *src, const struct switch_nvk_ir_value *object,
       const uint32_t *indices, unsigned count)
{
   if (!count) {
      if (src->shape != object->shape)
         return false;
      *out = *object;
      return true;
   }
   unsigned index = indices[0];
   *out = *src;
   if (src->elements && index < src->element_count) {
      if (!children(s, out))
         return false;
      memcpy(out->elements, src->elements, src->element_count * sizeof(*src->elements));
      return insert(s, &out->elements[index], &src->elements[index], object, indices + 1, count - 1);
   }
   if (!src->def || !object->def || !glsl_type_is_vector(src->shape) || count != 1 ||
       index >= src->def->num_components || object->shape != glsl_get_scalar_type(src->shape))
      return false;
   out->def = nir_vector_insert_imm(&s->b, src->def, object->def, index);
   return true;
}

bool
switch_nvk_ir_composite(struct switch_nvk_ir_builder *s,
                        const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   if (r->result >= s->program->values || r->type >= s->program->types)
      return false;
   struct switch_nvk_ir_value *out = &s->values[r->result];
   out->type = r->type;
   out->shape = s->types[r->type].type;
   if (!out->shape)
      return false;
   if (r->opcode == SpvOpCompositeExtract)
      return r->count >= 2 && r->count <= 65 && a[0] < s->program->values &&
         extract(s, out, &s->values[a[0]], a + 1, r->count - 1);
   if (r->opcode == SpvOpCompositeInsert) {
      if (r->count < 3 || r->count > 66 || a[0] >= s->program->values || a[1] >= s->program->values ||
          out->shape != s->values[a[1]].shape)
         return false;
      bool ok = insert(s, out, &s->values[a[1]], &s->values[a[0]], a + 2, r->count - 2);
      out->type = r->type;
      return ok;
   }
   for (unsigned i = 0; i < r->count; i++) {
      if (a[i] >= s->program->values)
         return false;
   }
   if (!glsl_type_is_vector_or_scalar(out->shape)) {
      if (!children(s, out) || out->element_count != r->count)
         return false;
      for (unsigned i = 0; i < r->count; i++) {
         if (s->values[a[i]].shape != child_type(out->shape, i))
            return false;
         out->elements[i] = s->values[a[i]];
      }
      return true;
   }
   unsigned components = glsl_get_vector_elements(out->shape), n = 0;
   nir_def *lanes[4];
   if (components > ARRAY_SIZE(lanes))
      return false;
   for (unsigned i = 0; i < r->count; i++) {
      nir_def *part = s->values[a[i]].def;
      if (!part || part->bit_size != glsl_get_bit_size(out->shape) ||
          part->num_components > components - n)
         return false;
      for (unsigned j = 0; j < part->num_components; j++)
         lanes[n++] = nir_channel(&s->b, part, j);
   }
   if (n != components)
      return false;
   out->def = nir_vec(&s->b, lanes, n);
   return true;
}
