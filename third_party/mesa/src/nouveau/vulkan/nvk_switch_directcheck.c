/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the host twin's proof of switch_nvk_compile_stage, macOS only. With
 * SWITCH_NVK_DIRECTCHECK set, every stage a graphics pipeline creation
 * compiles is compiled again by switch_nvk_compile_stage from the inputs the
 * raw path would give it (the SPIR-V, entry, specialization, layout, flags,
 * chains and sample shading of the creation) and compared with the stage the
 * pipeline holds: header, code, shader data, constant buffers, register count
 * and fragment bits. SWITCH_NVK_DIRECTCHECK_PROGRAMS names a programs.bin
 * (raw/pack/mkprograms.py) each stage must also be in. One line per stage that
 * differs and a total at exit, on stderr.
 */

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX

#include "nvk_switch_compile.h"

#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"

#include "vk_common_entrypoints.h"
#include "vk_pipeline.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include "util/simple_mtx.h"
#include "util/u_debug.h"

#include <stdio.h>
#include <stdlib.h>

static struct {
   simple_mtx_t lock;
   bool on;
   uint8_t *programs;
   size_t programs_bytes;
   uint32_t library, full, differ, missing, failed;
} check = {.lock = SIMPLE_MTX_INITIALIZER};

static void
report(void)
{
   fprintf(stderr,
           "switch directcheck: %u library stages, %u in full pipelines, "
           "%u differ, %u not in programs.bin, %u failed to compile\n",
           check.library, check.full, check.differ, check.missing,
           check.failed);
}

static void
load_programs(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return;
   fseek(f, 0, SEEK_END);
   const long bytes = ftell(f);
   fseek(f, 0, SEEK_SET);
   check.programs = malloc(bytes);
   if (check.programs != NULL &&
       fread(check.programs, 1, bytes, f) == (size_t)bytes)
      check.programs_bytes = bytes;
   fclose(f);
}

static void
start(void)
{
   static bool started;
   if (started)
      return;
   started = true;
   check.on = getenv("SWITCH_NVK_DIRECTCHECK") != NULL;
   if (!check.on)
      return;
   const char *programs = getenv("SWITCH_NVK_DIRECTCHECK_PROGRAMS");
   if (programs != NULL)
      load_programs(programs);
   atexit(report);
}

/* Whether programs.bin holds the stage: its records are "NVPG", the code,
 * cbuf type and data byte counts, the cbuf types, the 80-byte header, the
 * code and the data.
 */
static bool
in_programs(const struct switch_nvk_stage *s)
{
   size_t at = 0;
   while (at + 16 <= check.programs_bytes) {
      uint32_t counts[3];
      memcpy(counts, check.programs + at + 4, sizeof(counts));
      const uint8_t *header = check.programs + at + 16 + counts[1];
      const size_t next =
         at + 16 + counts[1] + sizeof(s->header) + counts[0] + counts[2];
      if (next > check.programs_bytes)
         return false;
      if (counts[0] == s->code_bytes && counts[2] == s->data_bytes &&
          memcmp(header, s->header, sizeof(s->header)) == 0 &&
          memcmp(header + sizeof(s->header), s->code, s->code_bytes) == 0 &&
          memcmp(header + sizeof(s->header) + s->code_bytes, s->data,
                 s->data_bytes) == 0)
         return true;
      at = next;
   }
   return false;
}

static const char *
difference(const struct switch_nvk_stage *a, const struct switch_nvk_stage *b)
{
   if (memcmp(a->header, b->header, sizeof(a->header)) != 0)
      return "header";
   if (a->code_bytes != b->code_bytes ||
       memcmp(a->code, b->code, a->code_bytes) != 0)
      return "code";
   if (a->data_bytes != b->data_bytes ||
       memcmp(a->data, b->data, a->data_bytes) != 0)
      return "data";
   if (a->cbuf_count != b->cbuf_count ||
       memcmp(a->cbuf_types, b->cbuf_types, a->cbuf_count) != 0)
      return "constant buffers";
   if (a->num_gprs != b->num_gprs ||
       a->early_fragment_tests != b->early_fragment_tests ||
       a->writes_depth != b->writes_depth ||
       a->post_depth_coverage != b->post_depth_coverage ||
       a->interlock_control != b->interlock_control)
      return "program info";
   return NULL;
}

static void
check_stage(VkDevice device, const VkGraphicsPipelineCreateInfo *info,
            const VkPipelineShaderStageCreateInfo *stage, VkPipeline pipeline)
{
   const struct vk_shader *held = vk_graphics_pipeline_stage_shader(
      pipeline, vk_to_mesa_shader_stage(stage->stage));
   if (held == NULL)
      return;

   VK_FROM_HANDLE(vk_shader_module, module, stage->module);
   const VkShaderModuleCreateInfo *inline_module =
      vk_find_struct_const(stage->pNext, SHADER_MODULE_CREATE_INFO);
   if ((module == NULL && inline_module == NULL) ||
       (module != NULL && module->nir != NULL))
      return;
   const struct switch_nvk_stage_info direct = {
      .spirv = module != NULL ? (const uint32_t *)module->data
                              : inline_module->pCode,
      .spirv_bytes = module != NULL ? module->size : inline_module->codeSize,
      .entry = stage->pName,
      .stage = stage->stage,
      .specialization = stage->pSpecializationInfo,
      .layout = info->layout,
      .pipeline_flags = vk_graphics_pipeline_create_flags(info),
      .pipeline_pnext = info->pNext,
      .stage_pnext = stage->pNext,
      .sample_shading = info->pMultisampleState != NULL &&
                        info->pMultisampleState->sampleShadingEnable,
   };
   const bool library =
      (direct.pipeline_flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) != 0;

   struct switch_nvk_stage made, want = {0};
   const VkResult result = switch_nvk_compile_stage(device, &direct, &made);
   switch_nvk_stage_of(container_of(held, const struct nvk_shader, vk), &want);

   const char *differs =
      result == VK_SUCCESS ? difference(&made, &want) : "compile";
   const bool missing = result == VK_SUCCESS && check.programs_bytes != 0 &&
                        !in_programs(&made);

   simple_mtx_lock(&check.lock);
   if (library)
      check.library++;
   else
      check.full++;
   check.failed += result != VK_SUCCESS;
   check.differ += differs != NULL && result == VK_SUCCESS;
   check.missing += missing;
   if (differs != NULL || missing)
      fprintf(stderr,
              "switch directcheck: %s %s stage %s: %s%s (code %u, held %u)\n",
              library ? "library" : "full pipeline",
              stage->pName, _mesa_shader_stage_to_abbrev(
                 vk_to_mesa_shader_stage(stage->stage)),
              differs != NULL ? differs : "same",
              missing ? ", not in programs.bin" : "", made.code_bytes,
              want.code_bytes);
   simple_mtx_unlock(&check.lock);

   if (result == VK_SUCCESS)
      switch_nvk_stage_finish(device, &made);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                            uint32_t count,
                            const VkGraphicsPipelineCreateInfo *infos,
                            const VkAllocationCallbacks *alloc,
                            VkPipeline *pipelines)
{
   const VkResult result = vk_common_CreateGraphicsPipelines(
      device, cache, count, infos, alloc, pipelines);
   simple_mtx_lock(&check.lock);
   start();
   const bool on = check.on;
   simple_mtx_unlock(&check.lock);
   if (!on)
      return result;
   for (uint32_t i = 0; i < count; i++) {
      if (pipelines[i] == VK_NULL_HANDLE)
         continue;
      for (uint32_t s = 0; s < infos[i].stageCount; s++)
         check_stage(device, &infos[i], &infos[i].pStages[s], pipelines[i]);
   }
   return result;
}

#endif /* defined(__APPLE__) && TARGET_OS_OSX */
