#ifndef NVK_SWITCH_IR_H
#define NVK_SWITCH_IR_H

#include <stddef.h>
#include <stdint.h>

struct switch_nvk_ir_record {
   uint32_t kind, opcode, result, type, first, count, flags, auxiliary;
};

struct switch_nvk_ir {
   uint32_t version, types, values, blocks, entry;
   uint32_t record_count, operand_count;
   const struct switch_nvk_ir_record *records;
   const uint32_t *operands;
};

struct nir_shader;
struct nir_shader_compiler_options;
struct spirv_to_nir_options;
struct VkSpecializationInfo;

struct nir_shader *switch_nvk_ir_to_nir(
   const struct switch_nvk_ir *program,
   const struct nir_shader_compiler_options *options,
   const struct spirv_to_nir_options *address_options,
   const struct VkSpecializationInfo *specialization);

#endif
