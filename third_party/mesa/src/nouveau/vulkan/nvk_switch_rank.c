// SPDX-License-Identifier: MIT
#include "nvk_switch_compile.h"
#include "nvk_switch_offline.h"
#include "nvk_shader.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

SWITCH_EXPORT bool
switch_nvk_stage_metrics(const struct switch_nvk_stage *stage,
                         struct switch_nvk_stage_metrics *out, size_t bytes)
{
   if (!stage || !stage->owner || !out || bytes != sizeof(*out))
      return false;
   const struct nvk_shader *shader = container_of(stage->owner, struct nvk_shader, vk);
   const struct nak_shader_info *info = &shader->info;
   *out = (struct switch_nvk_stage_metrics) {
      .version = 1,
      .code_bytes = stage->code_bytes,
      .instructions = info->num_instrs,
      .gprs = info->num_gprs,
      .max_warps = info->max_warps_per_sm,
      .scratch_bytes = info->slm_size,
      .memory_spills = info->num_spills_to_mem,
      .memory_fills = info->num_fills_from_mem,
      .register_spills = info->num_spills_to_reg,
      .register_fills = info->num_fills_from_reg,
      .static_cycles = info->num_static_cycles,
   };
   return true;
}

static bool
dominates(const struct switch_nvk_stage_metrics *a,
          const struct switch_nvk_stage_metrics *b)
{
   return a->code_bytes <= b->code_bytes && a->instructions <= b->instructions &&
      a->gprs <= b->gprs && a->max_warps >= b->max_warps && a->scratch_bytes <= b->scratch_bytes &&
      a->memory_spills <= b->memory_spills && a->memory_fills <= b->memory_fills &&
      a->register_spills <= b->register_spills && a->register_fills <= b->register_fills &&
      a->static_cycles <= b->static_cycles &&
      (a->code_bytes < b->code_bytes || a->instructions < b->instructions || a->gprs < b->gprs ||
       a->max_warps > b->max_warps || a->scratch_bytes < b->scratch_bytes ||
       a->memory_spills < b->memory_spills || a->memory_fills < b->memory_fills ||
       a->register_spills < b->register_spills || a->register_fills < b->register_fills ||
       a->static_cycles < b->static_cycles);
}

struct ranked_compiler {
   VkResult (*compile)(const void *ctx, const struct switch_nvk_stage_info *info,
                       struct switch_nvk_stage *out);
   void (*finish)(const void *ctx, struct switch_nvk_stage *stage);
   const void *ctx;
};

/* The stage compiled from its native program and from its portable SPIR-V:
 * the portable one where it dominates on every metric
 */
static VkResult
ranked(const struct ranked_compiler *c, const struct switch_nvk_stage_info *info,
       struct switch_nvk_stage *out)
{
   VkResult result = c->compile(c->ctx, info, out);
   if (result != VK_SUCCESS || !out->semantic_version)
      return result;
   struct switch_nvk_stage_info portable_info = *info;
   portable_info.semantic = NULL;
   if ((!portable_info.spirv || !portable_info.spirv_bytes) &&
       (!info->portable || !info->portable(info->portable_context,
                                          &portable_info.spirv, &portable_info.spirv_bytes)))
      return VK_SUCCESS;
   struct switch_nvk_stage portable = {0};
   result = c->compile(c->ctx, &portable_info, &portable);
   if (result != VK_SUCCESS)
      return VK_SUCCESS;
   struct switch_nvk_stage_metrics native_cost, portable_cost;
   if (switch_nvk_stage_metrics(out, &native_cost, sizeof(native_cost)) &&
       switch_nvk_stage_metrics(&portable, &portable_cost, sizeof(portable_cost)) &&
       dominates(&portable_cost, &native_cost)) {
      c->finish(c->ctx, out);
      *out = portable;
   } else {
      c->finish(c->ctx, &portable);
   }
   return VK_SUCCESS;
}

static VkResult
device_compile(const void *ctx, const struct switch_nvk_stage_info *info,
               struct switch_nvk_stage *out)
{
   return switch_nvk_compile_stage((VkDevice)ctx, info, out);
}

static void
device_finish(const void *ctx, struct switch_nvk_stage *stage)
{
   switch_nvk_stage_finish((VkDevice)ctx, stage);
}

SWITCH_EXPORT VkResult
switch_nvk_compile_ranked_stage(VkDevice device,
                                const struct switch_nvk_stage_info *info,
                                struct switch_nvk_stage *out)
{
   const struct ranked_compiler c = {device_compile, device_finish, device};
   return ranked(&c, info, out);
}

struct offline_ctx {
   const struct switch_nak_frozen *frozen;
   const struct switch_nak_layout *layout;
};

static VkResult
offline_compile(const void *ctx, const struct switch_nvk_stage_info *info,
                struct switch_nvk_stage *out)
{
   const struct offline_ctx *o = ctx;
   return switch_nak_compile_stage(o->frozen, info, o->layout, out);
}

static void
offline_finish(UNUSED const void *ctx, struct switch_nvk_stage *stage)
{
   switch_nak_stage_finish(stage);
}

SWITCH_EXPORT VkResult
switch_nak_compile_ranked_stage(const struct switch_nak_frozen *frozen,
                                const struct switch_nvk_stage_info *info,
                                const struct switch_nak_layout *layout,
                                struct switch_nvk_stage *out)
{
   const struct offline_ctx o = {frozen, layout};
   const struct ranked_compiler c = {offline_compile, offline_finish, &o};
   return ranked(&c, info, out);
}
