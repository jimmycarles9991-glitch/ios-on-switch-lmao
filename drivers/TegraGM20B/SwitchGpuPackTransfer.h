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

bool switch_gpu_pack_shutdown_requested(void);
#if TGM_STATS
uint64_t switch_gpu_pack_now_ns(void);
#endif
int switch_gpu_pack_read_exact(vnode_t vp, void *buf, size_t len, off_t offset, vfs_context_t ctx);
bool switch_gpu_pack_alloc_tier0(uint64_t tier0Bytes, IOMemoryDescriptor **multiOut,
				 void ***chunkCpuOut, uint64_t **pasOut, uint32_t *chunkCountOut);
// `frontier` (null: none) is raised to the chunks of kSwitchGpuPackTransferBytes landed so far, each once its bytes are in
// the image: the word jobs that read the image acquire on (switch_gpu_pack_job_wait)
bool switch_gpu_pack_stream_tier0(vnode_t vp, uint64_t heapOff, uint64_t tier0Bytes,
				  uint64_t *dstPas, void **chunkCpu, const char **whyOut,
				  int *errOut, SwitchGpuPackStreamStats *stats,
				  volatile uint32_t *frontier);
// The pages a render server wires at its open (TegraGM20BMetal pack_window.h packWindowCollectCpuRanges) read into the
// page cache: every fast key, the other tables' tier-0 parts and the segments' words. False: a read failed (the open
// reads what is left itself).
bool switch_gpu_pack_prefetch_tables(vnode_t vp, const SwitchGpuPackHeader &hdr, vfs_context_t ctx);

// The frontier word in the kernel's fence page: a store the GPU's acquires read from DRAM (the pool is write-combined,
// where an atomic read-modify-write is an SError)
inline void switch_gpu_pack_raise(volatile uint32_t *word, uint32_t value)
{
	__atomic_store_n(word, value, __ATOMIC_RELEASE);
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ volatile("dsb sy" ::: "memory");
#endif
}
