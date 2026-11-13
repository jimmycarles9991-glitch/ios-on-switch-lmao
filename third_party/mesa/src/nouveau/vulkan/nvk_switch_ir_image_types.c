// SPDX-License-Identifier: MIT
#include "nvk_switch_ir_internal.h"

static bool
image_format(uint32_t spv, enum pipe_format *out)
{
   switch (spv) {
#define FORMAT(S, P) case SpvImageFormat##S: *out = PIPE_FORMAT_##P; return true
   FORMAT(Unknown, NONE);
   FORMAT(Rgba32f, R32G32B32A32_FLOAT);
   FORMAT(Rgba16f, R16G16B16A16_FLOAT);
   FORMAT(R32f, R32_FLOAT);
   FORMAT(Rgba8, R8G8B8A8_UNORM);
   FORMAT(Rgba8Snorm, R8G8B8A8_SNORM);
   FORMAT(Rg32f, R32G32_FLOAT);
   FORMAT(Rg16f, R16G16_FLOAT);
   FORMAT(R11fG11fB10f, R11G11B10_FLOAT);
   FORMAT(R16f, R16_FLOAT);
   FORMAT(Rgba16, R16G16B16A16_UNORM);
   FORMAT(Rgb10A2, R10G10B10A2_UNORM);
   FORMAT(Rg16, R16G16_UNORM);
   FORMAT(Rg8, R8G8_UNORM);
   FORMAT(R16, R16_UNORM);
   FORMAT(R8, R8_UNORM);
   FORMAT(Rgba16Snorm, R16G16B16A16_SNORM);
   FORMAT(Rg16Snorm, R16G16_SNORM);
   FORMAT(Rg8Snorm, R8G8_SNORM);
   FORMAT(R16Snorm, R16_SNORM);
   FORMAT(R8Snorm, R8_SNORM);
   FORMAT(Rgba32i, R32G32B32A32_SINT);
   FORMAT(Rgba16i, R16G16B16A16_SINT);
   FORMAT(Rgba8i, R8G8B8A8_SINT);
   FORMAT(R32i, R32_SINT);
   FORMAT(Rg32i, R32G32_SINT);
   FORMAT(Rg16i, R16G16_SINT);
   FORMAT(Rg8i, R8G8_SINT);
   FORMAT(R16i, R16_SINT);
   FORMAT(R8i, R8_SINT);
   FORMAT(Rgba32ui, R32G32B32A32_UINT);
   FORMAT(Rgba16ui, R16G16B16A16_UINT);
   FORMAT(Rgba8ui, R8G8B8A8_UINT);
   FORMAT(R32ui, R32_UINT);
   FORMAT(Rgb10a2ui, R10G10B10A2_UINT);
   FORMAT(Rg32ui, R32G32_UINT);
   FORMAT(Rg16ui, R16G16_UINT);
   FORMAT(Rg8ui, R8G8_UINT);
   FORMAT(R16ui, R16_UINT);
   FORMAT(R8ui, R8_UINT);
   FORMAT(R64ui, R64_UINT);
   FORMAT(R64i, R64_SINT);
#undef FORMAT
   default: return false;
   }
}

bool
switch_nvk_ir_image_type(struct switch_nvk_ir_builder *s,
                         const struct switch_nvk_ir_record *r)
{
   if (r->result >= s->program->types || r->flags || r->auxiliary)
      return false;
   struct switch_nvk_ir_type *out = &s->types[r->result];
   const uint32_t *a = switch_nvk_ir_args(s, r);
   out->resource = r->opcode;
   if (r->opcode == SpvOpTypeSampler) {
      if (r->count) return false;
      out->type = glsl_bare_sampler_type();
      return true;
   }
   if (r->opcode == SpvOpTypeSampledImage) {
      if (r->count != 1 || a[0] >= s->program->types ||
          !s->types[a[0]].type || !glsl_type_is_texture(s->types[a[0]].type))
         return false;
      out->type = glsl_texture_type_to_sampler(s->types[a[0]].type, false);
      return out->type != NULL;
   }
   if (r->opcode != SpvOpTypeImage || r->count != 7 || a[0] >= s->program->types ||
       !s->types[a[0]].type || !glsl_type_is_scalar(s->types[a[0]].type) ||
       glsl_get_bit_size(s->types[a[0]].type) != 32 || a[2] > 1 || a[3] > 1 || a[4] > 1 ||
       (a[5] != 1 && a[5] != 2) || !image_format(a[6], &out->image_format))
      return false;
   enum glsl_base_type base = glsl_get_base_type(s->types[a[0]].type);
   if (base != GLSL_TYPE_FLOAT && base != GLSL_TYPE_INT && base != GLSL_TYPE_UINT)
      return false;
   enum glsl_sampler_dim dim;
   switch (a[1]) {
   case SpvDim1D: dim = GLSL_SAMPLER_DIM_1D; break;
   case SpvDim2D: dim = a[4] ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D; break;
   case SpvDim3D: dim = GLSL_SAMPLER_DIM_3D; break;
   case SpvDimCube: dim = GLSL_SAMPLER_DIM_CUBE; break;
   case SpvDimBuffer: dim = GLSL_SAMPLER_DIM_BUF; break;
   default: return false;
   }
   if ((a[4] && a[1] != SpvDim2D) ||
       (a[3] && a[1] != SpvDim1D && a[1] != SpvDim2D && a[1] != SpvDimCube) ||
       (a[5] == 1 && a[6] != SpvImageFormatUnknown) || (a[5] == 2 && a[2]) ||
       a[6] == SpvImageFormatR64i || a[6] == SpvImageFormatR64ui)
      return false;
   out->type = a[5] == 1 ? glsl_texture_type(dim, a[3], base) : glsl_image_type(dim, a[3], base);
   return out->type != NULL;
}
