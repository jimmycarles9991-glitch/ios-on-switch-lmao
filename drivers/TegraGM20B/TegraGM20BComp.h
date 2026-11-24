/* SPDX-License-Identifier: MIT */
/*
 * GM20B colour and depth compression as pure functions: the compbit backing store (nouveau/nvkm/subdev/ltc/gm20b.c),
 * which memory and binds may compress (gpu_mem.c), the GPU pool chunks a compressible BO is made of (shim/linux.c), and the PTE
 * fields a compressed bind writes (nvkm/subdev/mmu/vmmgf100.c).
 * test/gm20b_comp_test.cpp checks them against nvgpu's gm20b numbers.
 *
 * The CBC numbers are NVIDIA's nvgpu for gm20b (L4T r32.7.6, common/ltc/ltc_gm20b.c: gm20b_ltc_init_comptags,
 * gm20b_ltc_init_cbc, gm20b_ltc_cbc_fix_config; common/fb/fb_gm20b.c: gm20b_fb_compression_page_size).
 */
#ifndef TEGRA_GM20B_COMP_H
#define TEGRA_GM20B_COMP_H
#include <stdint.h>

/* One CBC comptag line covers 128 KiB (TGM_COMP_LINE_SHIFT = 17); each GMMU big page is 64 KiB
 * (TGM_COMP_SHIFT = 16), and USE_FULL_COMP_TAG_LINE selects the upper/lower half from PA bit 16. */
#define TGM_COMP_SHIFT 16
#define TGM_COMP_BYTES (1ULL << TGM_COMP_SHIFT)
#define TGM_COMP_LINE_SHIFT 17
#define TGM_COMP_LINE_BYTES (1ULL << TGM_COMP_LINE_SHIFT)

/* LTC_LTCS_LTSS_CBC_PARAM (0x17e280) */
static inline uint32_t tgm_cbc_comptags_per_cacheline(uint32_t param)
{
	return param & 0xffff;
}

static inline uint32_t tgm_cbc_cacheline_bytes(uint32_t param)
{
	return 512u << ((param >> 24) & 0xf);
}

static inline uint32_t tgm_cbc_slices(uint32_t param)
{
	return param >> 28;
}

struct tgm_cbc {
	uint32_t lines; /* line 0 included; nobody is given it (nvgpu reserves it too) */
	uint64_t bytes;
};

/* The backing store for ram_mib of memory (nvgpu covers all of RAM, one line per 128 KiB) on `ltcs` LTCs, at most
 * 2^line_bits - 1 lines: the PTE's COMPTAGLINE has 17 bits, 16 while USE_FULL_COMP_TAG_LINE (0x100c80 bit 12) takes
 * the top one. 0 if this L2 cannot compress. The bytes include nvgpu's slack for rounding the base up to 2 KiB per LTC;
 * the lines leave it out, so no line reaches past the end whatever the base's alignment. */
static inline int tgm_cbc_size(uint32_t param, uint32_t ltcs, uint64_t ram_mib, uint32_t line_bits,
			       struct tgm_cbc *c)
{
	const uint64_t per_row = tgm_cbc_comptags_per_cacheline(param);
	const uint64_t row =
		(uint64_t)tgm_cbc_cacheline_bytes(param) * tgm_cbc_slices(param) * ltcs;
	const uint64_t slack = (uint64_t)ltcs << 11;
	uint64_t cap = (1ULL << line_bits) - 1;
	uint64_t lines = ram_mib << 3;
	uint64_t bytes = 0;

	if (cap > 0x1ffff)
		cap = 0x1ffff;
	if (!per_row || !row || !lines)
		return 0;
	if (lines > cap)
		lines = cap;
	bytes = (lines + per_row - 1) / per_row * row + slack;
	bytes = (bytes + 0xffff) & ~0xffffULL;
	lines = (bytes - slack) / row * per_row;
	if (lines > cap)
		lines = cap;
	c->lines = (uint32_t)lines;
	c->bytes = bytes;
	return 1;
}

/* CBC_BASE (0x17e278) for a backing store at pa: its start rounded up to 2 KiB per LTC, in those units, doubled with two
 * LTCs (nvgpu gm20b_ltc_cbc_fix_config, "Bug 1477079"). 0 if the field (26 bits) cannot hold it. */
static inline int tgm_cbc_base(uint64_t pa, uint32_t ltcs, uint32_t *base)
{
	uint64_t b = 0;

	if (!ltcs)
		return 0;
	b = (pa >> 11) / ltcs;
	if (((b * ltcs) << 11) < pa)
		b++;
	if (ltcs == 2)
		b *= 2;
	if (b >> 26)
		return 0;
	*base = (uint32_t)b;
	return 1;
}

/* The GMMU page a memory object of 4 KiB pages may be mapped with: TGM_COMP_SHIFT when every 64 KiB
 * run is aligned and contiguous, and consecutive 64 KiB pairs sharing a 128 KiB comptag line carry
 * opposite PA bit 16 (USE_FULL_COMP_TAG_LINE half-line parity), else 12. */
static inline uint32_t tgm_comp_page_shift(const uint64_t *pas, uint32_t npages)
{
	const uint32_t per = (uint32_t)(TGM_COMP_BYTES >> 12);
	uint32_t i = 0;

	if (!npages || npages % per)
		return 12;
	for (i = 0; i < npages; i++) {
		const uint64_t first = pas[i - i % per];

		if ((first & (TGM_COMP_BYTES - 1)) || pas[i] != first + ((uint64_t)(i % per) << 12))
			return 12;
	}
	for (i = 0; i + 2u * per <= npages; i += 2u * per) {
		if (((pas[i] ^ pas[i + per]) & TGM_COMP_BYTES) == 0u)
			return 12;
	}
	return TGM_COMP_SHIFT;
}

#define TGM_COMP_PAGES (TGM_COMP_BYTES >> 12)

/* Whether `bits` (one bit per 4 KiB page) has a bit set in chunk `chunk`: pages chunk * TGM_COMP_PAGES on. */
static inline int tgm_comp_chunk_any(const unsigned long *bits, uint64_t chunk)
{
	const uint64_t word_bits = sizeof(unsigned long) * 8;
	uint64_t page = chunk * TGM_COMP_PAGES;
	const uint64_t end = page + TGM_COMP_PAGES;

	while (page < end) {
		const uint64_t at = page % word_bits;
		const uint64_t n = word_bits - at < end - page ? word_bits - at : end - page;
		const unsigned long mask = n == word_bits ? ~0UL : ((1UL << n) - 1) << at;

		if (bits[page / word_bits] & mask)
			return 1;
		page += n;
	}
	return 0;
}

/* Interleave `chunks[start..end)` so adjacent 64 KiB pairs carry opposite bit-0 (PA bit-16) parity
 * whenever both parities exist, keeping `chunks[0]` fixed. */
static inline void tgm_comp_alternate_chunks(uint64_t *chunks, uint32_t start, uint32_t end,
					     uint32_t boundary_parity, int match_boundary)
{
	uint32_t i = start;

	if (match_boundary && i < end && ((chunks[i] & 1u) == boundary_parity)) {
		for (uint32_t j = i + 1; j < end; j++) {
			if ((chunks[j] & 1u) != boundary_parity) {
				const uint64_t tmp = chunks[i];
				chunks[i] = chunks[j];
				chunks[j] = tmp;
				break;
			}
		}
		i++;
	} else if (match_boundary && i < end) {
		i++;
	}
	for (; i + 1 < end; i += 2) {
		if (((chunks[i] ^ chunks[i + 1]) & 1u) != 0u)
			continue;
		uint32_t j = i + 2;
		while (j < end && (((chunks[i] ^ chunks[j]) & 1u) == 0u))
			j++;
		if (j == end)
			break;
		const uint64_t tmp = chunks[i + 1];
		chunks[i + 1] = chunks[j];
		chunks[j] = tmp;
	}
}

/* `count` free chunks (each TGM_COMP_BYTES, aligned to it) of a pool of `npages` pages whose `map` has a bit set per page in
 * use and whose `dirty` has one per free page not known to be zero, for a compressible BO: no two of its big pages need be
 * adjacent, so a fragmented pool serves it as well as an empty one. One pass from the top down, away from the first-fit
 * allocations at the bottom; chunks with no dirty page are taken first, preferring balanced even/odd 64 KiB parity so
 * direct DRAM pairs satisfy USE_FULL_COMP_TAG_LINE without SMMU. Their indices go to `chunks`, the clean ones first;
 * returns how many of them are dirty (the last ones), or -1 with fewer than `count` free. */
static inline int tgm_comp_pick_chunks(const unsigned long *map, const unsigned long *dirty,
				       uint64_t npages, uint32_t count, uint64_t *chunks)
{
	uint32_t clean = 0;
	uint32_t unclean = 0;
	uint32_t clean_parity[2] = {0, 0};
	const uint32_t need_min = count / 2u;
	uint64_t chunk = npages / TGM_COMP_PAGES;

	while (chunk-- > 0 &&
	       (clean < count || clean_parity[0] < need_min || clean_parity[1] < need_min)) {
		if (tgm_comp_chunk_any(map, chunk))
			continue;
		const uint32_t p = (uint32_t)(chunk & 1u);
		if (!tgm_comp_chunk_any(dirty, chunk)) {
			if (clean < count) {
				if (clean + unclean == count)
					unclean--; /* it takes the slot of the last dirty one */
				chunks[clean++] = chunk;
				clean_parity[p]++;
			} else if (clean_parity[p] < need_min) {
				for (uint32_t k = clean - 1; k > 0; k--) {
					if ((chunks[k] & 1u) != p) {
						clean_parity[p ^ 1u]--;
						chunks[k] = chunk;
						clean_parity[p]++;
						break;
					}
				}
			}
		} else if (clean + unclean < count) {
			chunks[count - 1 - unclean++] = chunk;
		}
	}
	if (clean + unclean < count)
		return -1;
	if (clean >= 2u)
		tgm_comp_alternate_chunks(chunks, 0, clean, 0, 0);
	if (unclean) {
		const int straddles = (clean & 1u) != 0u;
		const uint32_t prev_p = straddles ? (uint32_t)(chunks[clean - 1] & 1u) : 0u;
		tgm_comp_alternate_chunks(chunks, clean, count, prev_p, straddles);
	}
	return (int)unclean;
}

/* How many chunks of a pool of `npages` pages have no page in use (`map`) and none not known to be zero (`dirty`): what
 * tgm_comp_pick_chunks can hand a compressible BO now. */
static inline uint64_t tgm_comp_clean_chunks(const unsigned long *map, const unsigned long *dirty,
					     uint64_t npages)
{
	uint64_t chunk = npages / TGM_COMP_PAGES;
	uint64_t clean = 0;

	while (chunk-- > 0)
		clean += !tgm_comp_chunk_any(map, chunk) && !tgm_comp_chunk_any(dirty, chunk);
	return clean;
}

/* A compressible kind binds only as whole big pages of memory that has them. */
static inline int tgm_comp_bind_ok(uint64_t va, uint64_t offset, uint64_t range, uint32_t mem_shift)
{
	return mem_shift >= TGM_COMP_SHIFT && range &&
	       !((va | offset | range) & (TGM_COMP_BYTES - 1));
}

/* The 128 KiB comptag line of a bind's first PTE for memory whose lines start at `first`. */
static inline uint64_t tgm_comp_line(uint64_t first, uint64_t offset)
{
	return first + (offset >> TGM_COMP_LINE_SHIFT);
}

/* The 64 KiB half-line counter for GM20B USE_FULL_COMP_TAG_LINE: `counter >> 1` yields the
 * 16-bit comptag line for each 64 KiB big page while PA bit 16 selects bit 60 of the PTE. */
static inline uint64_t tgm_comp_half_line(uint64_t first, uint64_t offset)
{
	return (first << 1) + (offset >> TGM_COMP_SHIFT);
}

/* NV_MMU_PTE_COMPTAGLINE (word 1 bits 12..28) and NV_MMU_PTE_KIND (word 1 bits 4..11) as bits of the 64-bit PTE. */
static inline uint64_t tgm_pte_comptagline(uint64_t line)
{
	return (line & 0x1ffff) << 44;
}

static inline uint64_t tgm_pte_comptagline_64k(uint64_t line, uint64_t addr)
{
	return tgm_pte_comptagline(line & 0xffffULL) |
	       ((addr & TGM_COMP_BYTES) ? (1ULL << 60) : 0ULL);
}

static inline uint64_t tgm_pte_kind(uint32_t kind)
{
	return (uint64_t)(kind & 0xff) << 36;
}

#endif
