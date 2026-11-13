/* SPDX-License-Identifier: MIT */
#ifndef NVK_SWITCH_SPLIT_LOAD_H
#define NVK_SWITCH_SPLIT_LOAD_H

#include "nir_builder.h"

static inline nir_def *
switch_split_load(nir_builder *b, nir_def *base, nir_def *size, nir_def *offset,
                  unsigned components, unsigned bits, unsigned alignment)
{
   nir_def *values[NIR_MAX_VEC_COMPONENTS];
   const unsigned bytes = bits / 8;
   nir_if *vector_full = NULL;
   nir_def *vector_value = NULL;
   if (components > 1) {
      nir_def *end = nir_iadd_imm(b, offset, components * bytes);
      vector_full = nir_push_if(b, nir_uge(b, nir_u2u64(b, size), end));
      vector_value = nir_load_global(b, components, bits, nir_iadd(b, base, offset), .align_mul = alignment);
      nir_push_else(b, vector_full);
   }
   for (unsigned c = 0; c < components; ++c) {
      nir_def *at = nir_iadd_imm(b, offset, c * bytes);
      nir_def *end = nir_iadd_imm(b, at, bytes);
      nir_def *address = nir_iadd(b, base, at);
      nir_if *whole = nir_push_if(b, nir_uge(b, nir_u2u64(b, size), end));
      nir_def *loaded = nir_load_global(b, 1, bits, address,
                                        .align_mul = alignment, .align_offset = (c * bytes) % alignment);
      nir_push_else(b, whole);
      nir_def *partial = nir_imm_intN_t(b, 0, bits);
      for (unsigned byte = 0; byte < bytes; ++byte) {
         nir_def *index = nir_iadd_imm(b, at, byte);
         nir_if *valid = nir_push_if(b, nir_ult(b, index, nir_u2u64(b, size)));
         nir_def *one = nir_load_global(b, 1, 8, nir_iadd_imm(b, address, byte), .align_mul = 1);
         nir_push_else(b, valid);
         nir_def *zero = nir_imm_intN_t(b, 0, 8);
         nir_pop_if(b, valid);
         one = nir_u2uN(b, nir_if_phi(b, one, zero), bits);
         partial = nir_ior(b, partial, nir_ishl_imm(b, one, 8 * byte));
      }
      nir_pop_if(b, whole);
      values[c] = nir_if_phi(b, loaded, partial);
   }
   nir_def *value = nir_vec(b, values, components);
   if (vector_full) {
      nir_pop_if(b, vector_full);
      value = nir_if_phi(b, vector_value, value);
   }
   return value;
}

#endif
