#include "nvk_switch_ir_internal.h"
#include "util/mesa-blake3.h"

static bool
emit(struct switch_nvk_ir_builder *s)
{
   const struct switch_nvk_ir *p = s->program;
   if (p->version != 1 || !p->types || !p->values || !p->blocks || p->entry >= p->blocks ||
       p->types > (1u << 20) || p->values > (1u << 24) || p->blocks > (1u << 20) ||
       p->record_count > (1u << 24) || p->operand_count > (1u << 26) ||
       !p->records || !p->operands)
      return false;
   s->types = rzalloc_array(s->b.shader, struct switch_nvk_ir_type, p->types);
   s->values = rzalloc_array(s->b.shader, struct switch_nvk_ir_value, p->values);
   s->blocks = rzalloc_array(s->b.shader, nir_block *, p->blocks);
   if (!s->types || !s->values || !s->blocks)
      return false;
   s->b.impl->structured = false;
   s->blocks[p->entry] = nir_start_block(s->b.impl);
   for (unsigned i = 0; i < p->record_count; i++) {
      const struct switch_nvk_ir_record *r = &p->records[i];
      if (r->first > p->operand_count || r->count > p->operand_count - r->first)
         return false;
      if (r->kind != 4) continue;
      if (r->result >= p->blocks) return false;
      if (r->result == p->entry) continue;
      if (s->blocks[r->result]) return false;
      nir_block *block = nir_block_create(s->b.impl);
      exec_list_push_tail(&s->b.impl->body, &block->cf_node.node);
      block->cf_node.parent = &s->b.impl->cf_node;
      s->blocks[r->result] = block;
   }
   for (unsigned i = 0; i < p->record_count; i++) {
      const struct switch_nvk_ir_record *r = &p->records[i];
      const uint32_t *a = switch_nvk_ir_args(s, r);
      switch (r->kind) {
      case 0: case 1: case 2: case 3: case 8:
         if (!switch_nvk_ir_declaration(s, r)) return false;
         break;
      case 4:
         s->b.cursor = nir_after_block(s->blocks[r->result]);
         break;
      case 5: {
         if (r->result >= p->values || r->type >= p->types || r->count % 2)
            return false;
         s->values[r->result].type = r->type;
         if (!switch_nvk_ir_phi(s, &s->values[r->result], s->types[r->type].type))
            return false;
         break;
      }
      case 6:
         if (!switch_nvk_ir_instruction(s, r)) return false;
         break;
      case 7:
         if (r->opcode == SpvOpReturn && r->count == 0) {
            nir_goto(&s->b, s->b.impl->end_block);
         } else if (r->opcode == SpvOpBranch && r->count == 1 && a[0] < p->blocks && s->blocks[a[0]]) {
            nir_goto(&s->b, s->blocks[a[0]]);
         } else if (r->opcode == SpvOpBranchConditional && r->count == 3 &&
                    a[0] < p->values && s->values[a[0]].def && a[1] < p->blocks && a[2] < p->blocks &&
                    s->values[a[0]].def->bit_size == 1 && s->values[a[0]].def->num_components == 1 &&
                    s->blocks[a[1]] && s->blocks[a[2]]) {
            if (a[1] == a[2]) nir_goto(&s->b, s->blocks[a[1]]);
            else nir_goto_if(&s->b, s->blocks[a[1]], s->values[a[0]].def, s->blocks[a[2]]);
         } else return false;
         break;
      default: return false;
      }
   }
   for (unsigned i = 0; i < p->record_count; i++) {
      const struct switch_nvk_ir_record *r = &p->records[i];
      if (r->kind != 5) continue;
      const uint32_t *a = switch_nvk_ir_args(s, r);
      for (unsigned j = 0; j < r->count; j += 2) {
         if (a[j] >= p->values ||
             a[j + 1] >= p->blocks || !s->blocks[a[j + 1]])
            return false;
         if (!switch_nvk_ir_phi_source(&s->values[r->result], &s->values[a[j]], s->blocks[a[j + 1]]))
            return false;
      }
   }
   return !s->b.shader->info.workgroup_size_variable;
}

nir_shader *
switch_nvk_ir_to_nir(const struct switch_nvk_ir *program,
                      const nir_shader_compiler_options *options,
                      const struct spirv_to_nir_options *address_options,
                      const VkSpecializationInfo *specialization)
{
   if (!program) return NULL;
   struct switch_nvk_ir_builder s = {
      .program = program, .address_options = address_options, .specialization = specialization,
      .b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options, "Switch native semantic stage"),
   };
   s.b.shader->info.internal = false;
   s.b.shader->info.workgroup_size_variable = true;
   s.b.shader->info.float_controls_execution_mode = address_options->float_controls_execution_mode;
   if (!emit(&s)) {
      ralloc_free(s.b.shader);
      return NULL;
   }
   nir_rematerialize_derefs_in_use_blocks_impl(s.b.impl);
   nir_sort_unstructured_blocks(s.b.impl);
   nir_lower_goto_ifs(s.b.shader);
   nir_lower_continue_constructs(s.b.shader);
   nir_lower_returns(s.b.shader);
   nir_opt_dce(s.b.shader);
   nir_opt_copy_prop(s.b.shader);
   nir_opt_constant_folding(s.b.shader);
   nir_opt_deref(s.b.shader);
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   _mesa_blake3_update(&hash, program->records, program->record_count * sizeof(*program->records));
   _mesa_blake3_update(&hash, program->operands, program->operand_count * sizeof(*program->operands));
   _mesa_blake3_final(&hash, s.b.shader->info.source_blake3);
   nir_validate_shader(s.b.shader, "Switch typed semantic IR");
   return s.b.shader;
}
