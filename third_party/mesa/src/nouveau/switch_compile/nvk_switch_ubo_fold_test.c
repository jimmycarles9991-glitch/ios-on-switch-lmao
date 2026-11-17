/* SPDX-License-Identifier: MIT */
/* nvk_switch_ubo_fold.c over a hand-built fragment program as NVK's lowering
 * leaves it: reads of the folded buffer's constant buffer whose observed bits
 * are held become their value (u8, u16, u32, u64, vector components, an offset of
 * two immediates or of a held byte, ldc_nv with a base), unknown bytes stay
 * run-time values, as do reads of other constant buffers and at a
 * run-time offset. Mask 0 or an unmapped buffer changes nothing, and a second
 * fold is a no-op.
 */
#include "nvk_switch_ubo_fold.h"

#include "nvk_shader.h"

#include "nir_builder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SINK 0x1000
#define DYNAMIC_OFFSET 0x2000
#define FOLDED_DESC 0x40
#define LOAD UINT64_MAX

enum { ROOT, FOLDED, OTHER_DESC, OTHER_SET };

static const nir_shader_compiler_options options = {0};
static int failures;

static void
check(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

static struct nvk_cbuf_map
cbuf_map(void)
{
   struct nvk_cbuf_map map = {.cbuf_count = 4};
   map.cbufs[ROOT] = (struct nvk_cbuf){.type = NVK_CBUF_TYPE_ROOT_DESC};
   map.cbufs[FOLDED] = (struct nvk_cbuf){
      .type = NVK_CBUF_TYPE_UBO_DESC, .desc_set = 0, .desc_offset = FOLDED_DESC};
   map.cbufs[OTHER_DESC] = (struct nvk_cbuf){
      .type = NVK_CBUF_TYPE_UBO_DESC, .desc_set = 0, .desc_offset = 0x80};
   map.cbufs[OTHER_SET] = (struct nvk_cbuf){
      .type = NVK_CBUF_TYPE_UBO_DESC, .desc_set = 1, .desc_offset = FOLDED_DESC};
   return map;
}

/* Bytes 0..13, 26..27 and 60..63 held, byte i as 0x40 + i */
static struct switch_nvk_ubo_fold
fold_of(uint64_t mask)
{
   struct switch_nvk_ubo_fold fold = {.desc = FOLDED_DESC, .mask = mask};
   for (unsigned i = 0; i < 64; i++)
      fold.bytes[i] = 0x40 + i;
   return fold;
}

#define HELD_MASK (0x3fffull | 0x3ull << 26 | 0xfull << 60)

struct probe {
   const char *what;
   unsigned cbuf, components, bit_size, offset;
   bool ldc, dynamic, split_offset, chained;
   unsigned store_mask; /* the components the program stores, 0: all */
   uint64_t want[4];
};

static const struct probe probes[] = {
   {"u8 at 5", FOLDED, 1, 8, 5, .want = {0x45}},
   {"u32 at 8", FOLDED, 1, 32, 8, .want = {0x4b4a4948}},
   {"vec4 u32 at 0, its last straddling bytes 13 and 14", FOLDED, 4, 32, 0,
    .want = {0x43424140, 0x47464544, 0x4b4a4948, LOAD}},
   {"u16 at 26", FOLDED, 1, 16, 26, .want = {0x5b5a}},
   {"u16 at 24, not held", FOLDED, 1, 16, 24, .want = {LOAD}},
   {"u64 at 0", FOLDED, 1, 64, 0, .want = {0x4746454443424140ull}},
   {"vec2 u32 at 60, its second past byte 63", FOLDED, 2, 32, 60,
    .want = {0x7f7e7d7c, LOAD}},
   {"u8 at 2 + 2", FOLDED, 1, 8, 4, .split_offset = true, .want = {0x44}},
   {"u32 at held byte 1 (0x41) - 0x39", FOLDED, 1, 32, 8, .chained = true,
    .want = {0x4b4a4948}},
   {"ldc_nv u32 at base 4 + 4", FOLDED, 1, 32, 8, .ldc = true,
    .want = {0x4b4a4948}},
   {"vec4 u8 at 12", FOLDED, 4, 8, 12, .want = {0x4c, 0x4d, LOAD, LOAD}},
   {"vec4 u32 at 0 read at .x alone", FOLDED, 4, 32, 0, .store_mask = 0x1,
    .want = {0x43424140}},
   {"vec4 u32 at 4 read at .xz, .y held and .z not", FOLDED, 4, 32, 4,
    .store_mask = 0x5, .want = {0x47464544, LOAD}},
   {"another descriptor's buffer", OTHER_DESC, 1, 32, 0, .want = {LOAD}},
   {"another set's buffer at the same offset", OTHER_SET, 1, 32, 0,
    .want = {LOAD}},
   {"the root table", ROOT, 1, 32, 0, .want = {LOAD}},
   {"a run-time offset", FOLDED, 1, 32, 0, .dynamic = true, .want = {LOAD}},
};

#define PROBES (sizeof(probes) / sizeof(probes[0]))

/* The cbuf index and offset as nir_lower_explicit_io leaves them: channels of
 * the address vec2 NVK's descriptor lowering starts from
 */
static nir_def *
read_of(nir_builder *b, const struct probe *p)
{
   nir_def *addr = nir_imm_ivec2(b, p->cbuf, p->ldc ? 4 : p->offset);
   nir_def *index = nir_channel(b, addr, 0), *offset = nir_channel(b, addr, 1);
   if (p->split_offset)
      offset = nir_iadd(b, nir_imm_int(b, p->offset / 2), nir_imm_int(b, p->offset / 2));
   if (p->chained) {
      nir_def *byte = nir_load_ubo(b, 1, 8, index, nir_imm_int(b, 1), .align_mul = 1, .range = ~0u);
      offset = nir_iadd_imm(b, nir_u2u32(b, byte), -0x39);
   }
   if (p->dynamic)
      offset = nir_load_global(b, 1, 32, nir_imm_int64(b, DYNAMIC_OFFSET), .align_mul = 4);
   const unsigned align = p->bit_size / 8;
   if (p->ldc)
      return nir_ldc_nv(b, p->components, p->bit_size, index, offset, .base = p->offset - 4,
                        .align_mul = align);
   return nir_load_ubo(b, p->components, p->bit_size, index, offset, .align_mul = align, .range = ~0u);
}

static nir_shader *
program(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "ubo-fold");
   for (unsigned k = 0; k < PROBES; k++) {
      nir_def *value = read_of(&b, &probes[k]);
      if (probes[k].store_mask)
         value = nir_channels(&b, value, probes[k].store_mask);
      nir_store_global(&b, value, nir_imm_int64(&b, SINK + 32 * k), .align_mul = probes[k].bit_size / 8);
   }
   return b.shader;
}

static bool
is_read(nir_scalar s)
{
   if (!nir_scalar_is_intrinsic(s))
      return false;
   const nir_intrinsic_op op = nir_scalar_intrinsic_op(s);
   return op == nir_intrinsic_load_ubo || op == nir_intrinsic_ldc_nv;
}

static unsigned
reads_in(nir_shader *s)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            n += op == nir_intrinsic_load_ubo || op == nir_intrinsic_ldc_nv;
         }
      }
   }
   return n;
}

static nir_intrinsic_instr *
store_of(nir_shader *s, unsigned k)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *store = nir_instr_as_intrinsic(instr);
            if (store->intrinsic == nir_intrinsic_store_global && nir_src_is_const(store->src[1]) &&
                nir_src_as_uint(store->src[1]) == SINK + 32 * k)
               return store;
         }
      }
   }
   return NULL;
}

static void
check_probe(nir_shader *s, unsigned k)
{
   const struct probe *p = &probes[k];
   nir_intrinsic_instr *store = store_of(s, k);
   check(store != NULL, p->what);
   if (store == NULL)
      return;
   const unsigned count = p->store_mask ? util_bitcount(p->store_mask) : p->components;
   for (unsigned c = 0; c < count; c++) {
      const nir_scalar v = nir_scalar_resolved(store->src[0].ssa, c);
      char what[160];
      if (p->want[c] == LOAD) {
         snprintf(what, sizeof(what), "%s: component %u stays a read", p->what, c);
         check(is_read(v), what);
      } else if (nir_scalar_is_const(v)) {
         snprintf(what, sizeof(what), "%s: component %u is 0x%" PRIx64 ", want 0x%" PRIx64, p->what, c,
                  nir_scalar_as_uint(v), p->want[c]);
         check(nir_scalar_as_uint(v) == p->want[c], what);
      } else {
         snprintf(what, sizeof(what), "%s: component %u is still read, want 0x%" PRIx64, p->what, c, p->want[c]);
         check(false, what);
      }
   }
}

static void
test_folds(void)
{
   const struct nvk_cbuf_map map = cbuf_map();
   const struct switch_nvk_ubo_fold fold = fold_of(HELD_MASK);
   nir_shader *s = program();
   check(reads_in(s) == PROBES + 1, "the program reads each probe once, the chained offset's byte too");
   check(switch_nvk_fold_ubo(s, &map, &fold), "the fold makes progress");
   nir_validate_shader(s, "after the fold");
   for (unsigned k = 0; k < PROBES; k++)
      check_probe(s, k);
   char what[96];
   snprintf(what, sizeof(what), "nine reads stay, %u do", reads_in(s));
   check(reads_in(s) == 9, what);

   char *once = nir_shader_as_str(s, NULL);
   check(!switch_nvk_fold_ubo(s, &map, &fold), "a second fold makes no progress");
   char *twice = nir_shader_as_str(s, NULL);
   check(strcmp(once, twice) == 0, "a second fold leaves the program as it was");
   ralloc_free(once);
   ralloc_free(twice);
   ralloc_free(s);
}

static void
check_untouched(const struct nvk_cbuf_map *map, const struct switch_nvk_ubo_fold *fold, const char *what)
{
   nir_shader *s = program();
   char *before = nir_shader_as_str(s, NULL);
   check(!switch_nvk_fold_ubo(s, map, fold), what);
   char *after = nir_shader_as_str(s, NULL);
   check(strcmp(before, after) == 0, what);
   ralloc_free(before);
   ralloc_free(after);
   ralloc_free(s);
}

static void
optimize(nir_shader *s)
{
   bool progress;
   do {
      progress = nir_opt_constant_folding(s);
      progress |= nir_opt_algebraic(s);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_dce(s);
      progress |= nir_opt_dead_cf(s);
   } while (progress);
}

static void
test_partial_words(unsigned bit_size, bool ldc, unsigned variant, bool observe_padding)
{
   const unsigned bytes = bit_size / 8;
   const uint64_t known_mask = BITFIELD64_MASK(bit_size - 8);
   const struct nvk_cbuf_map map = cbuf_map();
   struct switch_nvk_ubo_fold fold = fold_of(BITFIELD64_MASK(bytes - 1));
   fold.bytes[0] ^= variant;
   uint64_t known = 0;
   for (unsigned i = 0; i < bytes - 1; i++)
      known |= (uint64_t)fold.bytes[i] << (8 * i);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "partial-word");
   nir_def *index = nir_imm_int(&b, FOLDED), *zero = nir_imm_int(&b, 0);
   nir_def *value = ldc ? nir_ldc_nv(&b, 1, bit_size, index, zero, .align_mul = bytes)
                        : nir_load_ubo(&b, 1, bit_size, index, zero, .align_mul = bytes, .range = ~0u);
   if (observe_padding) {
      nir_store_global(&b, value, nir_imm_int64(&b, SINK), .align_mul = bytes);
      nir_store_global(&b, nir_ushr_imm(&b, value, bit_size - 8), nir_imm_int64(&b, SINK + 64), .align_mul = bytes);
   }
   nir_store_global(&b, nir_iand_imm(&b, value, known_mask), nir_imm_int64(&b, SINK + 32), .align_mul = bytes);
   check(switch_nvk_fold_ubo(b.shader, &map, &fold) == !observe_padding,
         "a partial word folds only when its unknown bits are unobserved");
   nir_validate_shader(b.shader, "after partial-word folding");
   check(!switch_nvk_fold_ubo(b.shader, &map, &fold), "refolding a partial word makes no progress");
   optimize(b.shader);
   check(reads_in(b.shader) == observe_padding, "observed padding keeps its original load");
   if (!observe_padding) {
      nir_scalar held = nir_scalar_resolved(store_of(b.shader, 1)->src[0].ssa, 0);
      check(nir_scalar_is_const(held) && nir_scalar_as_uint(held) == known,
            "a consumer of only held bits becomes a constant");
      ralloc_free(b.shader);
      return;
   }

   const uint64_t runtime[] = {0x1122334455667788ull, 0xaabbccddeeff0011ull};
   for (unsigned i = 0; i < ARRAY_SIZE(runtime); i++) {
      nir_shader *copy = nir_shader_clone(NULL, b.shader);
      nir_function_impl *impl = nir_shader_get_entrypoint(copy);
      nir_builder replace = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
            if (load->intrinsic != nir_intrinsic_load_ubo && load->intrinsic != nir_intrinsic_ldc_nv)
               continue;
            replace.cursor = nir_before_instr(instr);
            const uint64_t input = (runtime[i] & (BITFIELD64_MASK(bit_size) ^ known_mask)) | known;
            nir_def_replace(&load->def, nir_imm_intN_t(&replace, input, bit_size));
         }
      }
      optimize(copy);
      const uint64_t expected[] = {
         (runtime[i] & (BITFIELD64_MASK(bit_size) ^ known_mask)) | known,
         known,
         runtime[i] >> (bit_size - 8) & 0xff,
      };
      for (unsigned k = 0; k < ARRAY_SIZE(expected); k++) {
         nir_scalar result = nir_scalar_resolved(store_of(copy, k)->src[0].ssa, 0);
         check(nir_scalar_is_const(result) && nir_scalar_as_uint(result) == expected[k],
               "held bytes stay fixed while padding remains the input's value");
      }
      nir_validate_shader(copy, "after partial-word input substitution");
      ralloc_free(copy);
   }
   ralloc_free(b.shader);
}

static void
test_no_ops(void)
{
   const struct nvk_cbuf_map map = cbuf_map();
   const struct switch_nvk_ubo_fold none = fold_of(0);
   check_untouched(&map, &none, "mask 0 changes nothing");
   check_untouched(&map, NULL, "no fold changes nothing");
   const struct switch_nvk_ubo_fold held = fold_of(HELD_MASK);
   check_untouched(NULL, &held, "no constant buffer map changes nothing");
   struct switch_nvk_ubo_fold unmapped = held;
   unmapped.desc = 0xc0;
   check_untouched(&map, &unmapped, "a buffer the map does not bind changes nothing");
}

static void
test_select_values(nir_shader *s, bool unknown)
{
   const uint32_t inputs[] = {0, 1, 2, 4, UINT32_MAX};
   const uint32_t leaves[] = {0, 1, 6, 7, 23, UINT32_MAX};
   for (unsigned i = 0; i < ARRAY_SIZE(inputs); i++) {
      for (unsigned j = 0; j < (unknown ? ARRAY_SIZE(leaves) : 1); j++) {
         nir_shader *copy = nir_shader_clone(NULL, s);
         nir_function_impl *impl = nir_shader_get_entrypoint(copy);
         nir_builder b = nir_builder_create(impl);
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
               if (load->intrinsic != nir_intrinsic_load_global)
                  continue;
               const uint64_t at = nir_src_as_uint(load->src[0]);
               check(at == DYNAMIC_OFFSET || at == DYNAMIC_OFFSET + 4,
                     "only the index and unknown leaf are runtime inputs");
               b.cursor = nir_before_instr(instr);
               nir_def_replace(&load->def, nir_imm_int(&b, at == DYNAMIC_OFFSET ? inputs[i] : leaves[j]));
            }
         }
         optimize(copy);
         const uint32_t selected = inputs[i] == 0 ? 1 : inputs[i] == 1 ? (unknown ? leaves[j] : 6) : 0;
         for (unsigned k = 0; k < 24; k++) {
            for (unsigned unequal = 0; unequal < 2; unequal++) {
               nir_intrinsic_instr *store = store_of(copy, 2 * k + unequal);
               check(store != NULL && nir_src_is_const(store->src[0]) &&
                     nir_src_as_uint(store->src[0]) == ((selected == k) ^ unequal),
                     "equality and inequality preserve the selected leaf's truth");
            }
            check((store_of(copy, 48 + k) != NULL) == (selected == k),
                  "only the selected opcode arm writes its result");
         }
         nir_validate_shader(copy, "after constant-select input substitution");
         ralloc_free(copy);
      }
   }
}

static void
test_select_comparisons(bool mirrored, bool unknown)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "selected-opcodes");
   nir_def *index = nir_load_global(&b, 1, 32, nir_imm_int64(&b, DYNAMIC_OFFSET), .align_mul = 4);
   nir_def *zero = nir_imm_int(&b, 0), *bank = nir_imm_int(&b, FOLDED);
   nir_def *first = nir_load_ubo(&b, 1, 32, bank, zero, .align_mul = 4, .range = ~0u);
   nir_def *second = unknown
      ? nir_load_global(&b, 1, 32, nir_imm_int64(&b, DYNAMIC_OFFSET + 4), .align_mul = 4)
      : nir_load_ubo(&b, 1, 32, bank, nir_imm_int(&b, 4), .align_mul = 4, .range = ~0u);
   nir_def *tail = mirrored ? nir_bcsel(&b, nir_ine_imm(&b, index, 1), zero, second)
                            : nir_bcsel(&b, nir_ieq_imm(&b, index, 1), second, zero);
   nir_def *selected = mirrored ? nir_bcsel(&b, nir_ine_imm(&b, index, 0), tail, first)
                                : nir_bcsel(&b, nir_ieq_imm(&b, index, 0), first, tail);
   for (unsigned k = 0; k < 24; k++) {
      nir_def *equal = nir_ieq_imm(&b, selected, k);
      nir_def *unequal = nir_ine_imm(&b, selected, k);
      nir_store_global(&b, nir_b2i32(&b, equal), nir_imm_int64(&b, SINK + 32 * (2 * k)), .align_mul = 4);
      nir_store_global(&b, nir_b2i32(&b, unequal), nir_imm_int64(&b, SINK + 32 * (2 * k + 1)), .align_mul = 4);
      nir_push_if(&b, equal);
      nir_store_global(&b, nir_imm_int(&b, k), nir_imm_int64(&b, SINK + 32 * (48 + k)), .align_mul = 4);
      nir_pop_if(&b, NULL);
   }

   nir_shader *generic = nir_shader_clone(NULL, b.shader);
   optimize(generic);
   check(reads_in(generic) == (unknown ? 1 : 2), "the generic path keeps each unheld UBO load");
   for (unsigned k = 0; k < 24; k++)
      check(store_of(generic, 48 + k) != NULL, "every opcode arm remains possible without held selector bytes");
   nir_validate_shader(generic, "generic select comparisons");
   ralloc_free(generic);

   const struct nvk_cbuf_map map = cbuf_map();
   const struct switch_nvk_ubo_fold fold = {.desc = FOLDED_DESC, .mask = 0xff, .bytes = {1, 0, 0, 0, 6}};
   check(switch_nvk_fold_ubo(b.shader, &map, &fold), "the opcode leaves fold from the held UBO");
   optimize(b.shader);
   for (unsigned k = 0; k < 24; k++) {
      const bool reachable = unknown || k == 0 || k == 1 || k == 6;
      check((store_of(b.shader, 48 + k) != NULL) == reachable,
            "impossible opcode arms disappear; an unknown leaf keeps every opcode reachable");
   }
   nir_validate_shader(b.shader, "selected comparison arms");
   test_select_values(b.shader, unknown);
   ralloc_free(b.shader);
}

static void
test_ieee_classifications(bool ftz, bool fast)
{
   const uint32_t cases[] = {0, 1, 0x007fffff, 0x00800000, 0x00800001, 0x3f800000, 0x7f7fffff,
                            0x7f800000, 0x7f800001, 0x7fbfffff, 0x7fc00000, 0x7fc12345, 0x7fffffff};
   const nir_shader_compiler_options classification_options = {.has_ford_funord = true};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &classification_options, "ieee-classification");
   b.fp_math_ctrl = fast ? nir_fp_fast_math : nir_fp_no_fast_math;
   b.shader->info.float_controls_execution_mode =
      ftz ? FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32 : FLOAT_CONTROLS_DENORM_PRESERVE_FP32;
   nir_def *a = nir_load_global(&b, 1, 32, nir_imm_int64(&b, DYNAMIC_OFFSET), .align_mul = 4);
   nir_def *other = nir_load_global(&b, 1, 32, nir_imm_int64(&b, DYNAMIC_OFFSET + 4), .align_mul = 4);
   nir_def *abs_a = nir_iand_imm(&b, a, 0x7fffffff), *abs_b = nir_iand_imm(&b, other, 0x7fffffff);
   nir_def *predicates[] = {nir_ieq_imm(&b, abs_a, 0x7f800000), nir_ult_imm(&b, abs_a, 0x00800000),
                            nir_uge(&b, nir_imm_int(&b, 0x7f800000), nir_umax(&b, abs_a, abs_b))};
   for (unsigned k = 0; k < ARRAY_SIZE(predicates); k++) {
      nir_store_global(&b, nir_b2i32(&b, predicates[k]), nir_imm_int64(&b, SINK + 32 * k), .align_mul = 4);
      nir_push_if(&b, predicates[k]);
      nir_store_global(&b, nir_imm_int(&b, k), nir_imm_int64(&b, SINK + 32 * (k + 3)), .align_mul = 4);
      nir_pop_if(&b, NULL);
   }
   optimize(b.shader);
   nir_opt_algebraic_late(b.shader);
   nir_opt_dce(b.shader);
   unsigned integer_classifications = 0;
   bool ordered_pair = false;
   nir_foreach_block(block, nir_shader_get_entrypoint(b.shader)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         const nir_alu_instr *alu = nir_instr_as_alu(instr);
         integer_classifications += alu->def.bit_size == 32 && (alu->op == nir_op_iand || alu->op == nir_op_umax);
         ordered_pair |= alu->op == nir_op_ford;
         if (alu->op == nir_op_feq || alu->op == nir_op_flt || alu->op == nir_op_fabs || alu->op == nir_op_ford)
            check((alu->fp_math_ctrl & (nir_fp_preserve_nan | nir_fp_preserve_inf)) ==
                     (nir_fp_preserve_nan | nir_fp_preserve_inf), "classification keeps ordered NaN and infinity semantics");
      }
   }
   check(!integer_classifications, "unused integer classification masks and maximum disappear");
   check(ordered_pair, "the two non-NaN tests combine as the backend's ordered comparison");
   for (unsigned i = 0; i < 2 * ARRAY_SIZE(cases); i++) {
      for (unsigned j = 0; j < 2 * ARRAY_SIZE(cases); j++) {
         const uint32_t inputs[] = {cases[i % ARRAY_SIZE(cases)] | (i / ARRAY_SIZE(cases)) << 31,
                                    cases[j % ARRAY_SIZE(cases)] | (j / ARRAY_SIZE(cases)) << 31};
         const uint32_t x = inputs[0] & 0x7fffffff, y = inputs[1] & 0x7fffffff;
         const bool expected[] = {x == 0x7f800000, x < 0x00800000, MAX2(x, y) <= 0x7f800000};
         nir_shader *copy = nir_shader_clone(NULL, b.shader);
         nir_function_impl *impl = nir_shader_get_entrypoint(copy);
         nir_builder replace = nir_builder_create(impl);
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
               if (load->intrinsic != nir_intrinsic_load_global)
                  continue;
               replace.cursor = nir_before_instr(instr);
               const unsigned index = nir_src_as_uint(load->src[0]) == DYNAMIC_OFFSET ? 0 : 1;
               nir_def_replace(&load->def, nir_imm_intN_t(&replace, inputs[index], 32));
            }
         }
         optimize(copy);
         for (unsigned k = 0; k < ARRAY_SIZE(expected); k++) {
            nir_intrinsic_instr *store = store_of(copy, k);
            check(store && nir_src_is_const(store->src[0]) && nir_src_as_uint(store->src[0]) == expected[k],
                  "IEEE classification truth matches the integer bit reference");
            check((store_of(copy, k + 3) != NULL) == expected[k], "only the true classification body writes");
         }
         nir_validate_shader(copy, "after IEEE classification input substitution");
         ralloc_free(copy);
      }
   }
   nir_validate_shader(b.shader, "IEEE classification");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   test_folds();
   for (unsigned bits = 16; bits <= 64; bits *= 2)
      for (unsigned ldc = 0; ldc < 2; ldc++)
         for (unsigned variant = 0; variant < 2; variant++)
            for (unsigned padding = 0; padding < 2; padding++)
               test_partial_words(bits, ldc, variant, padding);
   test_no_ops();
   for (unsigned mirrored = 0; mirrored < 2; mirrored++)
      for (unsigned unknown = 0; unknown < 2; unknown++)
         test_select_comparisons(mirrored, unknown);
   for (unsigned ftz = 0; ftz < 2; ftz++)
      for (unsigned fast = 0; fast < 2; fast++)
         test_ieee_classifications(ftz, fast);
   glsl_type_singleton_decref();
   if (failures)
      return 1;
   puts("held reads of the folded buffer are constants, every other read stays, and refolding is a no-op");
   return 0;
}
