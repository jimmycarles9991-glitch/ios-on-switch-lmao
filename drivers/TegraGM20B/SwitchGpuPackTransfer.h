// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/vnode.h>
#include "SwitchGpuPackImage.h"
#include "TegraGM20BGpu.h"

struct SwitchGpuPackStreamStats {
	uint32_t pageinReqs;
	uint64_t copyNs;
};

struct SwitchGpuPackWarmStats {
	uint64_t readBytes;
	uint64_t residentBytes;
};

bool switch_gpu_pack_shutdown_requested(void);
#if TGM_STATS
uint64_t switch_gpu_pack_now_ns(void);
#endif
int switch_gpu_pack_read_exact(vnode_t vp, void *buf, size_t len, off_t offset, vfs_context_t ctx);
bool switch_gpu_pack_alloc_tier0(uint64_t tier0Bytes, IOMemoryDescriptor **multiOut,
				 void ***chunkCpuOut, uint64_t **pasOut, uint32_t *chunkCountOut);
bool switch_gpu_pack_stream_tier0(vnode_t vp, uint64_t heapOff, uint64_t tier0Bytes,
				  uint64_t *dstPas, void **chunkCpu, const char **whyOut,
				  int *errOut, SwitchGpuPackStreamStats *stats);
void switch_gpu_pack_warm_tier1(const SwitchGpuPackHeader *hdr, uint64_t fileSize,
				SwitchGpuPackWarmStats *stats);
