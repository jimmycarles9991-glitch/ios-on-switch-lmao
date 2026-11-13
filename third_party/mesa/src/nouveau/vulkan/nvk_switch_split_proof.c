/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the host's proof of a split (nvk_switch_split.h), macOS only. For
 * each sample pixel the whole fragment program is folded to its outputs; the
 * prologue is folded for the same draw, its frontier writes kept, and the split
 * fragment program folded reading them. The outputs (each store_output
 * component and each discard) must be the same bits, at the split and again
 * after nak_postprocess_nir: its late algebraic passes rewrite inexact float
 * operations, and a rewrite that spans the frontier in the whole program
 * cannot happen in the split one.
 *
 * Sample 0's uniform words are the probe's tags (pack/probe.mm: 0xA0000000
 * with the buffer and the word); the others draw words at random, floats in
 * [0, 1), floats in [-8, 8) or any bits. Push words, uniform buffers, set 0
 * and the root table's other words are each a model of their own; the shader
 * data is the program's.
 */

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "nvk_switch_split.h"

#if defined(__APPLE__) && TARGET_OS_OSX

#include "nvk_cmd_buffer.h"
#include "nvk_shader.h"
#include "nvk_switch_split_fold.h"

#include "nak.h"

#include "util/simple_mtx.h"

#include <stdio.h>
#include <stdlib.h>

#define FRONTIER_VA 0x200000000ull
#define RECORDS_VA 0x300000000ull
#define UBO_VA 0x400000000ull
#define UBO_BYTES 4096u
#define ROOT_BYTES 0x800u
#define SET_BYTES 256u
#define MAX_OUTPUTS 256

struct model {
   const struct nvk_cbuf_map *map;
   const nir_shader *fs;
   const struct switch_nvk_split_layout *layout;
   uint64_t sample;
   bool prologue;
   uint8_t frontier[SWITCH_SPLIT_MAX_FRONTIER_BYTES];
};

static uint64_t
mix(uint64_t h, uint64_t v)
{
   h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
   h ^= h >> 31;
   h *= 0xbf58476d1ce4e5b9ull;
   return h ^ (h >> 29);
}

static uint32_t
word(uint64_t sample, uint32_t tag, uint32_t index)
{
   if (sample == 0)
      return 0xa0000000u | (tag & 0xff) << 16 | (index & 0xffff);
   const uint64_t h = mix(mix(sample, tag), index);
   const float unit = (float)(h >> 40) / (float)(1u << 24);
   union { float f; uint32_t u; } v;
   switch (sample % 3) {
   case 1:
      v.f = unit;
      return v.u;
   case 2:
      v.f = unit * 16.0f - 8.0f;
      return v.u;
   default:
      return (uint32_t)h;
   }
}

static int
model_byte(uint32_t w, uint64_t at)
{
   return (w >> (8 * (at % 4))) & 0xff;
}

static uint32_t
cbuf_bytes(const struct model *m, uint32_t cbuf)
{
   switch (m->map->cbufs[cbuf].type) {
   case NVK_CBUF_TYPE_ROOT_DESC:
      return ROOT_BYTES;
   case NVK_CBUF_TYPE_SHADER_DATA:
      return align(m->fs->constant_data_size, 256);
   case NVK_CBUF_TYPE_DESC_SET:
      return SET_BYTES;
   default:
      return UBO_BYTES;
   }
}

static int
cbuf_byte(const struct model *m, uint32_t cbuf, uint64_t at)
{
   if (cbuf >= m->map->cbuf_count)
      return -1;
   if (at >= cbuf_bytes(m, cbuf))
      return 0;
   const uint32_t w = at / 4;
   switch (m->map->cbufs[cbuf].type) {
   case NVK_CBUF_TYPE_ROOT_DESC: {
      const uint32_t push = nvk_root_descriptor_offset(push) / 4;
      const bool pushed = w >= push && w < push + SWITCH_SPLIT_PUSH_BYTES / 4;
      return model_byte(word(m->sample, pushed ? 'P' : 'R', w), at);
   }
   case NVK_CBUF_TYPE_SHADER_DATA:
      return at < m->fs->constant_data_size
                ? ((const uint8_t *)m->fs->constant_data)[at] : 0;
   case NVK_CBUF_TYPE_DESC_SET:
      return model_byte(word(m->sample, 'S', w), at);
   default:
      return model_byte(word(m->sample, 'U' + cbuf, w), at);
   }
}

static int
params_byte(const struct model *m, uint64_t at)
{
   const uint32_t push = nvk_root_descriptor_offset(push) / 4;
   if (at < SWITCH_SPLIT_PUSH_BYTES)
      return model_byte(word(m->sample, 'P', push + at / 4), at);
   if (at >= SWITCH_SPLIT_FRONTIER_ADDRESS && at < SWITCH_SPLIT_FRONTIER_ADDRESS + 8)
      return (FRONTIER_VA >> (8 * (at - SWITCH_SPLIT_FRONTIER_ADDRESS))) & 0xff;
   return 0;
}

static int
prologue_byte(const struct model *m, bool global, uint64_t space, uint64_t at)
{
   const struct switch_nvk_split_layout *l = m->layout;
   if (global) {
      if (at >= RECORDS_VA && at < RECORDS_VA + SWITCH_SPLIT_BATCH_RECORD_BYTES) {
         at -= RECORDS_VA;
         if (at < SWITCH_SPLIT_BATCH_BUFFERS)
            return params_byte(m, at);
         const uint32_t ordinal = (at - SWITCH_SPLIT_BATCH_BUFFERS) / 16;
         const uint32_t byte = (at - SWITCH_SPLIT_BATCH_BUFFERS) % 16;
         if (!ordinal || ordinal > l->ubo_count)
            return 0;
         const uint64_t value = byte < 8 ? UBO_VA + (ordinal - 1) * 0x10000ull : UBO_BYTES;
         return (value >> (8 * (byte % 8))) & 0xff;
      }
      if (at >= UBO_VA && at < UBO_VA + l->ubo_count * 0x10000ull) {
         at -= UBO_VA;
         return cbuf_byte(m, l->ubo_cbuf[at / 0x10000], at % 0x10000);
      }
      return -1;
   }
   if (space == SWITCH_SPLIT_PARAMS_CBUF) {
      if (at < 8)
         return (RECORDS_VA >> (8 * at)) & 0xff;
      if (at < 12)
         return model_byte(1, at - 8);
      return at < 16 ? model_byte(SWITCH_SPLIT_BATCH_RECORD_BYTES, at - 12) : 0;
   }
   if (l->shader_data_cbuf && space == l->shader_data_cbuf) {
      return at < m->fs->constant_data_size
                ? ((const uint8_t *)m->fs->constant_data)[at] : 0;
   }
   return -1;
}

static int
read_byte(void *ctx, bool global, uint64_t space, uint64_t at)
{
   const struct model *m = ctx;
   if (m->prologue)
      return prologue_byte(m, global, space, at);
   if (global)
      return mix(mix(m->sample, 0x61), at) & 0xff;
   if (space == m->layout->frontier_cbuf)
      return at < m->layout->frontier_bytes ? m->frontier[at] : 0;
   return cbuf_byte(m, space, at);
}

static bool
write_byte(void *ctx, uint64_t at, uint8_t byte)
{
   struct model *m = ctx;
   if (!m->prologue || at < FRONTIER_VA ||
       at >= FRONTIER_VA + m->layout->frontier_bytes)
      return false;
   m->frontier[at - FRONTIER_VA] = byte;
   return true;
}

struct outputs {
   unsigned count;
   uint64_t key[MAX_OUTPUTS], bits[MAX_OUTPUTS];
   const char *why;
};

static void
put(struct outputs *o, uint64_t key, uint64_t bits)
{
   for (unsigned i = 0; i < o->count; i++) {
      if (o->key[i] == key) {
         o->bits[i] = bits;
         return;
      }
   }
   if (o->count == MAX_OUTPUTS) {
      o->why = "too many outputs";
      return;
   }
   o->key[o->count] = key;
   o->bits[o->count++] = bits;
}

static void
gather_store(struct outputs *o, nir_intrinsic_instr *store)
{
   if (!nir_src_is_const(store->src[0]) || !nir_src_is_const(store->src[1])) {
      o->why = "an output left unfolded";
      return;
   }
   const nir_io_semantics io = nir_intrinsic_io_semantics(store);
   const uint64_t at = (uint64_t)io.location << 24 | io.dual_source_blend_index << 20 |
                       nir_src_as_uint(store->src[1]) << 8;
   u_foreach_bit(c, nir_intrinsic_write_mask(store))
      put(o, at | (nir_intrinsic_component(store) + c),
          nir_src_comp_as_uint(store->src[0], c));
}

struct effect {
   uint64_t h;
   bool folded;
};

static bool
hash_effect_src(nir_src *src, void *data)
{
   struct effect *e = data;
   e->folded &= nir_src_is_const(*src);
   for (unsigned c = 0; e->folded && c < src->ssa->num_components; c++)
      e->h = mix(e->h, nir_src_comp_as_uint(*src, c));
   return true;
}

/* Any other intrinsic without a result (an image store, a barrier, the
 * interlock) is an output named by its operation and its operands.
 */
static void
gather_effect(struct outputs *o, nir_intrinsic_instr *intrin)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intrin->intrinsic];
   if (info->has_dest) {
      o->why = "an intrinsic left unfolded";
      return;
   }
   struct effect e = {.h = mix(2, intrin->intrinsic), .folded = true};
   for (unsigned i = 0; i < info->num_indices; i++)
      e.h = mix(e.h, intrin->const_index[i]);
   nir_foreach_src(&intrin->instr, hash_effect_src, &e);
   if (!e.folded)
      o->why = "an effect left unfolded";
   else
      put(o, 2ull << 40 | (e.h & 0xffffffffffull), 1);
}

static void
gather(nir_shader *s, struct outputs *o)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   if (exec_list_length(&impl->body) != 1) {
      o->why = "control flow left unfolded";
      return;
   }
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_load_const)
            continue;
         if (instr->type != nir_instr_type_intrinsic) {
            o->why = "an instruction left unfolded";
            return;
         }
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_store_output:
            gather_store(o, intrin);
            break;
         case nir_intrinsic_terminate_if:
         case nir_intrinsic_demote_if:
            put(o, 1ull << 40 | intrin->intrinsic,
                nir_src_comp_as_uint(intrin->src[0], 0));
            break;
         case nir_intrinsic_terminate:
         case nir_intrinsic_demote:
            put(o, 1ull << 40 | intrin->intrinsic, 1);
            break;
         default:
            gather_effect(o, intrin);
            break;
         }
      }
   }
}

static void
fold_outputs(const nir_shader *s, struct model *m, struct outputs *o)
{
   memset(o, 0, sizeof(*o));
   nir_shader *copy = nir_shader_clone(NULL, s);
   const struct switch_fold_model fm = {
      .ctx = m, .sample = m->sample, .read = read_byte, .write = write_byte,
   };
   if (!switch_fold(copy, &fm))
      o->why = m->prologue ? "the prologue left the model" : "a read left the model";
   else if (!m->prologue)
      gather(copy, o);
   ralloc_free(copy);
}

static const char *
compare(const struct outputs *a, const struct outputs *b, char *why, size_t n)
{
   if (a->why || b->why)
      return a->why ? a->why : b->why;
   if (a->count != b->count) {
      snprintf(why, n, "%u outputs against %u", a->count, b->count);
      return why;
   }
   for (unsigned i = 0; i < a->count; i++) {
      unsigned j = 0;
      while (j < b->count && b->key[j] != a->key[i])
         j++;
      if (j == b->count || b->bits[j] != a->bits[i]) {
         snprintf(why, n, "output %llx: %llx against %llx",
                  (unsigned long long)a->key[i], (unsigned long long)a->bits[i],
                  j == b->count ? 0ull : (unsigned long long)b->bits[j]);
         return why;
      }
   }
   return NULL;
}

static struct {
   simple_mtx_t lock;
   uint32_t states, identical, differ, inconclusive;
   bool registered;
} totals = {.lock = SIMPLE_MTX_INITIALIZER};

static void
report(void)
{
   fprintf(stderr,
           "switch split proof: %u states, %u bit-identical, %u differ, "
           "%u not folded\n",
           totals.states, totals.identical, totals.differ, totals.inconclusive);
}

struct forms {
   const nir_shader *whole, *prologue, *fs;
};

/* NULL where every sample's outputs are the same bits, else why not (`why`
 * where they differ) and the sample in `at`
 */
static const char *
prove_forms(const struct forms *f, struct model *m, struct outputs *whole,
            struct outputs *part, unsigned samples, char *why, size_t n,
            unsigned *at)
{
   for (unsigned s = 0; s < samples; s++) {
      *at = s;
      m->sample = s;
      m->prologue = false;
      fold_outputs(f->whole, m, whole);
      memset(m->frontier, 0, sizeof(m->frontier));
      m->prologue = true;
      fold_outputs(f->prologue, m, part);
      m->prologue = false;
      if (part->why)
         return part->why;
      fold_outputs(f->fs, m, part);
      const char *failed = compare(whole, part, why, n);
      if (failed)
         return failed;
   }
   return NULL;
}

static nir_shader *
postprocessed(const nir_shader *s, const struct nak_compiler *nak,
              nir_variable_mode robust2_modes, const struct nak_fs_key *fs_key)
{
   nir_shader *copy = nir_shader_clone(NULL, s);
   nak_postprocess_nir(copy, nak, robust2_modes, fs_key, false);
   return copy;
}

void
switch_nvk_split_prove(const nir_shader *fs, const struct nvk_cbuf_map *map,
                       const struct switch_nvk_split_nir *split,
                       const struct switch_nvk_split_proof *proof)
{
   struct model *m = calloc(1, sizeof(*m));
   struct outputs *whole = calloc(1, sizeof(*whole));
   struct outputs *part = calloc(1, sizeof(*part));
   if (m == NULL || whole == NULL || part == NULL) {
      free(m);
      free(whole);
      free(part);
      return;
   }
   m->map = map;
   m->fs = fs;
   m->layout = &split->layout;

   char why[160];
   unsigned at = 0;
   const struct forms split_point = {fs, split->prologue, split->fs};
   const char *failed = prove_forms(&split_point, m, whole, part,
                                    proof->samples, why, sizeof(why), &at);
   const char *stage = "at the split";
   if (failed == NULL) {
      const struct forms nak = {
         postprocessed(fs, proof->nak, proof->robust2_modes, proof->fs_key),
         postprocessed(split->prologue, proof->nak, 0, NULL),
         postprocessed(split->fs, proof->nak, proof->robust2_modes,
                       proof->fs_key),
      };
      failed = prove_forms(&nak, m, whole, part, proof->samples, why,
                           sizeof(why), &at);
      stage = "after NAK's NIR passes";
      ralloc_free((nir_shader *)nak.whole);
      ralloc_free((nir_shader *)nak.prologue);
      ralloc_free((nir_shader *)nak.fs);
   }

   const bool differs = failed != NULL && failed == why;
   simple_mtx_lock(&totals.lock);
   totals.states++;
   if (failed == NULL)
      totals.identical++;
   else if (differs)
      totals.differ++;
   else
      totals.inconclusive++;
   if (!totals.registered) {
      atexit(report);
      totals.registered = true;
   }
   fprintf(stderr,
           "switch split proof %016llx: %u values in %u bytes, %u "
           "instructions moved: ",
           (unsigned long long)proof->whole_id, split->layout.frontier_values,
           split->layout.frontier_bytes, split->layout.prologue_instrs);
   if (failed == NULL)
      fprintf(stderr,
              "%u outputs bit-identical in %u samples, at the split and after "
              "NAK's NIR passes\n",
              whole->count, proof->samples);
   else
      fprintf(stderr, "%s %s at sample %u: %s\n",
              differs ? "DIFFER" : "not folded", stage, at, failed);
   simple_mtx_unlock(&totals.lock);
   free(m);
   free(whole);
   free(part);
}

#else

void
switch_nvk_split_prove(const nir_shader *fs, const struct nvk_cbuf_map *map,
                       const struct switch_nvk_split_nir *split,
                       const struct switch_nvk_split_proof *proof)
{
}

#endif
