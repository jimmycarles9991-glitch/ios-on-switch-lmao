/* SPDX-License-Identifier: MIT */
/* nvk_switch_xr_planes.c over hand-built fragment programs: a sample, fetch
 * or gather of a joined binding reads its header and the next one (the alpha
 * plane's), each read of plane 0's swizzled RGB10A2Unorm (c / 1023) and plane
 * 1's R8Unorm (b / 255) comes out as Metal's half of the code, at f16 and f32;
 * an alpha gather reads plane 1 alone, a size query and another binding's
 * sample are left as they are.
 */
#include "nvk_switch_xr_planes.h"

#include "nir_builder.h"
#include "util/half_float.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SINK 0x1000
#define JOINED 1
#define OTHER 2

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

static nir_variable *
texture(nir_shader *s, unsigned binding)
{
   const struct glsl_type *type = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *var = nir_variable_create(s, nir_var_uniform, type, "t");
   var->data.descriptor_set = 0;
   var->data.binding = binding;
   return var;
}

static nir_def *
texture_read(nir_builder *b, nir_variable *var, nir_texop op, unsigned bit_size, unsigned component)
{
   nir_deref_instr *d = nir_build_deref_var(b, var);
   const bool fetch = op == nir_texop_txf, query = op == nir_texop_txs;
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, fetch || query ? 2 + fetch : 3);
   tex->op = op;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = query ? nir_type_int32 : bit_size == 16 ? nir_type_float16 : nir_type_float32;
   tex->component = component;
   unsigned at = 0;
   tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &d->def);
   if (fetch) {
      tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_ivec2(b, 3, 5));
      tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
   } else if (query) {
      tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
   } else {
      tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &d->def);
      tex->src[at++] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_vec2(b, 0.25f, 0.5f));
   }
   nir_def_init(&tex->instr, &tex->def, query ? 2 : 4, query ? 32 : bit_size);
   nir_builder_instr_insert(b, &tex->instr);
   return &tex->def;
}

static nir_shader *
program(nir_texop op, unsigned binding, unsigned bit_size, unsigned component)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "xr-planes");
   nir_def *texel = texture_read(&b, texture(b.shader, binding), op, bit_size, component);
   nir_store_global(&b, texel, nir_imm_int64(&b, SINK), .align_mul = 4);
   return b.shader;
}

static void
clean(nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_dce);
   } while (progress);
}

struct reads {
   unsigned plane0, plane1, other;
};

static struct reads
reads_of(nir_shader *s)
{
   struct reads r = {0};
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->backend_flags & SWITCH_NVK_TEX_ALPHA_PLANE)
               r.plane1++;
            else if (tex->def.bit_size == 32 && tex->dest_type == nir_type_float32)
               r.plane0++;
            else
               r.other++;
         }
      }
   }
   return r;
}

/* The program's stored lanes with plane 0's read `word` and plane 1's `alpha` */
static void
joined_at(nir_shader *s, const float word[4], const float alpha[4], float out[4])
{
   nir_foreach_function_impl(impl, s) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            const float *v = tex->backend_flags & SWITCH_NVK_TEX_ALPHA_PLANE ? alpha : word;
            b.cursor = nir_before_instr(instr);
            nir_def_replace(&tex->def, nir_imm_vec4(&b, v[0], v[1], v[2], v[3]));
         }
      }
   }
   clean(s);
   for (unsigned i = 0; i < 4; i++)
      out[i] = NAN;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic ||
                nir_instr_as_intrinsic(instr)->intrinsic != nir_intrinsic_store_global)
               continue;
            nir_intrinsic_instr *store = nir_instr_as_intrinsic(instr);
            if (!nir_src_is_const(store->src[0]))
               continue;
            for (unsigned i = 0; i < 4; i++)
               out[i] = nir_src_comp_as_float(store->src[0], i);
         }
      }
   }
}

static float
half_of(double exact)
{
   return _mesa_half_to_float(_mesa_float_to_half((float)exact));
}

static void
test_reads(void)
{
   const nir_texop ops[] = {nir_texop_tex, nir_texop_txf};
   for (unsigned o = 0; o < 2; o++) {
      for (unsigned bits = 16; bits <= 32; bits += 16) {
         nir_shader *s = program(ops[o], JOINED, bits, 0);
         check(switch_nvk_xr_planes(s, 1ull << JOINED), "a joined binding's read is joined");
         const struct reads r = reads_of(s);
         char what[96];
         snprintf(what, sizeof(what), "%s at f%u reads plane 0 and plane 1 once each (%u, %u, %u)",
                  o ? "a fetch" : "a sample", bits, r.plane0, r.plane1, r.other);
         check(r.plane0 == 1 && r.plane1 == 1 && r.other == 0, what);
         ralloc_free(s);
      }
   }
   nir_shader *other = program(nir_texop_tex, OTHER, 32, 0);
   check(!switch_nvk_xr_planes(other, 1ull << JOINED), "another binding's sample is left");
   ralloc_free(other);
   nir_shader *size = program(nir_texop_txs, JOINED, 32, 0);
   check(!switch_nvk_xr_planes(size, 1ull << JOINED), "a size query reads plane 0's header as it is");
   ralloc_free(size);
   nir_shader *vertex = program(nir_texop_tex, JOINED, 32, 0);
   vertex->info.stage = MESA_SHADER_VERTEX;
   check(!switch_nvk_xr_planes(vertex, 1ull << JOINED), "a vertex stage is left");
   ralloc_free(vertex);
}

static void
test_values(void)
{
   const unsigned codes[] = {0, 1, 127, 383, 384, 385, 512, 640, 893, 894, 1000, 1022, 1023};
   const unsigned bytes[] = {0, 1, 77, 128, 250, 254, 255};
   for (unsigned bits = 16; bits <= 32; bits += 16) {
      for (unsigned i = 0; i < ARRAY_SIZE(codes); i++) {
         const unsigned r = codes[i], g = codes[(i + 4) % ARRAY_SIZE(codes)], bl = codes[(i + 9) % ARRAY_SIZE(codes)];
         const unsigned a = bytes[i % ARRAY_SIZE(bytes)];
         const float word[4] = {r / 1023.0f, g / 1023.0f, bl / 1023.0f, 1.0f};
         const float alpha[4] = {a / 255.0f, 0, 0, 1};
         nir_shader *s = program(nir_texop_tex, JOINED, bits, 0);
         switch_nvk_xr_planes(s, 1ull << JOINED);
         float got[4];
         joined_at(s, word, alpha, got);
         const float want[4] = {half_of((r - 384.0) / 510.0), half_of((g - 384.0) / 510.0),
                                half_of((bl - 384.0) / 510.0), half_of(a / 255.0)};
         char what[128];
         snprintf(what, sizeof(what), "f%u codes %u %u %u alpha %u: %g %g %g %g, want %g %g %g %g", bits, r, g, bl,
                  a, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
         check(memcmp(got, want, sizeof(want)) == 0, what);
         ralloc_free(s);
      }
   }
}

static void
test_gathers(void)
{
   nir_shader *alpha = program(nir_texop_tg4, JOINED, 32, 3);
   switch_nvk_xr_planes(alpha, 1ull << JOINED);
   const struct reads ra = reads_of(alpha);
   check(ra.plane0 == 0 && ra.plane1 == 1, "an alpha gather reads plane 1 alone");
   nir_foreach_function_impl(impl, alpha) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex)
               check(nir_instr_as_tex(instr)->component == 0, "an alpha gather gathers plane 1's red");
         }
      }
   }
   const float bytes[4] = {0, 77 / 255.0f, 128 / 255.0f, 1};
   float got[4];
   joined_at(alpha, bytes, bytes, got);
   check(got[0] == 0 && got[1] == half_of(77 / 255.0) && got[2] == half_of(128 / 255.0) && got[3] == 1,
         "an alpha gather's four bytes over 255");
   ralloc_free(alpha);

   nir_shader *green = program(nir_texop_tg4, JOINED, 32, 1);
   switch_nvk_xr_planes(green, 1ull << JOINED);
   const struct reads rg = reads_of(green);
   check(rg.plane0 == 1 && rg.plane1 == 0, "a colour gather reads plane 0 alone");
   const float codes[4] = {384 / 1023.0f, 894 / 1023.0f, 0, 1};
   joined_at(green, codes, codes, got);
   check(got[0] == 0 && got[1] == 1 && got[2] == half_of(-384 / 510.0) && got[3] == half_of(639 / 510.0),
         "a colour gather's four codes");
   ralloc_free(green);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   test_reads();
   test_values();
   test_gathers();
   glsl_type_singleton_decref();
   if (failures)
      return 1;
   puts("joined samples, fetches and gathers of both planes and their values passed");
   return 0;
}
