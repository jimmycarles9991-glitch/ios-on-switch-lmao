// SPDX-License-Identifier: MIT
#include "nvk_switch_compile.h"
#include "nvk_switch_stage.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

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
   const struct switch_nvk_ranked_compiler c = {device_compile, device_finish, device};
   return switch_nvk_ranked(&c, info, out);
}
