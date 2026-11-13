/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: what the raw GM20B path (TegraGM20BMetal raw/runtime/raw_capture.h)
 * reads of a command buffer NVK recorded and nobody submits, to make a render
 * state's raw record from NVK's own emission: its push segments, the CPU's
 * view of the device memory they point into, the programs its draws bind, and
 * the device addresses NVK's context init set. The plugin finds these with
 * dlsym.
 */

#include "nvk_switch_capture.h"

#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_table.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_queue.h"
#include "nvk_shader.h"
#include "nvkmd/nvkmd.h"
#include "util/hash_table.h"
#include "util/simple_mtx.h"

#include "clc397.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

/* The push segments in the order nvk_queue_exec_cmd_buffer submits them. A
 * segment without a CPU mapping (nvk_cmd_buffer_push_indirect's) comes with
 * cpu NULL.
 */
SWITCH_EXPORT void
switch_nvk_capture_cmd_buffer(uint64_t command_buffer,
                              void (*segment)(void *ctx, uint64_t addr,
                                              const uint32_t *cpu,
                                              uint32_t dwords),
                              void *ctx)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd,
                  (VkCommandBuffer)(uintptr_t)command_buffer);

   util_dynarray_foreach(&cmd->pushes, struct nvk_cmd_push, push) {
      if (push->range == 0)
         continue;
      segment(ctx, push->addr, push->map, push->range / 4);
   }
}

/* An arena's CPU view of [addr, addr + bytes): within one page, so within one
 * of its memory objects, which it maps apart.
 */
static const void *
arena_map(const struct nvk_mem_arena *arena, uint64_t addr, uint64_t bytes)
{
   if (!nvk_mem_arena_is_contiguous(arena) || !nvk_mem_arena_is_mapped(arena) ||
       bytes == 0 || (addr >> 12) != ((addr + bytes - 1) >> 12))
      return NULL;

   const uint64_t base = nvk_contiguous_mem_arena_base_address(arena);
   if (addr < base || addr + bytes > base + nvk_mem_arena_size_B(arena))
      return NULL;
   return nvk_mem_arena_map(arena, addr, bytes);
}

/* The CPU's view of [addr, addr + bytes) of the command buffer's device's
 * memory, while it lives, or NULL: one memory object's mapped range, or a
 * page of the shader heap's or the descriptor tables' arenas (whose memory is
 * bound into a range of their own).
 */
SWITCH_EXPORT const void *
switch_nvk_capture_map(uint64_t command_buffer, uint64_t addr, uint64_t bytes)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd,
                  (VkCommandBuffer)(uintptr_t)command_buffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   uint64_t offset = 0;
   struct nvkmd_mem *mem = nvkmd_dev_lookup_mem_by_va(dev->nvkmd, addr, &offset);
   if (mem != NULL) {
      /* vkMapMemory maps into client_map, not map: the metal2vulkan
       * runtime's buffers and upload arenas are mapped there; a host import
       * is the caller's pages, mapped by nobody here.
       */
      const char *base = mem->map != NULL ? mem->map : mem->client_map;
#ifdef NVK_HAS_DARWIN
      if (base == NULL)
         base = nvkmd_darwin_mem_host_ptr(mem);
#endif
      const void *cpu = NULL;
      if (base != NULL && bytes <= mem->va->size_B - offset)
         cpu = base + offset;
      nvkmd_mem_unref(mem);
      if (cpu != NULL)
         return cpu;
   }

   const struct nvk_mem_arena *arenas[] = {
      &dev->shader_heap.arena, &dev->images.arena, &dev->samplers.arena,
   };
   for (uint32_t a = 0; a < ARRAY_SIZE(arenas); a++) {
      const void *cpu = arena_map(arenas[a], addr, bytes);
      if (cpu != NULL)
         return cpu;
   }
   return NULL;
}

struct switch_nvk_programs {
   simple_mtx_t lock;
   struct hash_table_u64 *by_header;
};

void
switch_nvk_programs_init(struct nvk_device *dev)
{
   struct switch_nvk_programs *p = calloc(1, sizeof(*p));
   if (p == NULL)
      return;
   p->by_header = _mesa_hash_table_u64_create(NULL);
   if (p->by_header == NULL) {
      free(p);
      return;
   }
   simple_mtx_init(&p->lock, mtx_plain);
   dev->switch_programs = p;
}

void
switch_nvk_programs_finish(struct nvk_device *dev)
{
   struct switch_nvk_programs *p = dev->switch_programs;
   if (p == NULL)
      return;
   _mesa_hash_table_u64_destroy(p->by_header);
   simple_mtx_destroy(&p->lock);
   free(p);
   dev->switch_programs = NULL;
}

void
switch_nvk_program_uploaded(struct nvk_device *dev,
                            const struct nvk_shader *shader)
{
   struct switch_nvk_programs *p = dev->switch_programs;
   if (p == NULL || shader->info.stage == MESA_SHADER_COMPUTE)
      return;
   simple_mtx_lock(&p->lock);
   _mesa_hash_table_u64_insert(p->by_header, shader->hdr_addr, (void *)shader);
   simple_mtx_unlock(&p->lock);
}

void
switch_nvk_program_freed(struct nvk_device *dev,
                         const struct nvk_shader *shader)
{
   struct switch_nvk_programs *p = dev->switch_programs;
   if (p == NULL)
      return;
   simple_mtx_lock(&p->lock);
   if (_mesa_hash_table_u64_search(p->by_header, shader->hdr_addr) == shader)
      _mesa_hash_table_u64_remove(p->by_header, shader->hdr_addr);
   simple_mtx_unlock(&p->lock);
}

/* A constant buffer as the raw path's pack names it (raw_pack.h PackCbufType):
 * the nvk_cbuf_type, and a UBO descriptor's set-0 offset / 16 in bits 3-7 or
 * a descriptor set's number in bits 3-7 (set 0's code is the bare type); a
 * UBO elsewhere is 7, which no raw draw binds.
 */
uint8_t
switch_nvk_cbuf_code(const struct nvk_cbuf *cbuf)
{
   if (cbuf->type == NVK_CBUF_TYPE_DESC_SET)
      return cbuf->desc_set < 32 ? NVK_CBUF_TYPE_DESC_SET | cbuf->desc_set << 3 : 7;
   if (cbuf->type != NVK_CBUF_TYPE_UBO_DESC)
      return cbuf->type;
   if (cbuf->desc_set != 0 || cbuf->desc_offset % 16 || cbuf->desc_offset >= 512)
      return 7;
   return NVK_CBUF_TYPE_UBO_DESC | (cbuf->desc_offset / 16) << 3;
}

/* The graphics program uploaded at `program`, a SET_PIPELINE_PROGRAM offset
 * (pre-Volta: from the shader heap's base): its shader header, code and
 * shader data as uploaded, and its constant buffers' switch_nvk_cbuf_code. The
 * pointers live as long as the shader (the pipelines that hold it).
 */
SWITCH_EXPORT bool
switch_nvk_capture_program(uint64_t command_buffer, uint32_t program,
                           const void **header, uint32_t *header_bytes,
                           const void **code, uint32_t *code_bytes,
                           const void **data, uint32_t *data_bytes,
                           uint8_t cbuf_types[16], uint32_t *cbuf_count)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd,
                  (VkCommandBuffer)(uintptr_t)command_buffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   struct switch_nvk_programs *p = dev->switch_programs;
   if (p == NULL || nvk_device_physical(dev)->info.cls_eng3d >= VOLTA_A)
      return false;

   simple_mtx_lock(&p->lock);
   const struct nvk_shader *shader =
      _mesa_hash_table_u64_search(p->by_header, program);
   if (shader != NULL) {
      *header = shader->info.hdr;
      *header_bytes = GF100_SHADER_HEADER_SIZE;
      *code = shader->code_ptr;
      *code_bytes = shader->code_size;
      *data = shader->data_ptr;
      *data_bytes = shader->data_size;
      *cbuf_count = MIN2(shader->cbuf_map.cbuf_count, 16);
      for (uint32_t c = 0; c < *cbuf_count; c++)
         cbuf_types[c] = switch_nvk_cbuf_code(&shader->cbuf_map.cbufs[c]);
   }
   simple_mtx_unlock(&p->lock);
   return shader != NULL;
}

/* What NVK's context init (nvk_push_draw_state_init and the queue's heap
 * binds) put in registers as this process's addresses, for the decoder to
 * start from: the shader heap's base (the program region), the zero page, the
 * queue's cb0 (the MME's root table), the texture and sampler header pools
 * and their highest indices. The queue is the first of the command buffer's
 * family, the one m2v submits to. False: no such queue.
 */
SWITCH_EXPORT bool
switch_nvk_capture_context(uint64_t command_buffer, uint64_t *shader_heap,
                           uint64_t *zero_page, uint64_t *cb0,
                           uint64_t *images, uint32_t *image_max,
                           uint64_t *samplers, uint32_t *sampler_max)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd,
                  (VkCommandBuffer)(uintptr_t)command_buffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_queue *queue = NULL;
   list_for_each_entry(struct vk_queue, q, &dev->vk.queues, link) {
      if (q->queue_family_index == cmd->vk.pool->queue_family_index) {
         queue = container_of(q, struct nvk_queue, vk);
         break;
      }
   }
   if (queue == NULL || queue->draw_cb0 == NULL)
      return false;

   const uint32_t image_count =
      nvk_descriptor_table_alloc_count(&dev->images);
   const uint32_t sampler_count =
      nvk_descriptor_table_alloc_count(&dev->samplers);
   *shader_heap = nvk_heap_contiguous_base_address(&dev->shader_heap);
   *zero_page = dev->zero_page->va->addr;
   *cb0 = queue->draw_cb0->va->addr;
   *images = nvk_descriptor_table_base_address(&dev->images);
   *image_max = image_count ? image_count - 1 : 0;
   *samplers = nvk_descriptor_table_base_address(&dev->samplers);
   *sampler_max = sampler_count ? sampler_count - 1 : 0;
   return true;
}
