#include "nvk_switch_ir_internal.h"

static const struct glsl_type *
type_of(struct switch_nvk_ir_builder *s, uint32_t id)
{
   return id < s->program->types ? s->types[id].type : NULL;
}

static nir_def *
value_of(struct switch_nvk_ir_builder *s, uint32_t id)
{
   return id < s->program->values ? s->values[id].def : NULL;
}

static bool
constant_uint(struct switch_nvk_ir_builder *s, uint32_t id, uint64_t *value)
{
   nir_def *def = value_of(s, id);
   if (def == NULL || def->num_components != 1 ||
       nir_def_instr_type(def) != nir_instr_type_load_const)
      return false;
   *value = nir_src_as_uint(nir_src_for_ssa(def));
   return true;
}

static bool
declare_type(struct switch_nvk_ir_builder *s,
              const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   if (r->result >= s->program->types)
      return false;
   struct switch_nvk_ir_type *out = &s->types[r->result];
   switch (r->opcode) {
   case SpvOpTypeVoid: case SpvOpTypeFunction:
      out->type = glsl_void_type(); break;
   case SpvOpTypeBool: out->type = glsl_bool_type(); break;
   case SpvOpTypeInt:
      if (r->count != 2 || (a[0] != 8 && a[0] != 16 && a[0] != 32 && a[0] != 64))
         return false;
      out->type = a[1] ? glsl_intN_t_type(a[0]) : glsl_uintN_t_type(a[0]);
      break;
   case SpvOpTypeFloat:
      if (r->count != 1 || (a[0] != 16 && a[0] != 32 && a[0] != 64))
         return false;
      out->type = glsl_floatN_t_type(a[0]); break;
   case SpvOpTypeVector:
      if (r->count != 2 || !type_of(s, a[0]) || a[1] < 2 || a[1] > 4)
         return false;
      out->type = glsl_vector_type(glsl_get_base_type(type_of(s, a[0])), a[1]);
      break;
   case SpvOpTypeArray: case SpvOpTypeRuntimeArray: {
      uint64_t length = 0;
      const bool sized = r->opcode == SpvOpTypeArray;
      if (r->count != (sized ? 2 : 1) || !type_of(s, a[0]) ||
          (sized && (!constant_uint(s, a[1], &length) || !length || length > UINT32_MAX)))
         return false;
      out->type = glsl_array_type(type_of(s, a[0]), length, r->flags);
      out->resource = s->types[a[0]].resource;
      out->image_format = s->types[a[0]].image_format;
      break;
   }
   case SpvOpTypeStruct: {
      if (r->count % 2 || r->count > 1024)
         return false;
      unsigned n = r->count / 2;
      struct glsl_struct_field *fields = rzalloc_array(s->b.shader, struct glsl_struct_field, n);
      if (!fields)
         return false;
      for (unsigned i = 0; i < n; i++) {
         fields[i].type = type_of(s, a[2 * i]);
         fields[i].offset = (int32_t)a[2 * i + 1];
         fields[i].name = "";
         if (!fields[i].type)
            return false;
      }
      out->type = glsl_struct_type(fields, n, "", false);
      ralloc_free(fields);
      break;
   }
   case SpvOpTypePointer:
      if (r->count != 2 || !type_of(s, a[1]))
         return false;
      out->storage = a[0];
      out->pointee = a[1];
      out->type = type_of(s, a[1]);
      break;
   case SpvOpTypeImage: case SpvOpTypeSampler: case SpvOpTypeSampledImage:
      return switch_nvk_ir_image_type(s, r);
   default: return false;
   }
   return out->type != NULL;
}

static bool
declare_constant(struct switch_nvk_ir_builder *s,
                  const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   const struct glsl_type *type = type_of(s, r->type);
   if (r->result >= s->program->values || !type)
      return false;
   if (r->opcode == SpvOpConstantNull) {
      s->values[r->result].type = r->type;
      return switch_nvk_ir_zero(s, &s->values[r->result], type);
   }
   if (r->opcode == SpvOpConstantComposite && !r->auxiliary)
      return switch_nvk_ir_composite(s, r);
   if (!glsl_type_is_vector_or_scalar(type))
      return false;
   unsigned bits = glsl_get_bit_size(type), components = glsl_get_vector_elements(type);
   nir_def *out = NULL;
   switch (r->opcode) {
   case SpvOpConstant: case SpvOpSpecConstant: {
      if (r->count != (r->opcode == SpvOpSpecConstant ? 3 : 2))
         return false;
      uint64_t value = a[0] | ((uint64_t)a[1] << 32);
      if (r->opcode == SpvOpSpecConstant && s->specialization) {
         const VkSpecializationInfo *info = s->specialization;
         for (unsigned i = 0; i < info->mapEntryCount; i++) {
            const VkSpecializationMapEntry *entry = &info->pMapEntries[i];
            if (entry->constantID != a[2])
               continue;
            if (entry->size > sizeof(value) || entry->offset > info->dataSize ||
                entry->size > info->dataSize - entry->offset)
               return false;
            value = 0;
            memcpy(&value, (const char *)info->pData + entry->offset, entry->size);
            break;
         }
      }
      out = nir_imm_intN_t(&s->b, bits == 1 ? value != 0 : value, bits);
      break;
   }
   case SpvOpConstantNull: out = nir_imm_zero(&s->b, components, bits); break;
   case SpvOpConstantComposite: {
      if (r->count != components || components > 4)
         return false;
      nir_def *parts[4];
      for (unsigned i = 0; i < components; i++) {
         parts[i] = value_of(s, a[i]);
         if (!parts[i] || parts[i]->num_components != 1 || parts[i]->bit_size != bits)
            return false;
      }
      out = nir_vec(&s->b, parts, components);
      if (r->auxiliary) {
         for (unsigned i = 0; i < 3; i++) {
            uint64_t size;
            if (components != 3 || !constant_uint(s, a[i], &size) || !size || size > 1024)
               return false;
            s->b.shader->info.workgroup_size[i] = size;
         }
         s->b.shader->info.workgroup_size_variable = false;
      }
      break;
   }
   case SpvOpSpecConstantOp: {
      if (r->count < 2 || r->count > 3 || components != 1)
         return false;
      uint64_t src[3];
      for (unsigned i = 0; i < r->count; i++) {
         if (!constant_uint(s, a[i], &src[i]))
            return false;
      }
      uint64_t value;
      switch (r->auxiliary) {
      case SpvOpIAdd: value = src[0] + src[1]; break;
      case SpvOpIMul: value = src[0] * src[1]; break;
      case SpvOpBitwiseAnd: value = src[0] & src[1]; break;
      case SpvOpShiftRightLogical: value = src[1] < 64 ? src[0] >> src[1] : 0; break;
      case SpvOpULessThan: value = src[0] < src[1]; break;
      case SpvOpSelect:
         if (r->count != 3) return false;
         value = src[0] ? src[1] : src[2]; break;
      default: return false;
      }
      out = nir_imm_intN_t(&s->b, value, bits);
      break;
   }
   default: return false;
   }
   s->values[r->result] = (struct switch_nvk_ir_value) { .def = out, .shape = type, .type = r->type };
   return out != NULL;
}

static gl_system_value
builtin_location(uint32_t builtin)
{
   switch (builtin) {
   case SpvBuiltInNumWorkgroups: return SYSTEM_VALUE_NUM_WORKGROUPS;
   case SpvBuiltInWorkgroupId: return SYSTEM_VALUE_WORKGROUP_ID;
   case SpvBuiltInLocalInvocationId: return SYSTEM_VALUE_LOCAL_INVOCATION_ID;
   case SpvBuiltInGlobalInvocationId: return SYSTEM_VALUE_GLOBAL_INVOCATION_ID;
   case SpvBuiltInLocalInvocationIndex: return SYSTEM_VALUE_LOCAL_INVOCATION_INDEX;
   case SpvBuiltInSubgroupLocalInvocationId: return SYSTEM_VALUE_SUBGROUP_INVOCATION;
   case SpvBuiltInSubgroupSize: return SYSTEM_VALUE_SUBGROUP_SIZE;
   case SpvBuiltInSubgroupId: return SYSTEM_VALUE_SUBGROUP_ID;
   case SpvBuiltInNumSubgroups: return SYSTEM_VALUE_NUM_SUBGROUPS;
   default: return SYSTEM_VALUE_MAX;
   }
}

static bool
declare_variable(struct switch_nvk_ir_builder *s,
                  const struct switch_nvk_ir_record *r)
{
   const uint32_t *a = switch_nvk_ir_args(s, r);
   if (r->result >= s->program->values || r->count != (r->kind == 3 ? 2 : 6) ||
       r->type >= s->program->types || !type_of(s, a[0]))
      return false;
   nir_variable_mode mode;
   SpvStorageClass storage = r->kind == 3 ? SpvStorageClassFunction : a[1];
   switch (storage) {
   case SpvStorageClassUniform: mode = nir_var_mem_ubo; break;
   case SpvStorageClassStorageBuffer: mode = nir_var_mem_ssbo; break;
   case SpvStorageClassPushConstant: mode = nir_var_mem_push_const; break;
   case SpvStorageClassPrivate: mode = nir_var_shader_temp; break;
   case SpvStorageClassWorkgroup: mode = nir_var_mem_shared; break;
   case SpvStorageClassFunction: mode = nir_var_function_temp; break;
   case SpvStorageClassInput: mode = nir_var_system_value; break;
   case SpvStorageClassUniformConstant:
      if (!s->types[a[0]].resource) return false;
      mode = glsl_type_is_image(glsl_without_array(type_of(s, a[0]))) ? nir_var_image : nir_var_uniform;
      break;
   default: return false;
   }
   nir_deref_instr *deref;
   if (mode == nir_var_mem_ubo || mode == nir_var_mem_ssbo) {
      if (a[3] != 0 || !glsl_type_is_struct(type_of(s, a[0])))
         return false;
      nir_address_format format = mode == nir_var_mem_ubo ?
         s->address_options->ubo_addr_format : s->address_options->ssbo_addr_format;
      unsigned components = nir_address_format_num_components(format);
      unsigned bits = nir_address_format_bit_size(format);
      nir_descriptor_type desc = mode == nir_var_mem_ubo ?
         nir_descriptor_type_uniform_buffer : nir_descriptor_type_storage_buffer;
      nir_def *index = nir_vulkan_resource_index(&s->b, components, bits,
         nir_imm_int(&s->b, 0), .desc_set = a[4], .binding = a[5], .desc_type = desc,
         .resource_type = mode == nir_var_mem_ubo ? nir_resource_type_uniform_buffer :
                                                 nir_resource_type_read_write_storage_buffer);
      nir_def *address = nir_load_vulkan_descriptor(&s->b, components, bits, index, .desc_type = desc);
      deref = nir_build_deref_cast(&s->b, address, mode, type_of(s, a[0]), 0);
      deref->cast.align_mul = mode == nir_var_mem_ubo ?
         s->address_options->min_ubo_alignment : s->address_options->min_ssbo_alignment;
      deref->cast.align_offset = 0;
      if (mode == nir_var_mem_ubo) s->b.shader->info.num_ubos++;
      else s->b.shader->info.num_ssbos++;
   } else {
      nir_variable *var = mode == nir_var_function_temp ?
         nir_local_variable_create(s->b.impl, type_of(s, a[0]), NULL) :
         nir_variable_create(s->b.shader, mode, type_of(s, a[0]), NULL);
      if (mode == nir_var_system_value) {
         if (a[3] != 2 || builtin_location(a[4]) == SYSTEM_VALUE_MAX)
            return false;
         var->data.location = builtin_location(a[4]);
      }
      if (mode == nir_var_mem_push_const)
         s->b.shader->num_uniforms = glsl_get_explicit_size(var->type, false);
      if (mode == nir_var_uniform || mode == nir_var_image) {
         if (a[3] != 0 || a[2] != UINT32_MAX) return false;
         var->data.descriptor_set = a[4];
         var->data.binding = a[5];
         var->data.explicit_binding = true;
         var->data.image.format = s->types[a[0]].image_format;
         if (mode == nir_var_image) {
            var->data.resource_type = nir_resource_type_read_write_image;
            var->data.access |= ACCESS_RESTRICT;
         } else {
            var->data.resource_type = s->types[a[0]].resource == SpvOpTypeSampler ?
               nir_resource_type_sampler : s->types[a[0]].resource == SpvOpTypeSampledImage ?
               nir_resource_type_combined_sampled_image : nir_resource_type_sampled_image;
         }
      }
      deref = nir_build_deref_var(&s->b, var);
   }
   s->values[r->result] = (struct switch_nvk_ir_value) { .deref = deref, .type = r->type };
   uint32_t init = r->kind == 3 ? a[1] : a[2];
   if (init != UINT32_MAX) {
      if (init >= s->program->values || mode == nir_var_mem_shared ||
          !switch_nvk_ir_store(s, deref, 0, &s->values[init]))
         return false;
   }
   return true;
}

bool
switch_nvk_ir_declaration(struct switch_nvk_ir_builder *s,
                          const struct switch_nvk_ir_record *r)
{
   switch (r->kind) {
   case 0: return declare_type(s, r);
   case 1: return declare_constant(s, r);
   case 2: case 3: return declare_variable(s, r);
   case 8:
      if ((r->result == 2 || r->result == 3) && r->count == 0 && !r->flags && !r->auxiliary) {
         if (r->result == 2) s->b.shader->info.maximally_reconverges = true;
         return true;
      }
      if (r->result == 1) {
         if (r->count || r->auxiliary || (r->flags != 16 && r->flags != 32 && r->flags != 64))
            return false;
         s->preserve_float_widths |= r->flags;
         return true;
      }
      if (r->result != 0 || r->count != 3 || r->flags || r->auxiliary)
         return false;
      for (unsigned i = 0; i < 3; i++) {
         uint32_t size = switch_nvk_ir_args(s, r)[i];
         if (!size || size > 1024) return false;
         s->b.shader->info.workgroup_size[i] = size;
      }
      s->b.shader->info.workgroup_size_variable = false;
      return true;
   default: return false;
   }
}
