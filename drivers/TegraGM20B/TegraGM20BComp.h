/* SPDX-License-Identifier: MIT */
/*
 * GM20B colour and depth compression as pure functions: the compbit backing store (nouveau/nvkm/subdev/ltc/gm20b.c),
 * which memory and binds may compress (gpu.c), and the PTE fields a compressed bind writes (nvkm/subdev/mmu/vmmgf100.c).
 * test/gm20b_comp_test.cpp checks them against nvgpu's gm20b numbers.
 *
 * The CBC numbers are NVIDIA's nvgpu for gm20b (L4T r32.7.6, common/ltc/ltc_gm20b.c: gm20b_ltc_init_comptags,
 * gm20b_ltc_init_cbc, gm20b_ltc_cbc_fix_config; common/fb/fb_gm20b.c: gm20b_fb_compression_page_size).
 */
#ifndef TEGRA_GM20B_COMP_H
#define TEGRA_GM20B_COMP_H
#include <stdint.h>

/* One comptag line covers 128 KiB. A compressed bind uses the spaces' 128 KiB big pages (gpu.c tgm_vm_new), so each PTE
 * names one whole line: the half-line select that USE_FULL_COMP_TAG_LINE gives 64 KiB pages (the PTE's top COMPTAGLINE
 * bit) never comes into play. */
#define TGM_COMP_SHIFT 17
#define TGM_COMP_BYTES (1ULL << TGM_COMP_SHIFT)

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

/* The GMMU page a memory object of 4 KiB pages may be mapped with: TGM_COMP_SHIFT when every 128 KiB of it is one
 * physically contiguous, 128 KiB-aligned run (what a big PTE maps), else 12. */
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
	return TGM_COMP_SHIFT;
}

/* A compressible kind binds only as whole big pages of memory that has them. */
static inline int tgm_comp_bind_ok(uint64_t va, uint64_t offset, uint64_t range, uint32_t mem_shift)
{
	return mem_shift >= TGM_COMP_SHIFT && range &&
	       !((va | offset | range) & (TGM_COMP_BYTES - 1));
}

/* The line of a bind's first PTE, for memory whose lines start at `first`; each further PTE takes the next. */
static inline uint64_t tgm_comp_line(uint64_t first, uint64_t offset)
{
	return first + (offset >> TGM_COMP_SHIFT);
}

/* NV_MMU_PTE_COMPTAGLINE (word 1 bits 12..28) and NV_MMU_PTE_KIND (word 1 bits 4..11) as bits of the 64-bit PTE. */
static inline uint64_t tgm_pte_comptagline(uint64_t line)
{
	return (line & 0x1ffff) << 44;
}

static inline uint64_t tgm_pte_kind(uint32_t kind)
{
	return (uint64_t)(kind & 0xff) << 36;
}

#endif
