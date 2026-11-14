// SPDX-License-Identifier: MIT
// chan506f.c's GPFIFO entries and kick on the host: the shim's barriers and register writes, counted. nvif_wr32 is the
// kernel's iowrite32 (switch_mmio_write32_publish: a store barrier, then the register).
#pragma once
#include "drivers/TegraGM20B/test/gpfifo_include/nvif/chan.h"

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned switch_gpu_test_barriers, switch_gpu_test_doorbells;
extern u32 switch_gpu_test_doorbell;
#ifdef __cplusplus
}
#endif

typedef uint8_t u8;
#define __iomem
#define BIT(n) (1u << (n))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v) >> 32))
#define wmb() ((void)++switch_gpu_test_barriers)
#define dma_wmb() ((void)++switch_gpu_test_barriers)
#define mb() ((void)++switch_gpu_test_barriers)
#define switch_gpu_test_register(address, value)             \
	((void)(address), (void)++switch_gpu_test_doorbells, \
	 (void)(switch_gpu_test_doorbell = (value)))
#define writel_relaxed(value, address) switch_gpu_test_register(address, value)
#define nvif_wr32(object, offset, value)   \
	((void)++switch_gpu_test_barriers, \
	 switch_gpu_test_register((char *)(object)->map.ptr + (offset), value))
#define nvif_rd32(object, offset) (*(volatile u32 *)((char *)(object)->map.ptr + (offset)))

#ifdef __cplusplus
extern "C" {
#endif
void nvif_chan506f_gpfifo_kick(struct nvif_chan *chan);
void nvif_chan506f_gpfifo_push(struct nvif_chan *chan, bool main, u64 addr, u32 size,
			       bool no_prefetch);
int nvif_chan506f_ctor(struct nvif_chan *chan, void *userd, void *gpfifo, u32 gpfifo_size,
		       void *push, u64 push_addr, u32 push_size);
#ifdef __cplusplus
}
#endif
