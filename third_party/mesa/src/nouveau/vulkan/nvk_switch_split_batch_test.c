/* SPDX-License-Identifier: MIT */
#include "nvk_switch_split_private.h"
#include "nvk_switch_split_fold.h"
#include "nir_builder.h"
#include "nak_private.h"
#include <stdio.h>
#include <stdlib.h>

#define RECORDS 33
#define RECORD_VA 0x200000000ull
#define SOURCE_VA 0x300000000ull
#define FRONTIER_VA 0x400000000ull

struct model {
   uint8_t records[RECORDS][SWITCH_SPLIT_BATCH_RECORD_BYTES];
   uint8_t input[RECORDS][64], output[RECORDS][32];
   uint8_t header[16];
   uint32_t size[RECORDS], offset[RECORDS];
   unsigned reads, writes;
};

static void
put(void *at, uint64_t value, unsigned bytes)
{
   for (unsigned i = 0; i < bytes; ++i)
      ((uint8_t *)at)[i] = value >> (8 * i);
}

static int
read_byte(void *ctx, bool global, uint64_t cbuf, uint64_t at)
{
   struct model *m = ctx;
   ++m->reads;
   if (!global) {
      if (cbuf == 0 && at < sizeof(m->header))
         return m->header[at];
      if (cbuf == 1 && at < 256)
         return (at * 13 + 7) & 0xff;
      return -1;
   }
   if (at >= RECORD_VA && at < RECORD_VA + sizeof(m->records))
      return ((uint8_t *)m->records)[at - RECORD_VA];
   if (at >= SOURCE_VA && at < SOURCE_VA + RECORDS * 0x1000ull) {
      at -= SOURCE_VA;
      unsigned row = at / 0x1000, byte = at % 0x1000;
      return byte < m->size[row] ? m->input[row][byte] : -1;
   }
   return -1;
}

static bool
write_byte(void *ctx, uint64_t at, uint8_t byte)
{
   struct model *m = ctx;
   if (at < FRONTIER_VA || at >= FRONTIER_VA + RECORDS * 256ull)
      return false;
   at -= FRONTIER_VA;
   if (at % 256 >= 32)
      return false;
   m->output[at / 256][at % 256] = byte;
   ++m->writes;
   return true;
}

static nir_shader *
program(struct switch_nvk_split_layout *layout, bool aligned)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "batch-source");
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *offset = nir_ldc_nv(&b, 1, 32, zero, zero, .base = PUSH_START, .align_mul = 4);
   if (aligned)
      offset = nir_iand_imm(&b, offset, ~15u);
   nir_def *words = nir_ldc_nv(&b, 4, 32, nir_imm_int(&b, 1), offset, .align_mul = 1);
   nir_def *wide = nir_ldc_nv(&b, 1, 64, nir_imm_int(&b, 1), offset,
                              .base = aligned ? 8 : 3, .align_mul = aligned ? 16 : 1,
                              .align_offset = aligned ? 8 : 0);
   nir_def *constant = nir_ldc_nv(&b, 1, 32, nir_imm_int(&b, 2), zero, .base = 12, .align_mul = 4);
   nir_store_output(&b, words, zero, .write_mask = 15);
   nir_store_output(&b, wide, zero, .write_mask = 1);
   nir_store_output(&b, constant, zero, .write_mask = 1);
   nir_store_output(&b, offset, zero, .write_mask = 1);
   nir_index_ssa_defs(b.impl);
   struct nvk_cbuf_map map = {.cbuf_count = 3};
   map.cbufs[0].type = NVK_CBUF_TYPE_ROOT_DESC;
   map.cbufs[1].type = NVK_CBUF_TYPE_UBO_DESC;
   map.cbufs[2].type = NVK_CBUF_TYPE_SHADER_DATA;
   struct split s = {.fs = b.shader, .impl = b.impl, .map = &map, .layout = layout};
   s.computed = rzalloc_array(b.shader, BITSET_WORD, BITSET_WORDS(b.impl->ssa_alloc));
   s.frontier = rzalloc_array(b.shader, BITSET_WORD, BITSET_WORDS(b.impl->ssa_alloc));
   s.slot = rzalloc_array(b.shader, uint32_t, b.impl->ssa_alloc);
   nir_foreach_block(block, b.impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def)
            BITSET_SET(s.computed, def->index);
      }
   }
   nir_def *values[] = {words, wide, constant, offset};
   const unsigned slots[] = {0, 16, 24, 28};
   for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
      BITSET_SET(s.frontier, values[i]->index);
      s.slot[values[i]->index] = slots[i];
   }
   if (!switch_split_lay_out_cbufs(&s))
      abort();
   nir_shader *out = switch_split_build_prologue(&s);
   ralloc_free(b.shader);
   return out;
}

static void
run(nir_shader *source, struct model *m, unsigned lane)
{
   struct switch_fold_model fold = {
      .ctx = m, .read = read_byte, .write = write_byte,
      .workgroup_id = {lane / SWITCH_SPLIT_BATCH_THREADS},
      .local_id = {lane % SWITCH_SPLIT_BATCH_THREADS},
      .check_alignment = true,
   };
   nir_shader *copy = nir_shader_clone(NULL, source);
   if (!switch_fold(copy, &fold)) {
      fprintf(stderr, "lane %u read outside its declared backing\n", lane);
      abort();
   }
   ralloc_free(copy);
}

static void
lower_ids(nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
         unsigned base;
         if (load->intrinsic == nir_intrinsic_load_workgroup_id)
            base = NAK_SV_CTAID_X;
         else if (load->intrinsic == nir_intrinsic_load_local_invocation_id)
            base = NAK_SV_TID_X;
         else
            continue;
         b.cursor = nir_before_instr(instr);
         nir_def *value[3];
         for (unsigned i = 0; i < 3; ++i)
            value[i] = nir_load_sysval_nv(&b, 32, .base = base + i);
         nir_def_replace(&load->def, nir_vec(&b, value, 3));
      }
   }
}

static void
test(bool aligned)
{
   struct switch_nvk_split_layout layout = {0};
   nir_shader *source = program(&layout, aligned);
   if (layout.batch_version != 1 || layout.batch_threads != 32 ||
       layout.ubo_count != 1 || layout.ubo_cbuf[0] != 1 ||
       layout.prologue_cbufs != 2 || layout.shader_data_cbuf != 1)
      abort();
   struct model m = {0};
   put(m.header, RECORD_VA, 8);
   put(m.header + 8, RECORDS, 4);
   put(m.header + 12, SWITCH_SPLIT_BATCH_RECORD_BYTES, 4);
   for (unsigned row = 0; row < RECORDS; ++row) {
      m.size[row] = (row * 7) % 33;
      m.offset[row] = row % 17;
      if (aligned)
         m.offset[row] &= ~15u;
      put(m.records[row], m.offset[row], 4);
      put(m.records[row] + SWITCH_SPLIT_FRONTIER_ADDRESS, FRONTIER_VA + 256 * row, 8);
      put(m.records[row] + SWITCH_SPLIT_BATCH_BUFFERS + 16, SOURCE_VA + 0x1000 * row, 8);
      put(m.records[row] + SWITCH_SPLIT_BATCH_BUFFERS + 24, m.size[row], 4);
      for (unsigned byte = 0; byte < 64; ++byte)
         m.input[row][byte] = row * 19 + byte * 3;
   }
   for (unsigned form = 0; form < 2; ++form) {
      if (form)
         lower_ids(source);
      for (unsigned lane = 0; lane < 64; ++lane) {
         unsigned before = m.writes;
         run(source, &m, lane);
         if (m.writes - before != (lane < RECORDS ? 32 : 0))
            abort();
      }
   }
   for (unsigned row = 0; row < RECORDS; ++row) {
      for (unsigned byte = 0; byte < 24; ++byte) {
         unsigned at = m.offset[row] + (byte < 16 ? byte : byte - 16 + (aligned ? 8 : 3));
         uint8_t want = at < m.size[row] ? m.input[row][at] : 0;
         if (m.output[row][byte] != want)
            abort();
      }
      for (unsigned byte = 0; byte < 4; ++byte)
         if (m.output[row][24 + byte] != ((12 + byte) * 13 + 7) % 256 ||
             m.output[row][28 + byte] != ((m.offset[row] >> (8 * byte)) & 0xff))
            abort();
   }
   put(m.header + 12, SWITCH_SPLIT_BATCH_RECORD_BYTES - 1, 4);
   unsigned before = m.writes;
   run(source, &m, 0);
   if (m.writes != before)
      abort();
   ralloc_free(source);
}

int
main(void)
{
   test(false);
   test(true);
   puts("33 batched NIR records, 31 inactive lanes, bounded partial and unaligned reads passed");
   return 0;
}
