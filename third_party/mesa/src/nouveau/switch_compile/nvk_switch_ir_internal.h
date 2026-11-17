#ifndef NVK_SWITCH_IR_INTERNAL_H
#define NVK_SWITCH_IR_INTERNAL_H

#include "nvk_switch_ir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv.h"
#include <vulkan/vulkan_core.h>

struct switch_nvk_ir_type {
   const struct glsl_type *type;
   uint32_t pointee;
   SpvStorageClass storage;
   uint32_t resource;
   enum pipe_format image_format;
};

struct switch_nvk_ir_value {
   nir_def *def;
   nir_deref_instr *deref;
   nir_deref_instr *sampler;
   const struct glsl_type *shape;
   struct switch_nvk_ir_value *elements;
   uint32_t element_count;
   uint32_t type;
};

struct switch_nvk_ir_builder {
   const struct switch_nvk_ir *program;
   const struct spirv_to_nir_options *address_options;
   const VkSpecializationInfo *specialization;
   nir_builder b;
   struct switch_nvk_ir_type *types;
   struct switch_nvk_ir_value *values;
   nir_block **blocks;
   uint32_t aggregate_elements;
   uint32_t preserve_float_widths;
};

bool switch_nvk_ir_load(struct switch_nvk_ir_builder *s, nir_deref_instr *deref,
                       enum gl_access_qualifier access, struct switch_nvk_ir_value *out);
bool switch_nvk_ir_store(struct switch_nvk_ir_builder *s, nir_deref_instr *deref,
                        enum gl_access_qualifier access, const struct switch_nvk_ir_value *src);
bool switch_nvk_ir_composite(struct switch_nvk_ir_builder *s,
                             const struct switch_nvk_ir_record *r);
bool switch_nvk_ir_zero(struct switch_nvk_ir_builder *s,
                       struct switch_nvk_ir_value *out, const struct glsl_type *shape);
bool switch_nvk_ir_undef(struct switch_nvk_ir_builder *s,
                        struct switch_nvk_ir_value *out, const struct glsl_type *shape);
bool switch_nvk_ir_phi(struct switch_nvk_ir_builder *s,
                      struct switch_nvk_ir_value *out, const struct glsl_type *shape);
bool switch_nvk_ir_phi_source(struct switch_nvk_ir_value *out,
                             const struct switch_nvk_ir_value *src, nir_block *block);

nir_def *switch_nvk_ir_alu(nir_builder *b, SpvOp operation,
                          nir_def **src, unsigned count,
                          unsigned components, unsigned bits, bool exact);
nir_def *switch_nvk_ir_glsl(nir_builder *b, uint32_t operation,
                           nir_def **src, unsigned count,
                           unsigned components, unsigned bits, bool exact);
bool switch_nvk_ir_barrier(struct switch_nvk_ir_builder *s,
                          const struct switch_nvk_ir_record *r);
bool switch_nvk_ir_access_barrier(struct switch_nvk_ir_builder *s,
                                 nir_deref_instr *deref, uint32_t memory_scope, bool load);
bool switch_nvk_ir_declaration(struct switch_nvk_ir_builder *s,
                               const struct switch_nvk_ir_record *r);
bool switch_nvk_ir_instruction(struct switch_nvk_ir_builder *s,
                               const struct switch_nvk_ir_record *r);
bool switch_nvk_ir_image_type(struct switch_nvk_ir_builder *s,
                              const struct switch_nvk_ir_record *r);
bool switch_nvk_ir_image(struct switch_nvk_ir_builder *s,
                         const struct switch_nvk_ir_record *r);

static inline const uint32_t *switch_nvk_ir_args(
   const struct switch_nvk_ir_builder *s,
   const struct switch_nvk_ir_record *r)
{
   return s->program->operands + r->first;
}

#endif
