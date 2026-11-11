/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the prologue of a split fragment program (nvk_switch_split.h), the
 * per-draw values the fragment program's per-pixel code reads computed once
 * for a draw and written to its frontier. It reads what the fragment program
 * read through constant buffers of its own bound as the draw binds them: a
 * uniform buffer at the draw's address and size, the root table's push words
 * from the draw's copy, the shader data. Constant buffer reads cost the
 * prologue what they cost the fragment program (an operand, a read past the
 * bound size is 0), where global loads would need an address and a bounds
 * test each.
 */

#include "nvk_switch_split_private.h"

#include "nir_builder.h"
#include "util/hash_table.h"

struct prologue {
   nir_builder b;
   nir_def *frontier_va;
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
   uint32_t prologue_cbuf;
   switch (c->type) {
   case NVK_CBUF_TYPE_ROOT_DESC:
      prologue_cbuf = SWITCH_SPLIT_PARAMS_CBUF;
      base -= PUSH_START;
      break;
   case NVK_CBUF_TYPE_SHADER_DATA:
      prologue_cbuf = p->shader_data_cbuf;
      break;
   default:
      prologue_cbuf = 1 + s->ubo_of_cbuf[cbuf];
      break;
   }
   return nir_ldc_nv(b, ldc->def.num_components, ldc->def.bit_size,
                     nir_imm_int(b, prologue_cbuf), offset, .base = base,
                     .align_mul = nir_intrinsic_align_mul(ldc),
                     .align_offset = nir_intrinsic_align_offset(ldc));
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
   l->prologue_cbufs = 1 + l->ubo_count;
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
   pro->info.workgroup_size[0] = 1;
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

   p.frontier_va = nir_ldc_nv(b, 1, 64, nir_imm_int(b, SWITCH_SPLIT_PARAMS_CBUF),
                              nir_imm_int(b, 0),
                              .base = SWITCH_SPLIT_FRONTIER_ADDRESS,
                              .align_mul = 8);
   build_prologue_body(s, &p);

   _mesa_hash_table_destroy(p.remap, NULL);
   nir_validate_shader(pro, "switch split prologue");
   return pro;
}
