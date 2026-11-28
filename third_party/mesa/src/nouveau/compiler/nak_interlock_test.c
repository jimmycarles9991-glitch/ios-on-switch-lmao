/* SPDX-License-Identifier: MIT */
#include "nak_private.h"
#include "nil.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct nak_constant_offset_info nak_const_offsets_base = {
   .interlock_ticket_buffer_cb = 0,
   .interlock_ticket_buffer_offset = 0x260,
};
const struct nak_constant_offset_info nak_const_offsets_turing_graphics = {
   .interlock_ticket_buffer_cb = 0,
   .interlock_ticket_buffer_offset = 0x260,
};

static void
require(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "%s\n", what);
      abort();
   }
}

struct section {
   unsigned ticket_reads, waits, releases;
   unsigned clock_reads, ssa_bars;
   unsigned vc_membars, device_releases;
   unsigned clock_reads_before_wait_break;
   bool saw_wait_break;
   nir_def *wait_addr, *release_addr;
};

static void
count(nir_shader *nir, struct section *s)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_jump &&
                nir_instr_as_jump(instr)->type == nir_jump_break) {
               s->saw_wait_break = true;
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_sysval_nv:
               s->ticket_reads +=
                  nir_intrinsic_base(intr) == NAK_SV_ORDERING_TICKET;
               if (nir_intrinsic_base(intr) == NAK_SV_CLOCK_LO) {
                  s->clock_reads++;
                  if (!s->saw_wait_break)
                     s->clock_reads_before_wait_break++;
               }
               break;
            case nir_intrinsic_ssa_bar_nv:
               s->ssa_bars++;
               break;
            case nir_intrinsic_membar_vc_nv:
               s->vc_membars++;
               break;
            case nir_intrinsic_barrier:
               s->device_releases +=
                  nir_intrinsic_memory_scope(intr) == SCOPE_DEVICE &&
                  nir_intrinsic_memory_semantics(intr) == NIR_MEMORY_RELEASE;
               break;
            case nir_intrinsic_load_global:
               s->waits++;
               s->wait_addr = intr->src[0].ssa;
               break;
            case nir_intrinsic_global_atomic:
               s->releases++;
               s->release_addr = intr->src[0].ssa;
               break;
            default:
               break;
            }
         }
      }
   }
}

static void
test_nir_lowering_sm53(void)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "interlock53");
   b.shader->info.fs.pixel_interlock_ordered = true;
   nir_begin_invocation_interlock(&b);
   nir_end_invocation_interlock(&b);

   const struct nak_compiler nak = {.sm = 53};
   require(nak_nir_lower_fs_interlock(b.shader, &nak),
           "sm53 interlock section was not lowered");
   nir_lower_vars_to_ssa(b.shader);
   nir_opt_copy_prop(b.shader);
   nir_opt_dce(b.shader);

   struct section s = {0};
   count(b.shader, &s);
   require(s.ticket_reads == 1, "sm53 read ticket more than once");
   require(s.waits == 1, "sm53 release waited for ticket again");
   require(s.releases == 1, "sm53 did not emit one release");
   require(s.wait_addr == s.release_addr,
           "sm53 release did not reuse waited ticket word");
   require(s.clock_reads == 2, "sm53 backoff should emit start and now clock reads");
   require(s.ssa_bars == 2, "sm53 backoff should emit two ssa_bar_nv barriers");
   require(s.clock_reads_before_wait_break == 0,
           "sm53 fast path must read zero clocks before the wait break");
   ralloc_free(b.shader);
}

static void
test_nir_lowering_sm75(void)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "interlock75");
   b.shader->info.fs.pixel_interlock_ordered = true;
   nir_begin_invocation_interlock(&b);
   nir_end_invocation_interlock(&b);

   const struct nak_compiler nak = {.sm = 75};
   require(nak_nir_lower_fs_interlock(b.shader, &nak),
           "sm75 interlock section was not lowered");
   nir_lower_vars_to_ssa(b.shader);
   nir_opt_copy_prop(b.shader);
   nir_opt_dce(b.shader);

   struct section s = {0};
   count(b.shader, &s);
   require(s.ticket_reads == 1, "sm75 read ticket more than once");
   require(s.waits == 0, "sm75 should use atomic spin, not load_global");
   require(s.releases == 2, "sm75 should emit atomic wait and atomic release");
   require(s.clock_reads == 0, "sm75 should not emit Maxwell clock backoff");
   require(s.ssa_bars == 0, "sm75 should not emit Maxwell clock barriers");
   ralloc_free(b.shader);
}

static uint64_t
sm50_word(const uint32_t *code, unsigned instr_idx)
{
   unsigned group = instr_idx / 3;
   unsigned slot = instr_idx % 3;
   unsigned base = group * 8 + (slot + 1) * 2;
   return (uint64_t)code[base] | ((uint64_t)code[base + 1] << 32);
}

static uint32_t
sm50_sched(const uint32_t *code, unsigned instr_idx)
{
   unsigned group = instr_idx / 3;
   unsigned slot = instr_idx % 3;
   uint64_t sw = (uint64_t)code[group * 8] | ((uint64_t)code[group * 8 + 1] << 32);
   return (uint32_t)((sw >> (21 * slot)) & 0x1fffffu);
}

static nir_shader *
make_interlocked_fs(const struct nak_compiler *nak)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, nak_nir_options(nak), "interlock_compile");
   b.shader->info.io_lowered = true;
   b.shader->info.fs.pixel_interlock_ordered = true;

   nir_def *addr = nir_ldc_nv(&b, 1, 64, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 8, .align_offset = 0);
   nir_begin_invocation_interlock(&b);
   nir_def *val = nir_load_global(&b, 1, 32, addr, .align_mul = 4);
   nir_store_global(&b, nir_iadd_imm(&b, val, 1), addr, .align_mul = 4);
   nir_end_invocation_interlock(&b);
   return b.shader;
}

static nir_shader *
make_shader_clock_fs(const struct nak_compiler *nak)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, nak_nir_options(nak), "clock_compile");
   b.shader->info.io_lowered = true;

   nir_def *addr = nir_ldc_nv(&b, 1, 64, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                              .align_mul = 8, .align_offset = 0);
   nir_def *clk = nir_shader_clock(&b, 64, .memory_scope = SCOPE_SUBGROUP);
   nir_store_global(&b, clk, addr, .align_mul = 8);
   return b.shader;
}

static void
test_compile_sm53_interlock(void)
{
   struct nv_device_info dev = {.sm = 53, .max_warps_per_mp = 64};
   struct nak_compiler *nak = nak_compiler_create(&dev);
   require(nak != NULL, "nak_compiler_create(sm53) failed");

   nir_shader *nir = make_interlocked_fs(nak);
   struct nak_fs_key fs_key = {0};
   struct nak_shader_bin *bin =
      nak_compile_shader(nir, true, nak, 0, &fs_key, false);
   require(bin != NULL, "sm53 interlock compile failed");
   require(bin->code_size > 0 && (bin->code_size % 32) == 0,
           "invalid sm53 code size");

   const uint32_t *code = bin->code;
   unsigned n = (bin->code_size / 32) * 3;
   int first_ldg_cg = -1;
   int first_fast_exit = -1;
   unsigned clock_s2r_count = 0;
   unsigned clock_s2r_before_exit = 0;
   bool has_iadd_sub = false;
   bool has_isetp_ge_u32 = false;
   bool has_bra = false;
   int last_membar = -1;
   unsigned reds = 0;

   for (unsigned i = 0; i < n; i++) {
      uint64_t w = sm50_word(code, i);
      uint32_t sched = sm50_sched(code, i);
      uint16_t op16 = (uint16_t)(w >> 48);

      require(op16 != 0x50c8, "sm53 interlock emitted unverified CS2R");

      if (op16 == 0xef98)
         last_membar = (int)((w >> 8) & 3);
      if ((op16 & 0xfff8) == 0xebf8) {
         reds++;
         require(last_membar == 3, "sm53 release RED must follow MEMBAR.VC");
      }

      if ((op16 & 0xfff8) == 0xeed0 && ((w >> 46) & 3) == 1 && first_ldg_cg < 0)
         first_ldg_cg = (int)i;
      if (first_ldg_cg >= 0 && first_fast_exit < 0 &&
          (op16 == 0xe340 || op16 == 0xe240))
         first_fast_exit = (int)i;

      if (op16 == 0xf0c8 && ((w >> 20) & 0xff) == NAK_SV_CLOCK_LO) {
         clock_s2r_count++;
         if (first_fast_exit < 0)
            clock_s2r_before_exit++;
         uint32_t wr_bar = (sched >> 5) & 7;
         uint32_t delay = sched & 0xf;
         require(wr_bar < 6, "S2R SR_CLOCKLO must allocate wr_bar");
         require(delay >= 2, "S2R SR_CLOCKLO must delay >= 2 before wait");
         require(i + 1 < n, "S2R SR_CLOCKLO at end of shader");
         uint64_t next_w = sm50_word(code, i + 1);
         uint32_t next_sched = sm50_sched(code, i + 1);
         uint32_t wt_mask = (next_sched >> 11) & 0x3f;
         require((next_w >> 48) == 0x50b0,
                 "Op::SrcBar must lower to NOP after S2R SR_CLOCKLO");
         require((wt_mask & (1u << wr_bar)) != 0,
                 "NOP from Op::SrcBar must wait on S2R SR_CLOCKLO wr_bar");
      }

      if ((op16 & 0xfffc) == 0x5c10 && ((w >> 48) & 3) != 0)
         has_iadd_sub = true;
      if (((op16 & 0xff00) == 0x5b00 || (op16 & 0xfe00) == 0x3600) &&
          ((w >> 49) & 7) == 6 && ((w >> 48) & 1) == 0)
         has_isetp_ge_u32 = true;
      if (op16 == 0xe240)
         has_bra = true;
   }

   require(first_ldg_cg >= 0, "sm53 interlock missing LDG.E.CG ticket poll");
   require(first_fast_exit > first_ldg_cg,
           "sm53 interlock missing fast-path break after LDG.E.CG");
   require(clock_s2r_count == 2,
           "sm53 interlock should emit start and now S2R SR_CLOCKLO");
   require(clock_s2r_before_exit == 0,
           "sm53 interlock must have zero clock reads on uncontended path");
   require(has_iadd_sub, "sm53 backoff missing IADD subtraction");
   require(has_isetp_ge_u32, "sm53 backoff missing ISETP.GE.U32");
   require(has_bra, "sm53 backoff missing BRA");
   require(reds == 1, "sm53 interlock should release with one RED");

   nak_shader_bin_destroy(bin);
   ralloc_free(nir);

   /* 64-bit shader_clock on SM53 also uses S2R + SrcBar without CS2R */
   nir_shader *clk_nir = make_shader_clock_fs(nak);
   struct nak_shader_bin *clk_bin =
      nak_compile_shader(clk_nir, true, nak, 0, &fs_key, false);
   require(clk_bin != NULL, "sm53 64-bit shader_clock compile failed");
   const uint32_t *clk_code = clk_bin->code;
   unsigned clk_n = (clk_bin->code_size / 32) * 3;
   unsigned lo_reads = 0, hi_reads = 0;
   for (unsigned i = 0; i < clk_n; i++) {
      uint64_t w = sm50_word(clk_code, i);
      uint32_t sched = sm50_sched(clk_code, i);
      uint16_t op16 = (uint16_t)(w >> 48);
      require(op16 != 0x50c8, "sm53 shader_clock must not emit CS2R");
      if (op16 == 0xf0c8) {
         uint8_t sr = (uint8_t)((w >> 20) & 0xff);
         if (sr == NAK_SV_CLOCK_LO || sr == NAK_SV_CLOCK_HI) {
            lo_reads += sr == NAK_SV_CLOCK_LO;
            hi_reads += sr == NAK_SV_CLOCK_HI;
            uint32_t wr_bar = (sched >> 5) & 7;
            require(wr_bar < 6, "shader_clock S2R must allocate wr_bar");
            require(i + 1 < clk_n, "shader_clock S2R at end of shader");
            uint64_t next_w = sm50_word(clk_code, i + 1);
            uint32_t next_sched = sm50_sched(clk_code, i + 1);
            require((next_w >> 48) == 0x50b0 &&
                       (((next_sched >> 11) & 0x3f) & (1u << wr_bar)) != 0,
                    "shader_clock S2R must be followed by NOP waiting on wr_bar");
         }
      }
   }
   require(lo_reads >= 1 && hi_reads >= 2,
           "sm53 64-bit shader_clock must emit S2R for SR_CLOCKLO and SR_CLOCKHI");
   nak_shader_bin_destroy(clk_bin);
   ralloc_free(clk_nir);

   nak_compiler_destroy(nak);
}

static unsigned
count_texop(nir_shader *nir, nir_texop op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block)
            n += instr->type == nir_instr_type_tex &&
                 nir_instr_as_tex(instr)->op == op;
      }
   }
   return n;
}

/* The storage form's multisampled image load on GM20B takes its sample's
 * place in the pixel from the fixed layout, with no TXQ of the sample
 * position; SM75 keeps the query */
static void
test_msaa_image_offsets(void)
{
   const struct nir_shader_compiler_options options = {0};
   for (unsigned sm = 53; sm <= 75; sm += 22) {
      nir_builder b =
         nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "ms");
      nir_def *coord = nir_vec4(&b, nir_imm_int(&b, 3), nir_imm_int(&b, 7),
                                nir_imm_int(&b, 0), nir_imm_int(&b, 0));
      nir_def *texel = nir_bindless_image_load(
         &b, 4, 32, nir_imm_int(&b, 5), coord, nir_imm_int(&b, 2),
         nir_imm_int(&b, 0), .image_dim = GLSL_SAMPLER_DIM_MS,
         .format = PIPE_FORMAT_R8G8B8A8_UNORM);
      nir_store_output(&b, texel, nir_imm_int(&b, 0),
                       .io_semantics.location = FRAG_RESULT_DATA0);
      const struct nak_compiler nak = {.sm = sm};
      require(nak_nir_lower_tex(b.shader, &nak), "MS image load not lowered");
      unsigned queries = count_texop(b.shader, nir_texop_sample_pos_nv);
      require(sm < 70 ? queries == 0 : queries == 1,
              sm < 70 ? "sm53 queries a sample position"
                      : "sm75 lost its sample position query");
      ralloc_free(b.shader);
   }
}

/* The cell an MS image access lowered on GM20B reads, for each sample count
 * and sample, is NIL's own (nil_sample_offset of nil_choose_sample_layout):
 * the TXQ of the count answered with a constant, the coordinates fold */
static void
test_msaa_cells_are_nil_s(void)
{
   const struct nir_shader_compiler_options options = {0};
   const struct nak_compiler nak = {.sm = 53};
   for (unsigned samples = 1; samples <= 8; samples *= 2) {
      for (unsigned s = 0; s < samples; s++) {
         nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                        &options, "cell");
         nir_def *zero = nir_imm_int(&b, 0);
         nir_def *texel = nir_bindless_image_load(
            &b, 4, 32, nir_imm_int(&b, 5), nir_vec4(&b, zero, zero, zero, zero),
            nir_imm_int(&b, s), zero, .image_dim = GLSL_SAMPLER_DIM_MS,
            .format = PIPE_FORMAT_R8G8B8A8_UNORM);
         nir_store_output(&b, texel, zero,
                          .io_semantics.location = FRAG_RESULT_DATA0);
         require(nak_nir_lower_tex(b.shader, &nak), "MS image load not lowered");
         nir_foreach_function_impl(impl, b.shader) {
            nir_foreach_block(block, impl) {
               nir_foreach_instr_safe(instr, block) {
                  if (instr->type != nir_instr_type_tex ||
                      nir_instr_as_tex(instr)->op != nir_texop_tex_type_nv)
                     continue;
                  nir_tex_instr *txq = nir_instr_as_tex(instr);
                  b.cursor = nir_before_instr(instr);
                  nir_def_rewrite_uses(&txq->def,
                                       nir_imm_ivec4(&b, 0, 0, samples, 0));
               }
            }
         }
         bool progress;
         do {
            progress = nir_opt_constant_folding(b.shader);
            progress |= nir_opt_algebraic(b.shader);
            progress |= nir_opt_copy_prop(b.shader);
            progress |= nir_opt_dce(b.shader);
         } while (progress);
         struct nil_sample_offset want =
            nil_sample_offset(nil_choose_sample_layout(samples), s);
         bool found = false;
         nir_foreach_function_impl(impl, b.shader) {
            nir_foreach_block(block, impl) {
               nir_foreach_instr(instr, block) {
                  if (instr->type != nir_instr_type_intrinsic)
                     continue;
                  nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
                  if (load->intrinsic != nir_intrinsic_bindless_image_load)
                     continue;
                  require(nir_src_is_const(load->src[1]),
                          "the cell did not fold to a constant");
                  found = true;
                  if (nir_src_comp_as_uint(load->src[1], 0) != want.x ||
                      nir_src_comp_as_uint(load->src[1], 1) != want.y) {
                     fprintf(stderr, "%ux sample %u: (%u, %u), NIL (%u, %u)\n",
                             samples, s,
                             (unsigned)nir_src_comp_as_uint(load->src[1], 0),
                             (unsigned)nir_src_comp_as_uint(load->src[1], 1),
                             want.x, want.y);
                     abort();
                  }
               }
            }
         }
         require(found, "no image load left");
         ralloc_free(b.shader);
      }
   }
}

/* A half's fmax and fmin on SM53 answer as Apple's GPUs do (host gpu_test
 * SignedZeroTests): x where x > y (x < y for fmin) or y is a NaN, so a tie of
 * +0 and -0 gives y. The full compile's HSETP2s: a NaN test of y alone, then
 * GT (fmax) or LT (fmin) of x against y; the SEL keeps x where it holds */
static void
test_half_min_max_take_y_at_a_tie(void)
{
   struct nv_device_info dev = {.sm = 53, .max_warps_per_mp = 64};
   struct nak_compiler *nak = nak_compiler_create(&dev);
   for (unsigned max = 0; max < 2; max++) {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_FRAGMENT, nak_nir_options(nak), "min_max");
      b.shader->info.io_lowered = true;
      nir_def *addr = nir_ldc_nv(&b, 1, 64, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                                 .align_mul = 8, .align_offset = 0);
      nir_def *xy = nir_load_global(&b, 2, 16, addr, .align_mul = 4);
      nir_def *x = nir_channel(&b, xy, 0), *y = nir_channel(&b, xy, 1);
      nir_def *r = max ? nir_fmax(&b, x, y) : nir_fmin(&b, x, y);
      nir_store_global(&b, nir_vec2(&b, r, r), addr, .align_mul = 4);
      struct nak_fs_key fs_key = {0};
      struct nak_shader_bin *bin =
         nak_compile_shader(b.shader, true, nak, 0, &fs_key, false);
      require(bin != NULL, "min/max compile failed");
      const uint32_t *code = bin->code;
      unsigned n = (bin->code_size / 32) * 3, nan_tests = 0, compares = 0;
      unsigned nan_of = ~0u, compared_b = ~0u;
      for (unsigned i = 0; i < n; i++) {
         uint64_t w = sm50_word(code, i);
         if (((uint16_t)(w >> 48) & 0xfff8) != 0x5d20)
            continue;
         unsigned cmp = (w >> 35) & 0xf;
         unsigned a = (w >> 8) & 0xff | ((w >> 47) & 3) << 8;
         unsigned b2 = (w >> 20) & 0xff | ((w >> 28) & 3) << 8;
         if (cmp == 8) {
            nan_tests++;
            require(a == b2, "the NaN test reads one operand twice");
            nan_of = a;
         } else {
            compares++;
            require(cmp == (max ? 4 : 1),
                    max ? "fmax keeps x where x > y" : "fmin keeps x where x < y");
            compared_b = b2;
         }
      }
      require(nan_tests == 1 && compares == 1, "one NaN test and one compare");
      require(nan_of == compared_b, "the NaN test is of y, the compare's B");
      nak_shader_bin_destroy(bin);
      ralloc_free(b.shader);
   }
   nak_compiler_destroy(nak);
}

static void
test_compile_sm75(void)
{
   struct nv_device_info dev = {.sm = 75, .max_warps_per_mp = 32};
   struct nak_compiler *nak = nak_compiler_create(&dev);
   require(nak != NULL, "nak_compiler_create(sm75) failed");

   struct nak_fs_key fs_key = {0};

   nir_shader *nir = make_interlocked_fs(nak);
   struct nak_shader_bin *bin =
      nak_compile_shader(nir, true, nak, 0, &fs_key, false);
   require(bin != NULL && bin->asm_str != NULL, "sm75 interlock compile failed");
   require(strstr(bin->asm_str, "cs2r") == NULL,
           "sm75 interlock should not emit clock backoff");
   require(strstr(bin->asm_str, "sr[0x50]") == NULL,
           "sm75 interlock should not read SR_CLOCKLO");
   nak_shader_bin_destroy(bin);
   ralloc_free(nir);

   nir_shader *clk_nir = make_shader_clock_fs(nak);
   struct nak_shader_bin *clk_bin =
      nak_compile_shader(clk_nir, true, nak, 0, &fs_key, false);
   require(clk_bin != NULL && clk_bin->asm_str != NULL,
           "sm75 64-bit shader_clock compile failed");
   require(strstr(clk_bin->asm_str, "cs2r sr[0x50]") != NULL &&
              strstr(clk_bin->asm_str, "cs2r sr[0x51]") != NULL,
           "sm75 shader_clock must keep CS2R");
   nak_shader_bin_destroy(clk_bin);
   ralloc_free(clk_nir);

   nak_compiler_destroy(nak);
}

/* A section as SPIR-V gives it (an acquire after the begin, a release before
 * the end) with a demote inside: two releases, the bail-out's and the end's */
static struct section
lower_spirv_section(unsigned sm)
{
   const struct nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "release");
   b.shader->info.fs.pixel_interlock_ordered = true;
   const nir_variable_mode modes =
      nir_var_image | nir_var_mem_ssbo | nir_var_mem_global;
   nir_begin_invocation_interlock(&b);
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE, NIR_MEMORY_ACQUIRE, modes);
   nir_demote_if(&b, nir_ieq_imm(&b, nir_load_subgroup_invocation(&b), 3));
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE, NIR_MEMORY_RELEASE, modes);
   nir_end_invocation_interlock(&b);

   const struct nak_compiler nak = {.sm = sm};
   require(nak_nir_lower_fs_interlock(b.shader, &nak), "section not lowered");
   nir_lower_vars_to_ssa(b.shader);
   nir_opt_dce(b.shader);
   struct section s = {0};
   count(b.shader, &s);
   ralloc_free(b.shader);
   return s;
}

static void
test_release_order(void)
{
   struct section s = lower_spirv_section(53);
   require(s.releases == 2, "sm53: a release for the demote and the end");
   require(s.vc_membars == s.releases, "sm53: a MEMBAR.VC before every release");
   require(s.device_releases == 0,
           "sm53: the release barrier before the end is MEMBAR.VC alone");
   s = lower_spirv_section(75);
   require(s.vc_membars == 0, "sm75 has no MEMBAR.VC");
   require(s.device_releases == 1, "sm75 keeps the release barrier");
}

int
main(void)
{
   test_release_order();
   test_nir_lowering_sm53();
   test_nir_lowering_sm75();
   test_compile_sm53_interlock();
   test_compile_sm75();
   test_msaa_image_offsets();
   test_msaa_cells_are_nil_s();
   test_half_min_max_take_y_at_a_tie();
   puts("interlock NIR and SM53/SM75 full-pipeline SASS checks passed");
   return 0;
}
