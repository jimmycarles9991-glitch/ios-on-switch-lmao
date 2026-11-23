/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define lower_32_bits(n) ((u32)((n) & 0xffffffff))

enum nvkm_memory_target {
	NVKM_MEM_TARGET_INST_SR_LOST,
	NVKM_MEM_TARGET_INST,
	NVKM_MEM_TARGET_VRAM,
	NVKM_MEM_TARGET_HOST,
	NVKM_MEM_TARGET_NCOH,
};

struct nvkm_device;
struct nvkm_vmm;

struct nvkm_vma {
	u64 addr;
	u64 size;
	u8 page;
};

struct nvkm_memory;

struct nvkm_memory_func {
	u64 (*size)(struct nvkm_memory *);
	int (*map)(struct nvkm_memory *, u64 offset, struct nvkm_vmm *, struct nvkm_vma *,
		   void *argv, u32 argc);
};

struct nvkm_memory {
	const struct nvkm_memory_func *func;
};

struct nvkm_gpuobj;

struct nvkm_gpuobj_func {
	void (*wr32)(struct nvkm_gpuobj *, u32 offset, u32 data);
};

struct nvkm_gpuobj {
	const struct nvkm_gpuobj_func *ptrs;
};

#define nvkm_memory_size(p) ((p)->func->size(p))
#define nvkm_memory_map(p, o, vm, va, av, ac) ((p)->func->map((p), (o), (vm), (va), (av), (ac)))
#define nvkm_wo32(o, a, d) ((o)->ptrs->wr32((o), (a), (d)))

#ifdef __cplusplus
extern "C" {
#endif
int nvkm_memory_new(struct nvkm_device *, enum nvkm_memory_target, u64 size, u32 align, bool zero,
		    struct nvkm_memory **);
void nvkm_memory_unref(struct nvkm_memory **);
int nvkm_vmm_get(struct nvkm_vmm *, u8 page, u64 size, struct nvkm_vma **);
void nvkm_vmm_put(struct nvkm_vmm *, struct nvkm_vma **);
#ifdef __cplusplus
}
#endif
