#include "nvk_switch_ir_internal.h"

static nir_def *
build_alu(nir_builder *b, SpvOp operation, nir_def **s,
                  unsigned count, unsigned components, unsigned bits,
                  bool exact)
{
   nir_def *a = count > 0 ? s[0] : NULL;
   nir_def *c = count > 1 ? s[1] : NULL;
   nir_def *d = count > 2 ? s[2] : NULL;
   nir_op op;
   switch (operation) {
   case SpvOpSNegate: op = nir_op_ineg; break;
   case SpvOpFNegate: op = nir_op_fneg; break;
   case SpvOpNot: case SpvOpLogicalNot: op = nir_op_inot; break;
   case SpvOpBitReverse: op = nir_op_bitfield_reverse; break;
   case SpvOpBitCount: op = nir_op_bit_count; break;
   case SpvOpIsNan: return nir_fneu(b, a, a);
   case SpvOpIsInf:
      return nir_feq(b, nir_fabs(b, a), nir_imm_floatN_t(b, INFINITY, a->bit_size));
   case SpvOpCopyObject: return a;
   case SpvOpQuantizeToF16: op = nir_op_fquantize2f16; break;
   case SpvOpIAdd: op = nir_op_iadd; break;
   case SpvOpISub: op = nir_op_isub; break;
   case SpvOpIMul: op = nir_op_imul; break;
   case SpvOpUDiv: op = nir_op_udiv; break;
   case SpvOpSDiv: op = nir_op_idiv; break;
   case SpvOpUMod: op = nir_op_umod; break;
   case SpvOpSRem: op = nir_op_irem; break;
   case SpvOpSMod: op = nir_op_imod; break;
   case SpvOpFAdd: op = nir_op_fadd; break;
   case SpvOpFSub: op = nir_op_fsub; break;
   case SpvOpFMul: case SpvOpVectorTimesScalar: op = nir_op_fmul; break;
   case SpvOpFDiv: op = nir_op_fdiv; break;
   case SpvOpFRem: op = nir_op_frem; break;
   case SpvOpShiftRightLogical: op = nir_op_ushr; break;
   case SpvOpShiftRightArithmetic: op = nir_op_ishr; break;
   case SpvOpShiftLeftLogical: op = nir_op_ishl; break;
   case SpvOpBitwiseOr: case SpvOpLogicalOr: op = nir_op_ior; break;
   case SpvOpBitwiseXor: op = nir_op_ixor; break;
   case SpvOpBitwiseAnd: case SpvOpLogicalAnd: op = nir_op_iand; break;
   case SpvOpIEqual: case SpvOpLogicalEqual: op = nir_op_ieq; break;
   case SpvOpINotEqual: case SpvOpLogicalNotEqual: op = nir_op_ine; break;
   case SpvOpULessThan: op = nir_op_ult; break;
   case SpvOpSLessThan: op = nir_op_ilt; break;
   case SpvOpUGreaterThanEqual: op = nir_op_uge; break;
   case SpvOpSGreaterThanEqual: op = nir_op_ige; break;
   case SpvOpUGreaterThan: return nir_ult(b, c, a);
   case SpvOpSGreaterThan: return nir_ilt(b, c, a);
   case SpvOpULessThanEqual: return nir_uge(b, c, a);
   case SpvOpSLessThanEqual: return nir_ige(b, c, a);
   case SpvOpFOrdEqual: op = nir_op_feq; break;
   case SpvOpFUnordNotEqual: op = nir_op_fneu; break;
   case SpvOpFOrdLessThan: op = nir_op_flt; break;
   case SpvOpFOrdGreaterThanEqual: op = nir_op_fge; break;
   case SpvOpFOrdGreaterThan: return nir_flt(b, c, a);
   case SpvOpFOrdLessThanEqual: return nir_fge(b, c, a);
   case SpvOpFOrdNotEqual: return nir_iand(b, nir_fneu(b, a, c), nir_ford(b, a, c));
   case SpvOpFUnordEqual: return nir_ior(b, nir_feq(b, a, c), nir_funord(b, a, c));
   case SpvOpFUnordLessThan: return nir_inot(b, nir_fge(b, a, c));
   case SpvOpFUnordGreaterThan: return nir_inot(b, nir_fge(b, c, a));
   case SpvOpFUnordLessThanEqual: return nir_inot(b, nir_flt(b, c, a));
   case SpvOpFUnordGreaterThanEqual: return nir_inot(b, nir_flt(b, a, c));
   case SpvOpSelect: return nir_bcsel(b, a, c, d);
   case SpvOpFmaKHR: op = nir_op_ffma; break;
   case SpvOpDot: return nir_fdot(b, a, c);
   case SpvOpBitFieldInsert: op = nir_op_bitfield_insert; break;
   case SpvOpBitFieldSExtract: op = nir_op_ibitfield_extract; break;
   case SpvOpBitFieldUExtract: op = nir_op_ubitfield_extract; break;
   case SpvOpConvertFToU: return nir_f2uN(b, a, bits);
   case SpvOpConvertFToS: return nir_f2iN(b, a, bits);
   case SpvOpConvertSToF: return nir_i2fN(b, a, bits);
   case SpvOpConvertUToF: return nir_u2fN(b, a, bits);
   case SpvOpUConvert: return nir_u2uN(b, a, bits);
   case SpvOpSConvert: return nir_i2iN(b, a, bits);
   case SpvOpFConvert:
      return exact && bits == 16 ? nir_f2f16_rtne(b, a) : nir_f2fN(b, a, bits);
   case SpvOpBitcast:
      return a->bit_size == bits ? a : nir_bitcast_vector(b, a, bits);
   case SpvOpVectorExtractDynamic: return nir_vector_extract(b, a, c);
   case SpvOpVectorInsertDynamic: return nir_vector_insert(b, a, c, d);
   case SpvOpUndef: return nir_undef(b, components, bits);
   case SpvOpAny: case SpvOpAll: {
      nir_def *v = nir_channel(b, a, 0);
      for (unsigned i = 1; i < a->num_components; i++)
         v = operation == SpvOpAny ? nir_ior(b, v, nir_channel(b, a, i)) :
                                    nir_iand(b, v, nir_channel(b, a, i));
      return v;
   }
   default: return NULL;
   }
   if (count != nir_op_infos[op].num_inputs || count > 4)
      return NULL;
   return nir_build_alu(b, op, a, c, d, count > 3 ? s[3] : NULL);
}

static bool
alu_shape(SpvOp operation, nir_def **src, unsigned count,
          unsigned components, unsigned bits)
{
   unsigned expected;
   switch (operation) {
   case SpvOpSNegate: case SpvOpFNegate: case SpvOpNot: case SpvOpLogicalNot:
   case SpvOpBitReverse: case SpvOpBitCount: case SpvOpQuantizeToF16:
   case SpvOpIsNan: case SpvOpIsInf: case SpvOpCopyObject:
   case SpvOpConvertFToU: case SpvOpConvertFToS: case SpvOpConvertSToF: case SpvOpConvertUToF:
   case SpvOpUConvert: case SpvOpSConvert: case SpvOpFConvert: case SpvOpBitcast:
   case SpvOpAny: case SpvOpAll:
      expected = 1; break;
   case SpvOpIAdd: case SpvOpISub: case SpvOpIMul: case SpvOpUDiv: case SpvOpSDiv:
   case SpvOpUMod: case SpvOpSRem: case SpvOpSMod:
   case SpvOpFAdd: case SpvOpFSub: case SpvOpFMul: case SpvOpFDiv: case SpvOpFRem:
   case SpvOpVectorTimesScalar: case SpvOpDot:
   case SpvOpShiftRightLogical: case SpvOpShiftRightArithmetic: case SpvOpShiftLeftLogical:
   case SpvOpBitwiseOr: case SpvOpLogicalOr: case SpvOpBitwiseXor:
   case SpvOpBitwiseAnd: case SpvOpLogicalAnd:
   case SpvOpIEqual: case SpvOpLogicalEqual: case SpvOpINotEqual: case SpvOpLogicalNotEqual:
   case SpvOpULessThan: case SpvOpSLessThan: case SpvOpUGreaterThanEqual: case SpvOpSGreaterThanEqual:
   case SpvOpFOrdEqual: case SpvOpFUnordNotEqual: case SpvOpFOrdLessThan: case SpvOpFOrdGreaterThanEqual:
   case SpvOpUGreaterThan: case SpvOpSGreaterThan: case SpvOpULessThanEqual: case SpvOpSLessThanEqual:
   case SpvOpFOrdGreaterThan: case SpvOpFOrdLessThanEqual: case SpvOpFOrdNotEqual:
   case SpvOpFUnordEqual: case SpvOpFUnordLessThan: case SpvOpFUnordGreaterThan:
   case SpvOpFUnordLessThanEqual: case SpvOpFUnordGreaterThanEqual: case SpvOpVectorExtractDynamic:
      expected = 2; break;
   case SpvOpSelect: case SpvOpVectorInsertDynamic: case SpvOpFmaKHR:
   case SpvOpBitFieldSExtract: case SpvOpBitFieldUExtract:
      expected = 3; break;
   case SpvOpBitFieldInsert: expected = 4; break;
   case SpvOpUndef: expected = 0; break;
   default: return false;
   }
   if (count != expected || count > 4 || !components || components > 4 ||
       (bits != 1 && bits != 8 && bits != 16 && bits != 32 && bits != 64))
      return false;
   for (unsigned i = 0; i < count; i++) {
      if (!src[i]) return false;
   }
   switch (operation) {
   case SpvOpUndef: return true;
   case SpvOpDot:
      return components == 1 && bits >= 16 && src[0]->bit_size == bits && src[1]->bit_size == bits &&
         src[0]->num_components > 1 && src[0]->num_components <= 4 &&
         src[0]->num_components == src[1]->num_components;
   case SpvOpSelect:
      return src[0]->bit_size == 1 && (src[0]->num_components == 1 || src[0]->num_components == components) &&
         src[1]->bit_size == bits && src[2]->bit_size == bits &&
         src[1]->num_components == components && src[2]->num_components == components;
   case SpvOpBitcast:
      return bits != 1 && src[0]->bit_size != 1 &&
         src[0]->num_components * src[0]->bit_size == components * bits;
   case SpvOpVectorExtractDynamic:
      return components == 1 && src[0]->bit_size == bits && src[0]->num_components > 1 &&
         src[1]->num_components == 1 && src[1]->bit_size == 32;
   case SpvOpVectorInsertDynamic:
      return src[0]->num_components == components && src[0]->bit_size == bits &&
         src[1]->num_components == 1 && src[1]->bit_size == bits &&
         src[2]->num_components == 1 && src[2]->bit_size == 32;
   case SpvOpAny: case SpvOpAll:
      return components == 1 && bits == 1 && src[0]->bit_size == 1;
   case SpvOpConvertFToU: case SpvOpConvertFToS: case SpvOpConvertSToF: case SpvOpConvertUToF:
   case SpvOpUConvert: case SpvOpSConvert: case SpvOpFConvert:
      return src[0]->num_components == components && src[0]->bit_size != 1 && bits != 1 &&
         (operation != SpvOpFConvert || (src[0]->bit_size >= 16 && bits >= 16)) &&
         ((operation != SpvOpConvertFToU && operation != SpvOpConvertFToS) || src[0]->bit_size >= 16) &&
         ((operation != SpvOpConvertSToF && operation != SpvOpConvertUToF) || bits >= 16);
   case SpvOpBitCount:
      return bits == 32 && src[0]->bit_size != 1 && src[0]->num_components == components;
   case SpvOpIsNan: case SpvOpIsInf:
      return bits == 1 && src[0]->bit_size >= 16 && src[0]->num_components == components;
   case SpvOpIEqual: case SpvOpINotEqual: case SpvOpULessThan: case SpvOpSLessThan:
   case SpvOpUGreaterThanEqual: case SpvOpSGreaterThanEqual: case SpvOpUGreaterThan:
   case SpvOpSGreaterThan: case SpvOpULessThanEqual: case SpvOpSLessThanEqual:
   case SpvOpFOrdEqual: case SpvOpFUnordNotEqual: case SpvOpFOrdLessThan: case SpvOpFOrdGreaterThanEqual:
   case SpvOpFOrdGreaterThan: case SpvOpFOrdLessThanEqual: case SpvOpFOrdNotEqual:
   case SpvOpFUnordEqual: case SpvOpFUnordLessThan: case SpvOpFUnordGreaterThan:
   case SpvOpFUnordLessThanEqual: case SpvOpFUnordGreaterThanEqual:
      return bits == 1 && src[0]->bit_size != 1 && src[0]->bit_size == src[1]->bit_size &&
         src[0]->num_components == components && src[1]->num_components == components;
   case SpvOpLogicalNot: case SpvOpLogicalAnd: case SpvOpLogicalOr:
   case SpvOpLogicalEqual: case SpvOpLogicalNotEqual:
      if (bits != 1) return false;
      break;
   case SpvOpQuantizeToF16:
      if (bits != 32) return false;
      break;
   case SpvOpFNegate: case SpvOpFAdd: case SpvOpFSub: case SpvOpFMul:
   case SpvOpFDiv: case SpvOpFRem: case SpvOpVectorTimesScalar: case SpvOpFmaKHR:
      if (bits < 16) return false;
      break;
   case SpvOpCopyObject: break;
   default:
      if (bits == 1) return false;
      break;
   }
   for (unsigned i = 0; i < count; i++) {
      unsigned width = components;
      unsigned size = bits;
      if ((operation == SpvOpBitFieldInsert && i >= 2) ||
          ((operation == SpvOpBitFieldSExtract || operation == SpvOpBitFieldUExtract) && i >= 1)) {
         width = 1;
         size = 32;
      }
      if (operation == SpvOpVectorTimesScalar && i == 1)
         width = 1;
      if (src[i]->num_components != width) return false;
      if (i == 1 && (operation == SpvOpShiftLeftLogical || operation == SpvOpShiftRightLogical ||
                     operation == SpvOpShiftRightArithmetic)) {
         if (src[i]->bit_size == 1) return false;
      } else if (src[i]->bit_size != size) return false;
   }
   return true;
}

nir_def *
switch_nvk_ir_alu(nir_builder *b, SpvOp operation, nir_def **src,
                  unsigned count, unsigned components, unsigned bits,
                  bool exact)
{
   if (!alu_shape(operation, src, count, components, bits))
      return NULL;
   const unsigned saved = b->fp_math_ctrl;
   switch (operation) {
   case SpvOpIsNan: case SpvOpIsInf: case SpvOpFOrdNotEqual: case SpvOpFUnordEqual:
   case SpvOpFUnordLessThan: case SpvOpFUnordGreaterThan:
   case SpvOpFUnordLessThanEqual: case SpvOpFUnordGreaterThanEqual:
      b->fp_math_ctrl |= nir_fp_preserve_nan | nir_fp_preserve_inf;
      break;
   default: break;
   }
   if (exact)
      b->fp_math_ctrl |= nir_fp_exact;
   nir_def *result = build_alu(b, operation, src, count, components, bits, exact);
   b->fp_math_ctrl = saved;
   return result;
}
