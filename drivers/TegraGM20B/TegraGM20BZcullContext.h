/* SPDX-License-Identifier: MIT */
/*
 * A GR context's zcull image: FECS saves the zcull RAM into it when it switches the context out and
 * restores it when it switches the context back in (nvgpu gr_gk20a_ctx_zcull_setup, separate buffer
 * mode). The golden image's mode, no ctxsw, leaves the zcull RAM out of the context, so a context
 * switched back in mid-pass would find another context's zcull there. Mode and pointer words: nvgpu
 * hw_ctxsw_prog_gm20b.h ctxsw_prog_main_image_zcull_o, _zcull_ptr_o.
 * test/gm20b_zcull_context_test.cpp checks it.
 */
#ifndef TEGRA_GM20B_ZCULL_CONTEXT_H
#define TEGRA_GM20B_ZCULL_CONTEXT_H

#include <core/gpuobj.h>
#include <core/memory.h>
#include <subdev/mmu.h>
#include "TegraGM20BZcull.h"

#define TGM_ZCULL_CTX_MODE 0x1c
#define TGM_ZCULL_CTX_PTR 0x20
#define TGM_ZCULL_CTX_NO_CTXSW 1
#define TGM_ZCULL_CTX_SEPARATE_BUFFER 2

/* `bytes`: FECS's discovered zcull image size, 0 for none. Zero-filled, as Mesa zero-fills zcull
 * storage before zcull first reads it (nvk_cmd_buffer.c: garbage there kills the context). The
 * caller's teardown runs tgm_zcull_image_del after a failure here too. */
static inline int tgm_zcull_image_new(struct nvkm_device *device, struct nvkm_vmm *vmm, u32 bytes,
				      void *map_args, u32 map_argc, struct nvkm_memory **memory,
				      struct nvkm_vma **vma)
{
	int ret = 0;

	if (!bytes)
		return 0;
	ret = nvkm_memory_new(device, NVKM_MEM_TARGET_INST, bytes, TGM_ZCULL_CTXSW_ALIGN, true,
			      memory);
	if (ret)
		return ret;
	ret = nvkm_vmm_get(vmm, 12, nvkm_memory_size(*memory), vma);
	if (ret)
		return ret;
	return nvkm_memory_map(*memory, 0, vmm, *vma, map_args, map_argc);
}

/* No image: no ctxsw, as the golden image holds (nvgpu gr_gk20a_init_golden_ctx_image) */
static inline void tgm_zcull_image_bind(struct nvkm_gpuobj *ctx, const struct nvkm_vma *vma)
{
	nvkm_wo32(ctx, TGM_ZCULL_CTX_MODE,
		  vma ? TGM_ZCULL_CTX_SEPARATE_BUFFER : TGM_ZCULL_CTX_NO_CTXSW);
	nvkm_wo32(ctx, TGM_ZCULL_CTX_PTR, vma ? lower_32_bits(vma->addr >> 8) : 0);
}

static inline void tgm_zcull_image_del(struct nvkm_vmm *vmm, struct nvkm_memory **memory,
				       struct nvkm_vma **vma)
{
	nvkm_vmm_put(vmm, vma);
	nvkm_memory_unref(memory);
}

#endif
