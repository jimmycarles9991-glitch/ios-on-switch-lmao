// SPDX-License-Identifier: MIT
#include "nvk_switch_ir_internal.h"
#include "compiler/spirv/GLSL.std.450.h"
#include "compiler/nir/nir_builtin_builder.h"

nir_def *
switch_nvk_ir_glsl(nir_builder *b, uint32_t operation, nir_def **src,
                   unsigned count, unsigned components, unsigned bits, bool exact)
{
   if (operation == GLSLstd450Atan2) {
      if (count != 2 || !components || components > 4 || (bits != 16 && bits != 32 && bits != 64) ||
          !src[0] || !src[1] || src[0]->num_components != components || src[1]->num_components != components ||
          src[0]->bit_size != bits || src[1]->bit_size != bits)
         return NULL;
      const unsigned saved = b->fp_math_ctrl;
      if (exact) b->fp_math_ctrl |= nir_fp_exact;
      nir_def *out = nir_atan2(b, src[0], src[1]);
      b->fp_math_ctrl = saved;
      return out;
   }
   nir_op op;
   unsigned extra = 0;
   bool clamp = false;
   switch (operation) {
   case GLSLstd450Round: case GLSLstd450RoundEven: op = nir_op_fround_even; break;
   case GLSLstd450Trunc: op = nir_op_ftrunc; break;
   case GLSLstd450FAbs: op = nir_op_fabs; break;
   case GLSLstd450SAbs: op = nir_op_iabs; break;
   case GLSLstd450FSign: op = nir_op_fsign; break;
   case GLSLstd450SSign: op = nir_op_isign; break;
   case GLSLstd450Floor: op = nir_op_ffloor; break;
   case GLSLstd450Ceil: op = nir_op_fceil; break;
   case GLSLstd450Fract: op = nir_op_ffract; break;
   case GLSLstd450Sin: op = nir_op_fsin; break;
   case GLSLstd450Cos: op = nir_op_fcos; break;
   case GLSLstd450Pow: op = nir_op_fpow; break;
   case GLSLstd450Exp2: op = nir_op_fexp2; break;
   case GLSLstd450Log2: op = nir_op_flog2; break;
   case GLSLstd450Sqrt: op = nir_op_fsqrt; break;
   case GLSLstd450InverseSqrt: op = nir_op_frsq; break;
   case GLSLstd450UMin: op = nir_op_umin; break;
   case GLSLstd450UMax: op = nir_op_umax; break;
   case GLSLstd450SMin: op = nir_op_imin; break;
   case GLSLstd450SMax: op = nir_op_imax; break;
   case GLSLstd450NMin: extra |= nir_fp_preserve_nan; FALLTHROUGH;
   case GLSLstd450FMin: op = nir_op_fmin; extra |= nir_fp_preserve_inf; break;
   case GLSLstd450NMax: extra |= nir_fp_preserve_nan; FALLTHROUGH;
   case GLSLstd450FMax: op = nir_op_fmax; extra |= nir_fp_preserve_inf; break;
   case GLSLstd450NClamp: extra |= nir_fp_preserve_nan; FALLTHROUGH;
   case GLSLstd450FClamp: op = nir_op_fmin; extra |= nir_fp_preserve_inf; clamp = true; break;
   case GLSLstd450UClamp: op = nir_op_umin; clamp = true; break;
   case GLSLstd450SClamp: op = nir_op_imin; clamp = true; break;
   case GLSLstd450FMix: op = nir_op_flrp; break;
   case GLSLstd450Fma: op = nir_op_ffma_weak; break;
   case GLSLstd450FindILsb: op = nir_op_find_lsb; break;
   case GLSLstd450FindSMsb: op = nir_op_ifind_msb; break;
   case GLSLstd450FindUMsb: op = nir_op_ufind_msb; break;
   case GLSLstd450PackSnorm4x8: op = nir_op_pack_snorm_4x8; break;
   case GLSLstd450PackUnorm4x8: op = nir_op_pack_unorm_4x8; break;
   case GLSLstd450PackSnorm2x16: op = nir_op_pack_snorm_2x16; break;
   case GLSLstd450PackUnorm2x16: op = nir_op_pack_unorm_2x16; break;
   case GLSLstd450PackHalf2x16: op = nir_op_pack_half_2x16; break;
   case GLSLstd450UnpackSnorm4x8: op = nir_op_unpack_snorm_4x8; break;
   case GLSLstd450UnpackUnorm4x8: op = nir_op_unpack_unorm_4x8; break;
   case GLSLstd450UnpackSnorm2x16: op = nir_op_unpack_snorm_2x16; break;
   case GLSLstd450UnpackUnorm2x16: op = nir_op_unpack_unorm_2x16; break;
   case GLSLstd450UnpackHalf2x16: op = nir_op_unpack_half_2x16; break;
   default: return NULL;
   }
   const nir_op_info *info = &nir_op_infos[op];
   if (count != (clamp ? 3 : info->num_inputs) || count > 3 || !components || components > 4 ||
       (bits != 16 && bits != 32 && bits != 64) ||
       (info->output_size && info->output_size != components) ||
       (nir_alu_type_get_type_size(info->output_type) && nir_alu_type_get_type_size(info->output_type) != bits))
      return NULL;
   for (unsigned i = 0; i < count; ++i) {
      unsigned input = clamp ? 0 : i;
      unsigned size = nir_alu_type_get_type_size(info->input_types[input]);
      unsigned width = info->input_sizes[input];
      if (!src[i] || src[i]->bit_size != (size ? size : bits) ||
          src[i]->num_components != (width ? width : components))
         return NULL;
   }
   unsigned saved = b->fp_math_ctrl;
   if (operation == GLSLstd450FClamp) b->fp_math_ctrl = nir_fp_preserve_inf;
   b->fp_math_ctrl |= extra;
   if (exact) b->fp_math_ctrl |= nir_fp_exact;
   nir_def *out;
   if (clamp) {
      if (op == nir_op_fmin) out = nir_fclamp(b, src[0], src[1], src[2]);
      else if (op == nir_op_umin) out = nir_uclamp(b, src[0], src[1], src[2]);
      else out = nir_iclamp(b, src[0], src[1], src[2]);
   } else {
      out = nir_build_alu(b, op, src[0], count > 1 ? src[1] : NULL, count > 2 ? src[2] : NULL, NULL);
   }
   b->fp_math_ctrl = saved;
   return out;
}
