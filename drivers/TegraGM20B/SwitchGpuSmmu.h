// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GPU_SMMU_BIT (1ULL << 34)
#define SWITCH_GPU_SMMU_GRANULE (64ULL << 10)
#define SWITCH_GPU_SMMU_GRANULE_PAGES 16u
#define SWITCH_GPU_SMMU_WINDOW_BASE 0x10000000ULL
#define SWITCH_GPU_SMMU_MAX_WINDOW_BYTES (1ULL << 30)
#define SWITCH_GPU_SMMU_UNCOMPRESSED_RESERVE (128ULL << 20)

/* GM20B's a[34] = 0 accesses are never translated (swgroup GPU); a[34] = 1 accesses always are (swgroup GPUB), through
 * the ASID of field va[33:32] (T210 TRM 18.5.3 note 2, 18.11.1.136). Fields 0 and 1 are RAM at its own address
 * (TegraSMMUIdentity.h): a page translates while a memory object holds it. Field 2 is the compaction window. */
#define SWITCH_GPU_SMMU_WINDOW_FIELD (2ULL << 32)

static inline uint64_t switch_gpu_smmu_pa(uint64_t iova)
{
	return SWITCH_GPU_SMMU_BIT | SWITCH_GPU_SMMU_WINDOW_FIELD | (iova & 0xffffffffULL);
}

static inline int switch_gpu_smmu_is_iova(uint64_t pa)
{
	return (pa & (SWITCH_GPU_SMMU_BIT | (3ULL << 32))) ==
	       (SWITCH_GPU_SMMU_BIT | SWITCH_GPU_SMMU_WINDOW_FIELD);
}

/* A RAM page (below 6 GiB) through GPUB's view of RAM. */
static inline uint64_t switch_gpu_smmu_ram_pa(uint64_t pa)
{
	return SWITCH_GPU_SMMU_BIT | pa;
}

static inline int switch_gpu_smmu_is_ram_pa(uint64_t pa)
{
	return (pa & SWITCH_GPU_SMMU_BIT) != 0u && ((pa >> 32) & 3u) < 2u;
}

static inline uint32_t switch_gpu_smmu_prefix_pages(uint32_t npages)
{
	return npages & ~(SWITCH_GPU_SMMU_GRANULE_PAGES - 1u);
}

static inline int switch_gpu_smmu_prefix_scattered(const uint64_t *pas, uint32_t prefix_pages)
{
	for (uint32_t i = 0; i < prefix_pages; i += SWITCH_GPU_SMMU_GRANULE_PAGES) {
		const uint64_t first = pas[i];
		if (first & (SWITCH_GPU_SMMU_GRANULE - 1u))
			return 1;
		for (uint32_t k = 1; k < SWITCH_GPU_SMMU_GRANULE_PAGES; ++k) {
			if (pas[i + k] != first + ((uint64_t)k << 12))
				return 1;
		}
	}
	return 0;
}

/* GM20B selects Tegra SMMU per transaction via physical bit 34; bit 34 = 0 accesses DRAM directly.
 * Compact scattered 64 KiB-multiple allocations and non-alternating 64 KiB chunks whose bit 16
 * collides on a shared 128 KiB comptagline into contiguous 64 KiB SMMU IOVAs. */
static inline int switch_gpu_smmu_should_map(const uint64_t *pas, uint32_t npages)
{
	if (!pas || !npages || (npages % SWITCH_GPU_SMMU_GRANULE_PAGES) != 0u)
		return 0;
	if (switch_gpu_smmu_prefix_scattered(pas, npages))
		return 1;
	for (uint32_t i = 0; i + 2u * SWITCH_GPU_SMMU_GRANULE_PAGES <= npages;
	     i += 2u * SWITCH_GPU_SMMU_GRANULE_PAGES) {
		const uint64_t diff = pas[i] ^ pas[i + SWITCH_GPU_SMMU_GRANULE_PAGES];
		if ((diff & SWITCH_GPU_SMMU_GRANULE) == 0u)
			return 1;
	}
	return 0;
}

/* Uncompressed 64 KiB big-page compaction maps only the aligned prefix via SMMU and keeps any
 * < 64 KiB tail on direct PA, so tail pages must not alias any prefix page in pre-SMMU L2. */
static inline int switch_gpu_smmu_should_map_big(const uint64_t *pas, uint32_t npages)
{
	const uint32_t prefix = switch_gpu_smmu_prefix_pages(npages);
	if (!pas || !prefix)
		return 0;
	if (!switch_gpu_smmu_prefix_scattered(pas, prefix))
		return 0;
	if (prefix < npages) {
		uint64_t tail_min = pas[prefix];
		uint64_t tail_max = pas[prefix];
		for (uint32_t t = prefix + 1u; t < npages; ++t) {
			if (pas[t] < tail_min)
				tail_min = pas[t];
			if (pas[t] > tail_max)
				tail_max = pas[t];
		}
		for (uint32_t i = 0; i < prefix; ++i) {
			const uint64_t pa = pas[i];
			if (pa < tail_min || pa > tail_max)
				continue;
			for (uint32_t t = prefix; t < npages; ++t) {
				if (pas[t] == pa)
					return 0;
			}
		}
	}
	return 1;
}

static inline uint32_t switch_gpu_smmu_pfn_table_cap(uint32_t npages)
{
	uint32_t cap = 64u;
	if (!npages || npages > (uint32_t)(SWITCH_GPU_SMMU_MAX_WINDOW_BYTES >> 12))
		return 0;
	while (cap < npages * 2u)
		cap <<= 1;
	return cap;
}

/* Pre-SMMU L2 tags lines by BIT(34)|iova vs direct PA: reject duplicate physical pages so one
 * physical page is never addressed under two pre-SMMU tags within the same allocation. */
static inline int switch_gpu_smmu_pages_unique(const uint64_t *pas, uint32_t npages,
					       uint32_t *table, uint32_t table_cap)
{
	if (!pas || !npages || !table || !table_cap || (table_cap & (table_cap - 1u)) != 0u ||
	    table_cap <= npages)
		return 0;
	const uint32_t mask = table_cap - 1u;
	for (uint32_t i = 0; i < npages; ++i) {
		const uint64_t pa = pas[i];
		if (!pa || (pa & 0xfffULL) || pa >= SWITCH_GPU_SMMU_BIT)
			return 0;
		const uint32_t pfn = (uint32_t)(pa >> 12);
		uint32_t slot = (pfn ^ (pfn >> 11)) & mask;
		while (table[slot]) {
			if (table[slot] == pfn)
				return 0;
			slot = (slot + 1u) & mask;
		}
		table[slot] = pfn;
	}
	return 1;
}

int switch_gpu_smmu_init(void);
/* The compaction window may be used: gpu_mem.c's modes 1 and 2, the compressible and big-page imports. */
int switch_gpu_smmu_ready(void);
/* GPUB attached: memory objects reach RAM through its view (switch_gpu_smmu_hold). */
int switch_gpu_smmu_ram_ready(void);
/* Holds of 4 KiB RAM pages (raw physical addresses) in GPUB's view, published before return; all or nothing. 0, or -12
 * when a page table could not be had. Thread context. */
int switch_gpu_smmu_hold(const uint64_t *pas, uint32_t npages);
/* Ends the holds of the RAM-view entries of `pages` (addresses as switch_gpu_smmu_ram_pa made them; other entries are
 * skipped), under one lock of the view and one commit, published before return: a page no other holder holds faults
 * from then on. A page with no hold panics. Thread context. */
void switch_gpu_smmu_release_ram(const uint64_t *pages, uint32_t npages);
uint64_t switch_gpu_smmu_map(const uint64_t *pas, uint32_t npages);
uint64_t switch_gpu_smmu_map_reserved(const uint64_t *pas, uint32_t npages, uint64_t reserve_bytes);
void switch_gpu_smmu_unmap(uint64_t iova, uint32_t npages);
void switch_gpu_smmu_usage(uint64_t *total, uint64_t *free_bytes);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
/* The SMMU's TLB and PTC hits and misses since the previous call (switch_smmu_stats_window), for the stats window. */
void switch_gpu_smmu_stats(char *line, size_t size);
#endif

#ifdef __cplusplus
}
#endif
