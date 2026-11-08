/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_queue.h"

#include "nvk_buffer.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"
#include "nvk_entrypoints.h"
#include "nvk_rust.h"
#include "vk_common_entrypoints.h"
#include "vk_semaphore.h"
#include "nv_push.h"

#include "nv_push_cl9039.h"
#include "nv_push_cl9097.h"
#include "nv_push_cl90b5.h"
#include "nv_push_cla0c0.h"
#include "cla1c0.h"
#include "clb197.h"
#include "nv_push_clc3c0.h"
#include "nv_push_clc397.h"

static VkResult
nvk_queue_push(struct nvk_queue *queue, const struct nv_push *push);

static VkResult
nvk_queue_init_context_state(struct nvk_queue *queue);

static void
nvk_queue_heap_state_init(struct nvk_queue_heap_state *qhs)
{
   memset(qhs, 0, sizeof(*qhs));
}

static void
nvk_queue_heap_state_finish(struct nvk_device *dev,
                            struct nvk_queue_heap_state *qhs)
{ }

static VkResult
nvk_queue_heap_state_bind_image_heap(struct nvk_queue *queue,
                                     struct nvk_queue_heap_state *qhs,
                                     uint64_t image_heap_addr,
                                     uint32_t image_count)
{
   /* We can't actually set a size of 0 but we can set a NULL address */
   if (image_heap_addr == 0 || image_count == 0) {
      image_heap_addr = 0;
      image_count = 1;
   }

   /* If the heap is already bound, there's nothing to do */
   if (qhs->image_heap_addr == image_heap_addr &&
       qhs->image_count == image_count)
      return VK_SUCCESS;

   qhs->image_heap_addr = image_heap_addr;
   qhs->image_count = image_count;

   uint32_t push_data[16];
   struct nv_push push;
   nv_push_init(&push, push_data, 16,
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

   if (queue->engines & NVKMD_ENGINE_COMPUTE) {
      P_MTHD(p, NVA0C0, SET_TEX_HEADER_POOL_A);
      P_NVA0C0_SET_TEX_HEADER_POOL_A(p, image_heap_addr >> 32);
      P_NVA0C0_SET_TEX_HEADER_POOL_B(p, image_heap_addr);
      P_NVA0C0_SET_TEX_HEADER_POOL_C(p, image_count - 1);
      P_IMMD(p, NVA0C0, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   if (queue->engines & NVKMD_ENGINE_3D) {
      P_MTHD(p, NV9097, SET_TEX_HEADER_POOL_A);
      P_NV9097_SET_TEX_HEADER_POOL_A(p, image_heap_addr >> 32);
      P_NV9097_SET_TEX_HEADER_POOL_B(p, image_heap_addr);
      P_NV9097_SET_TEX_HEADER_POOL_C(p, image_count - 1);
      P_IMMD(p, NV9097, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   return nvk_queue_push(queue, p);
}

static VkResult
nvk_queue_heap_state_bind_sampler_heap(struct nvk_queue *queue,
                                       struct nvk_queue_heap_state *qhs,
                                       uint64_t sampler_heap_addr,
                                       uint32_t sampler_count)
{
   /* We can't actually set a size of 0 but we can set a NULL address */
   if (sampler_heap_addr == 0 || sampler_count == 0) {
      sampler_heap_addr = 0;
      sampler_count = 1;
   }

   /* If the heap is already bound, there's nothing to do */
   if (qhs->sampler_heap_addr == sampler_heap_addr &&
       qhs->sampler_count == sampler_count)
      return VK_SUCCESS;

   qhs->sampler_heap_addr = sampler_heap_addr;
   qhs->sampler_count = sampler_count;

   uint32_t push_data[16];
   struct nv_push push;
   nv_push_init(&push, push_data, 16,
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

   if (queue->engines & NVKMD_ENGINE_COMPUTE) {
      P_MTHD(p, NVA0C0, SET_TEX_SAMPLER_POOL_A);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_A(p, sampler_heap_addr >> 32);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_B(p, sampler_heap_addr);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_C(p, sampler_count - 1);
      P_IMMD(p, NVA0C0, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   if (queue->engines & NVKMD_ENGINE_3D) {
      P_MTHD(p, NV9097, SET_TEX_SAMPLER_POOL_A);
      P_NV9097_SET_TEX_SAMPLER_POOL_A(p, sampler_heap_addr >> 32);
      P_NV9097_SET_TEX_SAMPLER_POOL_B(p, sampler_heap_addr);
      P_NV9097_SET_TEX_SAMPLER_POOL_C(p, sampler_count - 1);
      P_IMMD(p, NV9097, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   return nvk_queue_push(queue, p);
}

static VkResult
nvk_queue_heap_state_bind_dev_heaps(struct nvk_queue *queue)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   VkResult result;

   const uint64_t image_heap_addr =
      nvk_descriptor_table_base_address(&dev->images);
   const uint32_t image_count =
      nvk_descriptor_table_alloc_count(&dev->images);

   const uint64_t sampler_heap_addr =
      nvk_descriptor_table_base_address(&dev->samplers);
   const uint32_t sampler_count =
      nvk_descriptor_table_alloc_count(&dev->samplers);

   result = nvk_queue_heap_state_bind_image_heap(queue,
                                                 &queue->heap_state,
                                                 image_heap_addr,
                                                 image_count);
   if (result != VK_SUCCESS)
      return result;

   result = nvk_queue_heap_state_bind_sampler_heap(queue,
                                                   &queue->heap_state,
                                                   sampler_heap_addr,
                                                   sampler_count);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

static void
nvk_queue_slm_state_init(struct nvk_queue_slm_state *qss)
{
   memset(qss, 0, sizeof(*qss));

   /* Set something bogus to force it to flush on the first set */
   qss->bytes_per_warp = -1;
   qss->bytes_per_tpc = -1;
}

static void
nvk_queue_slm_state_finish(struct nvk_device *dev,
                           struct nvk_queue_slm_state *qss)
{
   if (qss->mem)
      nvkmd_mem_unref(qss->mem);
}

static VkResult
nvk_queue_slm_state_update(struct nvk_queue *queue,
                           struct nvk_queue_slm_state *qss)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   struct nvkmd_mem *mem;
   uint32_t bytes_per_warp, bytes_per_tpc;
   mem = nvk_slm_area_get_mem_ref(&dev->slm, &bytes_per_warp, &bytes_per_tpc);
   if (qss->mem == mem &&
       qss->bytes_per_warp == bytes_per_warp &&
       qss->bytes_per_tpc == bytes_per_tpc) {
      /* No change */
      if (mem)
         nvkmd_mem_unref(mem);
      return VK_SUCCESS;
   }

   if (qss->mem)
      nvkmd_mem_unref(qss->mem);
   qss->mem = mem;
   qss->bytes_per_warp = bytes_per_warp;
   qss->bytes_per_tpc = bytes_per_tpc;

   uint32_t push_data[64];
   struct nv_push push;
   nv_push_init(&push, push_data, 64,
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

   if (qss->mem) {
      const uint64_t slm_addr = qss->mem->va->addr;
      const uint64_t slm_size = qss->mem->size_B;
      const uint64_t slm_per_warp = qss->bytes_per_warp;
      const uint64_t slm_per_tpc = qss->bytes_per_tpc;
      assert(!(slm_per_tpc & 0x7fff));

      if (queue->engines & NVKMD_ENGINE_COMPUTE) {
         P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_A);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);

         P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A(p, slm_per_tpc >> 32);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_B(p, slm_per_tpc);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_C(p, 0xff);

         if (pdev->info.cls_compute < VOLTA_COMPUTE_A) {
            P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_THROTTLED_A);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A(p, slm_per_tpc >> 32);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_B(p, slm_per_tpc);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C(p, 0xff);
         }
      }

      if (queue->engines & NVKMD_ENGINE_3D) {
         P_MTHD(p, NV9097, SET_SHADER_LOCAL_MEMORY_A);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_C(p, slm_size >> 32);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_D(p, slm_size);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_E(p, slm_per_warp);
      }
   }

   return nvk_queue_push(queue, p);
}

static VkResult
nvk_queue_submit_bind(struct nvk_queue *queue,
                      struct vk_queue_submit *submit)
{
   VkResult result;

   result = nvkmd_ctx_wait(queue->bind_ctx, &queue->vk.base,
                           submit->wait_count, submit->waits);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < submit->buffer_bind_count; i++) {
      result = nvk_queue_buffer_bind(queue, &submit->buffer_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submit->image_bind_count; i++) {
      result = nvk_queue_image_bind(queue, &submit->image_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submit->image_opaque_bind_count; i++) {
      result = nvk_queue_image_opaque_bind(queue, &submit->image_opaque_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   result = nvkmd_ctx_signal(queue->bind_ctx, &queue->vk.base,
                             submit->signal_count, submit->signals);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

static VkResult
nvk_queue_exec_cmd_buffer(struct nvk_queue *queue,
                          struct nvk_cmd_buffer *cmd)
{
   VkResult result = VK_SUCCESS;

   const uint32_t max_execs =
      util_dynarray_num_elements(&cmd->pushes, struct nvk_cmd_push);
   STACK_ARRAY(struct nvkmd_ctx_exec, execs, max_execs);
   uint32_t exec_count = 0;

   util_dynarray_foreach(&cmd->pushes, struct nvk_cmd_push, push) {
      if (push->range == 0)
         continue;

      execs[exec_count++] = (struct nvkmd_ctx_exec) {
         .addr = push->addr,
         .size_B = push->range,
         .incomplete = push->incomplete,
         .no_prefetch = push->no_prefetch,
      };
   }

   result = nvkmd_ctx_exec(queue->exec_ctx, &queue->vk.base,
                           exec_count, execs);

   STACK_ARRAY_FINISH(execs);
   return result;
}

static VkResult
nvk_queue_submit_exec(struct nvk_queue *queue,
                      struct vk_queue_submit *submit)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   VkResult result;

   if (submit->command_buffer_count > 0) {
      nvk_descriptor_table_flush_map(dev, &dev->images);
      nvk_descriptor_table_flush_map(dev, &dev->samplers);
      nvk_heap_flush_maps(dev, &dev->shader_heap);
      assert(dev->event_heap.arena.mem_flags & NVKMD_MEM_COHERENT);

      result = nvk_queue_heap_state_bind_dev_heaps(queue);
      if (result != VK_SUCCESS)
         return result;

      result = nvk_queue_slm_state_update(queue, &queue->slm_state);
      if (result != VK_SUCCESS)
         return result;

      uint64_t upload_time_point;
      result = nvk_upload_queue_flush(dev, &dev->upload, &upload_time_point);
      if (result != VK_SUCCESS)
         return result;

      if (upload_time_point > 0) {
         struct vk_sync_wait wait = {
            .sync = dev->upload.stream.sync,
            .stage_mask = ~0,
            .wait_value = upload_time_point,
         };
         result = nvkmd_ctx_wait(queue->exec_ctx, &queue->vk.base, 1, &wait);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   result = nvkmd_ctx_wait(queue->exec_ctx, &queue->vk.base,
                           submit->wait_count, submit->waits);
   if (result != VK_SUCCESS)
      return result;

   for (unsigned i = 0; i < submit->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd =
         container_of(submit->command_buffers[i], struct nvk_cmd_buffer, vk);
      result = nvk_queue_exec_cmd_buffer(queue, cmd);
      if (result != VK_SUCCESS)
         return result;
   }

   result = nvkmd_ctx_signal(queue->exec_ctx, &queue->vk.base,
                             submit->signal_count, submit->signals);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

#ifdef NVK_HAS_DARWIN
/* ios-on-switch-lmao: the host memory the caller's next vkQueueSubmit on
 * `queue` from this thread reads and writes (access: 1 read, 2 write, 3 both),
 * so the kernel orders that work after other processes' GPU writes to it, on
 * the GPU (nvkmd.h nvkmd_darwin_hint_surfaces). The metal2vulkan runtime calls
 * it before each of its submissions (found with dlsym: the Vulkan loader opens
 * this library RTLD_LOCAL). A submission it named none for runs as before.
 */
__attribute__((visibility("default"))) void
switch_nvk_submit_surfaces(VkQueue _queue, const VkDeviceMemory *mems,
                           const uint8_t *access, uint32_t count)
{
   VK_FROM_HANDLE(vk_queue, vk_queue, _queue);
   nvkmd_darwin_hint_surfaces(container_of(vk_queue, struct nvk_queue, vk));
   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(nvk_device_memory, mem, mems[i]);
      if (mem != NULL && mem->mem != NULL)
         nvkmd_darwin_hint_surface(mem->mem, access[i]);
   }
}

/* The kernel killed the queue's channel (a GPU fault or hang): a new one, with
 * the context a new queue starts with. Only between vkQueueSubmit calls, so
 * a call's later work never runs on the new channel after its earlier work
 * failed on the old one.
 */
static VkResult
nvk_queue_reopen_if_lost(struct nvk_queue *queue)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   if (queue->exec_ctx == NULL || !nvkmd_darwin_ctx_lost(queue->exec_ctx))
      return VK_SUCCESS;

   VkResult result = nvkmd_darwin_ctx_reopen(queue->exec_ctx, &queue->vk.base);
   if (result != VK_SUCCESS)
      return vk_queue_set_lost(&queue->vk, "no channel after a channel kill");

   nvk_queue_heap_state_init(&queue->heap_state);
   nvk_queue_slm_state_finish(dev, &queue->slm_state);
   nvk_queue_slm_state_init(&queue->slm_state);
   /* Warps of the killed channel may have held interlock tickets: the new
    * context's ticket dispensers start at zero, and so must the buffer.
    */
   if (queue->fsi_memory != NULL) {
      result = nvk_upload_queue_fill(dev, &dev->upload,
                                     queue->fsi_memory->va->addr, 0,
                                     queue->fsi_memory->size_B);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk,
                                  "interlock reset after a channel kill");
   }
   result = nvk_queue_init_context_state(queue);
   if (result != VK_SUCCESS)
      return vk_queue_set_lost(&queue->vk,
                               "context setup after a channel kill");
   return VK_SUCCESS;
}

/* What the last vkQueueSubmit2 signalling `semaphore` at `value` came to once
 * the semaphore reached it: 0, or the kernel's error (TegraGM20BABI.h method
 * 11) when the kernel killed the channel with that work in flight. Metal's
 * command buffer fails with it, and the queue goes on.
 */
__attribute__((visibility("default"))) uint32_t
switch_nvk_semaphore_failure(VkSemaphore _semaphore, uint64_t value)
{
   VK_FROM_HANDLE(vk_semaphore, semaphore, _semaphore);
   if (semaphore == NULL)
      return 0;
   return nvkmd_darwin_sync_failure(vk_semaphore_get_active_sync(semaphore),
                                    value);
}

/* One vkQueueSubmit can make several driver submissions (Mesa merges its
 * VkSubmitInfos only where no signal comes between): the hint is the call's,
 * for each of them, wherever it runs (nvk_queue_submit_data).
 */
VKAPI_ATTR VkResult VKAPI_CALL
nvk_QueueSubmit2(VkQueue _queue, uint32_t submitCount,
                 const VkSubmitInfo2 *pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(vk_queue, vk_queue, _queue);
   struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);
   VkResult reopened = nvk_queue_reopen_if_lost(queue);
   if (reopened != VK_SUCCESS)
      return reopened;
   nvkmd_darwin_submit_call(queue, true);
   VkResult result =
      vk_common_QueueSubmit2(_queue, submitCount, pSubmits, fence);
   nvkmd_darwin_submit_call(queue, false);
   return result;
}

static void *
nvk_queue_submit_data(struct vk_queue *vk_queue)
{
   return nvkmd_darwin_submit_data(container_of(vk_queue, struct nvk_queue, vk));
}
#endif

static VkResult
nvk_queue_submit(struct vk_queue *vk_queue,
                 struct vk_queue_submit *submit)
{
   struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);
   VkResult result;

   if (vk_queue_is_lost(&queue->vk))
      return VK_ERROR_DEVICE_LOST;

   if (submit->buffer_bind_count > 0 ||
       submit->image_bind_count > 0  ||
       submit->image_opaque_bind_count > 0) {
      assert(submit->command_buffer_count == 0);
      result = nvk_queue_submit_bind(queue, submit);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "Bind operation failed");
   } else {
#ifdef NVK_HAS_DARWIN
      nvkmd_darwin_submit_surfaces(queue->exec_ctx, submit->driver_data);
#endif
      result = nvk_queue_submit_exec(queue, submit);
#ifdef NVK_HAS_DARWIN
      nvkmd_darwin_submit_surfaces_end(queue->exec_ctx);
#endif
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "Submit failed");
   }

   return VK_SUCCESS;
}

static VkResult
nvk_queue_push(struct nvk_queue *queue, const struct nv_push *push)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   if (vk_queue_is_lost(&queue->vk))
      return VK_ERROR_DEVICE_LOST;

   if (nv_push_dw_count(push) == 0)
      return VK_SUCCESS;

   return nvk_mem_stream_push(dev, &queue->push_stream, queue->exec_ctx,
                              push->start, nv_push_dw_count(push), NULL);
}

static VkResult
nvk_queue_init_context_state(struct nvk_queue *queue)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkResult result;

   uint32_t push_data[4096 + 1024];
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data),
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

   /* M2MF state */
   if (pdev->info.cls_m2mf <= FERMI_MEMORY_TO_MEMORY_FORMAT_A) {
      /* we absolutely do not support Fermi, but if somebody wants to toy
       * around with it, this is a must
       */
      P_MTHD(p, NV9039, SET_OBJECT);
      P_NV9039_SET_OBJECT(p, {
         .class_id = pdev->info.cls_m2mf,
         .engine_id = 0,
      });
   }

   if (queue->engines & NVKMD_ENGINE_3D) {
      result = nvk_push_draw_state_init(queue, p);
      if (result != VK_SUCCESS)
         return result;
   }

   if (queue->engines & NVKMD_ENGINE_COMPUTE) {
      result = nvk_push_dispatch_state_init(queue, p);
      if (result != VK_SUCCESS)
         return result;
   }

   if (queue->engines & NVKMD_ENGINE_VDEC) {
      result = nvk_push_video_decode_state_init(queue, p);
      if (result != VK_SUCCESS)
         return result;
   }

   return nvk_queue_push(queue, &push);
}

static VkQueueGlobalPriority
get_queue_global_priority(const VkDeviceQueueCreateInfo *pCreateInfo)
{
   const VkDeviceQueueGlobalPriorityCreateInfo *priority_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO);
   if (priority_info == NULL)
      return VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;

   return priority_info->globalPriority;
}

VkResult
nvk_queue_create(struct nvk_device *dev,
                 const VkDeviceQueueCreateInfo *pCreateInfo,
                 uint32_t index_in_family)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkResult result;

   assert(pCreateInfo->queueFamilyIndex < pdev->queue_family_count);
   const struct nvk_queue_family *queue_family =
      &pdev->queue_families[pCreateInfo->queueFamilyIndex];

   const VkQueueGlobalPriority global_priority =
      get_queue_global_priority(pCreateInfo);

   /* From the Vulkan 1.3.295 spec:
    *
    *    "If the globalPriorityQuery feature is enabled and the requested
    *    global priority is not reported via
    *    VkQueueFamilyGlobalPriorityPropertiesKHR, the driver implementation
    *    must fail the queue creation. In this scenario,
    *    VK_ERROR_INITIALIZATION_FAILED is returned."
    */
   if (dev->vk.enabled_features.globalPriorityQuery &&
       global_priority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (global_priority > VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_NOT_PERMITTED;

   struct nvk_queue *queue = vk_zalloc(&dev->vk.alloc, sizeof(struct nvk_queue),
                                       8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queue)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = vk_queue_init(&queue->vk, &dev->vk, pCreateInfo, index_in_family);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   nvk_queue_heap_state_init(&queue->heap_state);
   nvk_queue_slm_state_init(&queue->slm_state);

   queue->engines =
      nvk_queue_engines_from_queue_flags(queue_family->queue_flags);

   if (queue_family->queue_flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
      queue->engines |= NVKMD_ENGINE_VDEC;

   if (queue->engines) {
      result = nvkmd_dev_create_ctx(dev->nvkmd, &dev->vk.base,
                                    queue->engines, &queue->exec_ctx);
      if (result != VK_SUCCESS)
         goto fail_init;

      result = nvkmd_dev_alloc_mem(dev->nvkmd, &dev->vk.base,
                                   4096, 0, NVKMD_MEM_LOCAL,
                                   &queue->draw_cb0);
      if (result != VK_SUCCESS)
         goto fail_exec_ctx;

      if (pdev->info.cls_eng3d >= MAXWELL_B &&
          (queue->engines & NVKMD_ENGINE_3D) &&
          nvk_device_fragment_interlock(dev)) {
         uint32_t interlock_buffer_size =
            pdev->info.gpc_count * 0x100 * sizeof(uint32_t);
         result = nvkmd_dev_alloc_mem(dev->nvkmd, &dev->vk.base,
                                      interlock_buffer_size, 0, NVKMD_MEM_LOCAL,
                                      &queue->fsi_memory);
         if (result != VK_SUCCESS)
            goto fail_draw_cb0;

         result = nvk_upload_queue_fill(dev, &dev->upload,
                                        queue->fsi_memory->va->addr, 0,
                                        queue->fsi_memory->size_B);
         if (result != VK_SUCCESS)
            goto fail_fsi;
      }
   }

   if (queue_family->queue_flags & VK_QUEUE_SPARSE_BINDING_BIT) {
      result = nvkmd_dev_create_ctx(dev->nvkmd, &dev->vk.base,
                                    NVKMD_ENGINE_BIND, &queue->bind_ctx);
      if (result != VK_SUCCESS)
         goto fail_fsi;
   }

   result = nvk_mem_stream_init(dev, &queue->push_stream);
   if (result != VK_SUCCESS)
      goto fail_bind_ctx;

   result = nvk_queue_init_context_state(queue);
   if (result != VK_SUCCESS)
      goto fail_push_stream;

   queue->vk.driver_submit = nvk_queue_submit;
#ifdef NVK_HAS_DARWIN
   queue->vk.driver_submit_data = nvk_queue_submit_data;
   queue->vk.driver_submit_data_free = nvkmd_darwin_submit_data_free;
#endif

   return VK_SUCCESS;

fail_push_stream:
   nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
   nvk_mem_stream_finish(dev, &queue->push_stream);
fail_bind_ctx:
   if (queue->bind_ctx != NULL)
      nvkmd_ctx_destroy(queue->bind_ctx);
fail_fsi:
   if (queue->fsi_memory != NULL)
      nvkmd_mem_unref(queue->fsi_memory);
fail_draw_cb0:
   if (queue->draw_cb0 != NULL)
      nvkmd_mem_unref(queue->draw_cb0);
fail_exec_ctx:
   if (queue->exec_ctx != NULL)
      nvkmd_ctx_destroy(queue->exec_ctx);
fail_init:
   nvk_queue_slm_state_finish(dev, &queue->slm_state);
   nvk_queue_heap_state_finish(dev, &queue->heap_state);
   vk_queue_finish(&queue->vk);
fail_alloc:
   vk_free(&dev->vk.alloc, queue);

   return result;
}

void
nvk_queue_destroy(struct nvk_device *dev, struct nvk_queue *queue)
{
   nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
   nvk_mem_stream_finish(dev, &queue->push_stream);
   if (queue->fsi_memory != NULL) {
      nvk_upload_queue_sync(dev, &dev->upload);
      nvkmd_mem_unref(queue->fsi_memory);
   }
   if (queue->draw_cb0 != NULL) {
      nvk_upload_queue_sync(dev, &dev->upload);
      nvkmd_mem_unref(queue->draw_cb0);
   }
   nvk_queue_slm_state_finish(dev, &queue->slm_state);
   nvk_queue_heap_state_finish(dev, &queue->heap_state);
   if (queue->bind_ctx != NULL)
      nvkmd_ctx_destroy(queue->bind_ctx);
   if (queue->exec_ctx != NULL)
      nvkmd_ctx_destroy(queue->exec_ctx);
   vk_queue_finish(&queue->vk);
   vk_free(&dev->vk.alloc, queue);
}
