/*
 * Copyright © 2026 Switch
 * SPDX-License-Identifier: MIT
 *
 * NVK kernel backend for Darwin: the TegraGM20B IOKit user client (wire ABI in
 * TegraGM20BABI.h, semantics in scratchpad/gpu/CONTRACT.md). Structured like
 * nvkmd_nouveau, with each DRM ioctl replaced by one IOConnectCallStructMethod.
 * Sync objects live in user space (one kernel fence seqno per channel), as in
 * UniXNU's Turnip darwin backend.
 */
#ifndef NVKMD_DARWIN_H
#define NVKMD_DARWIN_H 1

#include "nvkmd/nvkmd.h"
#include "util/u_dynarray.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "util/simple_mtx.h"
#include "util/vma.h"

#include "TegraGM20BABI.h"
#include "nvkmd_darwin_kill.h"
#include "nvkmd_darwin_submit_state.h"

#include <IOKit/IOKitLib.h>

struct nvkmd_darwin_conn {
   io_connect_t conn;
   bool tunnel;
   uint32_t selector;
   bool kernel_map; /* TegraGM20BInfoKernelMap: map through methods 12, 13 */
   bool exec_surfaces; /* TegraGM20BInfoExecSurfaces: method 17 */
   bool kill_record; /* TegraGM20BInfoKillRecord */
};

struct nvkmd_darwin_pdev {
   struct nvkmd_pdev base;

   struct TegraGM20BAbiInfo info;

   struct vk_sync_type sync_type;
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, darwin);

VkResult nvkmd_darwin_create_pdev(struct vk_object_base *log_obj,
                                  enum nvk_debug debug_flags,
                                  struct nvkmd_pdev **pdev_out);

#define NVKMD_DARWIN_MAX_CHANNELS 16

struct nvkmd_darwin_channel {
   uint32_t id;                         /* 0 = free slot */
   const volatile uint32_t *fence;      /* completed seqno, mapped read-only */
   mach_vm_address_t fence_map;
   uint32_t fence_token;
};

struct nvkmd_darwin_dev {
   struct nvkmd_dev base;

   struct nvkmd_darwin_conn conn;
   bool shared_conn; /* the process's connection (darwin_shared), not its own */

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
   uint64_t replay_start;

   simple_mtx_t channels_mutex;
   struct nvkmd_darwin_channel channels[NVKMD_DARWIN_MAX_CHANNELS];
   struct darwin_kills kills; /* channels_mutex */
};

NVKMD_DECL_SUBCLASS(dev, darwin);

struct nvkmd_darwin_mem {
   struct nvkmd_mem base;

   uint64_t handle;
   uint64_t bo_offset_B; /* where the memory starts in the BO: 0 but for foreign memory */
   uint32_t map_token;  /* 0 for host imports */
   uint32_t surface_id; /* the IOSurface a host import is of, 0: none */
   void *host_ptr;      /* host imports: the caller's own mapping */

   /* Foreign memory (switch_nvk_import_foreign): another driver's BO, bound
    * in its VA already; drop(lease) when it is freed, and no kernel call.
    */
   void (*drop)(void *lease);
   void *lease;
};

NVKMD_DECL_SUBCLASS(mem, darwin);

struct nvkmd_darwin_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, darwin);

struct nvkmd_darwin_exec_ctx {
   struct nvkmd_ctx base;

   uint32_t channel;
   enum nvkmd_engines engines;
   uint32_t last_seqno;  /* last submitted; 0 before the first submission */
   bool submitted;       /* an Exec of it went through (a seqno may wrap to 0) */

   struct darwin_submit_state submission;

   struct TegraGM20BAbiExec req;

   /* The submission's host imports, sent before each of its Execs while set
    * (nvkmd_darwin_submit_surfaces)
    */
   bool surfaces_given;
   struct TegraGM20BAbiExecSurfaces surfaces;
};

NVKMD_DECL_SUBCLASS(ctx, darwin_exec);

struct nvkmd_darwin_bind_ctx {
   struct nvkmd_ctx base;

   struct nvkmd_va *prev_va;
   struct TegraGM20BAbiVmBind req;
};

NVKMD_DECL_SUBCLASS(ctx, darwin_bind);

/* A binary sync: unsignaled, signaled, or pending on one channel's fence. */
enum nvkmd_darwin_sync_state {
   NVKMD_DARWIN_SYNC_UNSIGNALED,
   NVKMD_DARWIN_SYNC_SIGNALED,
   NVKMD_DARWIN_SYNC_FENCE,
};

struct nvkmd_darwin_sync {
   struct vk_sync base;

   /* Binary syncs */
   enum nvkmd_darwin_sync_state state;
   struct nvkmd_darwin_dev *dev;
   uint32_t channel;
   uint32_t seqno;

   /* Timeline syncs (base.flags & VK_SYNC_IS_TIMELINE): the highest value
    * known reached, and the submitted signal operations not yet seen
    * complete, each pending on one channel's seqno.
    */
   uint64_t value;
   struct util_dynarray points; /* struct nvkmd_darwin_sync_point */
   struct darwin_failures *failures; /* values whose signals failed, NULL: none */
};

struct nvkmd_darwin_sync_point {
   uint64_t value;
   struct nvkmd_darwin_dev *dev;
   uint32_t channel;
   uint32_t seqno;
   uint32_t error;
};

/* The IOSurface the next host import on this thread covers, from the Metal frontend (dlsym: the Vulkan loader opened
 * this library RTLD_LOCAL). */
void switch_nvk_import_surface(uint32_t surface_id, const void *base, uint64_t bytes);
/* The shape the next surface import on this thread asks the surface's layout for (method 16: width, height, bpp,
 * bytes_per_row, flags TegraGM20BLayout*). The import binds a block-linear surface under the kind it is written in. */
void switch_nvk_import_surface_shape(uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytes_per_row,
                                     uint32_t flags);
/* The last surface import on this thread: its handle (0: none made) and the layout it was answered (method 16's reply;
 * UINT32_MAX: not asked, or no answer). */
uint32_t switch_nvk_import_surface_layout(uint64_t *handle);
/* The layout of the surface whose import (this process's connection) is `handle`, for a shape: method 16's reply; 0
 * where the kernel lays no surface out block-linear; UINT32_MAX: no answer. */
uint32_t switch_nvk_surface_layout(uint64_t handle, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytes_per_row,
                                   uint32_t flags);
/* The GPU may write the surface from now on (method 15): before its first render, blit or store. false: not said. */
bool switch_nvk_surface_gpu_writes(uint32_t surface_id);
/* The GPU priority the process's Metal queues asked for (method 18: Metal's values, TegraGM20BABI.h). false: not
 * applied. */
bool switch_nvk_gpu_priority(uint32_t priority, uint32_t background_priority);
/* The process's connection, for the plugin's raw GM20B path (drivers/TegraGM20BMetal/raw/runtime/raw_kernel.cpp): opened by
 * the first of it and NVK's first device, closed after the last release. One connection is one GPU address space, so each
 * driver can use memory the other bound with no second import or bind. [*va_start, *va_end): client VAs NVK never hands
 * out, the caller's to bind. false: no connection. */
bool switch_nvk_connection(uint32_t *conn, bool *tunnel, uint32_t *selector, uint64_t *va_start,
                           uint64_t *va_end);
void switch_nvk_connection_release(void);
/* The next host import on this thread whose range lies in [cpu, cpu + bytes) is memory the caller has bound in the
 * shared connection's VM already: BO `handle` (cpu its first byte) at `va`, PTE kind `kind`. NVK makes it with no kernel
 * call and frees it with none: hold(lease) when it makes it, drop(lease) when it frees it. cpu NULL: none. */
void switch_nvk_import_foreign(const void *cpu, uint64_t bytes, uint64_t handle, uint64_t va, uint32_t kind,
                               void (*hold)(void *), void (*drop)(void *), void *lease);

#endif /* NVKMD_DARWIN_H */
