/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the prologue of a split fragment program (nvk_switch_split.h), the
 * per-draw values the fragment program's per-pixel code reads computed by
 * one lane per draw. Records hold push words, a frontier address and bounded
 * UBO views. Shader constants remain shared in CB1 across the batch.
 */

#include "nvk_switch_split_private.h"
#include "nvk_switch_split_load.h"

#include "nir_builder.h"
#include "util/hash_table.h"

struct prologue {
   nir_builder b;
   nir_def *frontier_va;
   nir_def *record_va;
   struct hash_table *remap;
   int8_t shader_data_cbuf;
};

static nir_def *
prologue_ldc(struct split *s, struct prologue *p, nir_intrinsic_instr *ldc)
{
   nir_builder *b = &p->b;
   const uint32_t cbuf = nir_src_as_uint(ldc->src[0]);
   const struct nvk_cbuf *c = &s->map->cbufs[cbuf];
   nir_def *offset = _mesa_hash_table_search(p->remap, ldc->src[1].ssa)->data;
   uint32_t base = nir_intrinsic_base(ldc);
   nir_def *address, *size;
   switch (c->type) {
   case NVK_CBUF_TYPE_ROOT_DESC:
      address = p->record_va;
      size = nir_imm_int(b, SWITCH_SPLIT_PUSH_BYTES);
      base -= PUSH_START;
      break;
   case NVK_CBUF_TYPE_SHADER_DATA:
      return nir_ldc_nv(b, ldc->def.num_components, ldc->def.bit_size,
                         nir_imm_int(b, p->shader_data_cbuf), offset, .base = base,
                         .align_mul = nir_intrinsic_align_mul(ldc),
                         .align_offset = nir_intrinsic_align_offset(ldc));
   default: {
      const unsigned descriptor = SWITCH_SPLIT_BATCH_BUFFERS + 16 * (1 + s->ubo_of_cbuf[cbuf]);
      address = nir_load_global(b, 1, 64, nir_iadd_imm(b, p->record_va, descriptor), .align_mul = 16);
      size = nir_load_global(b, 1, 32, nir_iadd_imm(b, p->record_va, descriptor + 8), .align_mul = 8);
      break;
   }
   }
   return switch_split_load(b, address, size, nir_iadd_imm(b, nir_u2u64(b, offset), base),
                             ldc->def.num_components, ldc->def.bit_size,
                             MIN2(nir_intrinsic_align(ldc), 16));
}

/* The frontier is 256-byte aligned: the alignment lets NAK merge stores */
static void
store_frontier(struct prologue *p, nir_def *value, uint32_t at)
{
   nir_builder *b = &p->b;
   if (value->bit_size == 1)
      value = nir_b2i32(b, value);
   else if (value->bit_size < 32)
      value = nir_u2u32(b, value);
   nir_store_global(b, value, nir_iadd_imm(b, p->frontier_va, at),
                    .align_mul = 256, .align_offset = at % 256, .write_mask = 1);
}

static void
build_prologue_body(struct split *s, struct prologue *p)
{
   nir_builder *b = &p->b;
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def == NULL || !BITSET_TEST(s->computed, def->index))
            continue;
         if (instr->type == nir_instr_type_intrinsic) {
            nir_def *v = prologue_ldc(s, p, nir_instr_as_intrinsic(instr));
            _mesa_hash_table_insert(p->remap, def, v);
         } else {
            nir_instr *copy = nir_instr_clone_deep(b->shader, instr, p->remap);
            nir_builder_instr_insert(b, copy);
         }
         if (!BITSET_TEST(s->frontier, def->index))
            continue;
         nir_def *v = _mesa_hash_table_search(p->remap, def)->data;
         const nir_component_mask_t read = nir_def_components_read(def);
         u_foreach_bit(c, read)
            store_frontier(p, nir_channel(b, v, c),
                           s->slot[def->index] + c * slot_bytes(def));
      }
   }
}

static bool
reads_cbuf_type(struct split *s, enum nvk_cbuf_type type)
{
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def == NULL || !BITSET_TEST(s->computed, def->index) ||
             instr->type != nir_instr_type_intrinsic)
            continue;
         const uint32_t cbuf =
            nir_src_as_uint(nir_instr_as_intrinsic(instr)->src[0]);
         if (s->map->cbufs[cbuf].type == type)
            return true;
      }
   }
   return false;
}

bool
switch_split_lay_out_cbufs(struct split *s)
{
   struct switch_nvk_split_layout *l = s->layout;
   memset(s->ubo_of_cbuf, -1, sizeof(s->ubo_of_cbuf));
   nir_foreach_block(block, s->impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def == NULL || !BITSET_TEST(s->computed, def->index) ||
             instr->type != nir_instr_type_intrinsic)
            continue;
         const uint32_t cbuf =
            nir_src_as_uint(nir_instr_as_intrinsic(instr)->src[0]);
         if (s->map->cbufs[cbuf].type == NVK_CBUF_TYPE_UBO_DESC &&
             s->ubo_of_cbuf[cbuf] < 0) {
            if (l->ubo_count == SWITCH_SPLIT_MAX_UBOS)
               return false;
            s->ubo_of_cbuf[cbuf] = l->ubo_count;
            l->ubo_cbuf[l->ubo_count++] = cbuf;
         }
      }
   }
   l->batch_version = SWITCH_SPLIT_BATCH_VERSION;
   l->batch_threads = SWITCH_SPLIT_BATCH_THREADS;
   l->prologue_cbufs = 1;
   if (reads_cbuf_type(s, NVK_CBUF_TYPE_SHADER_DATA))
      l->shader_data_cbuf = l->prologue_cbufs++;
   return true;
}

nir_shader *
switch_split_build_prologue(struct split *s)
{
   struct prologue p = {
      .b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, s->fs->options,
                                          "switch_split_prologue"),
      .remap = _mesa_pointer_hash_table_create(NULL),
      .shader_data_cbuf = s->layout->shader_data_cbuf,
   };
   nir_builder *b = &p.b;
   nir_shader *pro = b->shader;
   pro->info.workgroup_size[0] = s->layout->batch_threads;
   pro->info.workgroup_size[1] = 1;
   pro->info.workgroup_size[2] = 1;
   pro->info.float_controls_execution_mode =
      s->fs->info.float_controls_execution_mode;
   pro->info.io_lowered = true;
   if (p.shader_data_cbuf > 0) {
      pro->constant_data_size = s->fs->constant_data_size;
      pro->constant_data = ralloc_memdup(pro, s->fs->constant_data,
                                         s->fs->constant_data_size);
   }

   nir_def *header = nir_ldc_nv(b, 4, 32, nir_imm_int(b, SWITCH_SPLIT_PARAMS_CBUF),
                               nir_imm_int(b, 0), .align_mul = 16);
   nir_def *lane = nir_iadd(b, nir_imul_imm(b, nir_channel(b, nir_load_workgroup_id(b), 0),
                                          s->layout->batch_threads),
                           nir_channel(b, nir_load_local_invocation_id(b), 0));
   nir_def *count = nir_channel(b, header, 2);
   nir_def *stride = nir_channel(b, header, 3);
   nir_def *valid = nir_iand(b, nir_ult(b, lane, count),
                            nir_ieq_imm(b, stride, SWITCH_SPLIT_BATCH_RECORD_BYTES));
   nir_if *active = nir_push_if(b, valid);
   p.record_va = nir_iadd(b, nir_pack_64_2x32_split(b, nir_channel(b, header, 0), nir_channel(b, header, 1)),
                          nir_imul(b, nir_u2u64(b, lane), nir_u2u64(b, stride)));
   p.frontier_va = nir_load_global(b, 1, 64, nir_iadd_imm(b, p.record_va, SWITCH_SPLIT_FRONTIER_ADDRESS), .align_mul = 16);
   build_prologue_body(s, &p);
   nir_pop_if(b, active);

   _mesa_hash_table_destroy(p.remap, NULL);
   nir_validate_shader(pro, "switch split prologue");
   return pro;
}
