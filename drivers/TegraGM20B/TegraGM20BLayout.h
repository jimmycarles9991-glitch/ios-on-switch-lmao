// SPDX-License-Identifier: MIT
// The GM20B's block-linear layout of a one-level 2D colour image, which every GPU user of this tree takes from here: the raw
// GM20B path's textures and mip chains (raw/fx), the kernel's lock-time detile and the display's tiled scan
// (TegraScanoutCopy.h). It is Mesa NIL's layout for that image (nil/image.rs Image::new_tiled, nil/tiling.rs Tiling::choose
// and clamp, GOBType::TegraColor, Image::nvc0_choose_pte_kind), so NVK's VK_IMAGE_TILING_OPTIMAL image of the same size and
// texel size lies at the same bytes under the same PTE kind, and NVK's view of the memory is free (nvk_image_plane_bind
// reuses a VA whose kind is the plane's). drivers/TegraGM20BMetal/test/raw_layout_test.cpp holds it to NIL's answers.
//
// GOBs of 64 bytes x 8 rows; a block is one GOB wide and 2^y_log2 GOBs tall; blocks lie row by row. Inside a GOB the bytes
// are in the TegraColor arrangement (nil/copy.rs CopyGOBTegra), which is where kind TGM_LAYOUT_KIND memory holds them in
// DRAM: what the CPU sees (the raw path's device self-test) and what DC A scans.
//
// Which IOSurfaces are block-linear (the kernel decides once per surface, TegraGM20BTiled.cpp, at its first GPU import that
// asks; every process, driver and engine reads that decision): those the GPU produces. A surface is tiled when its first
// asking import will write it on the GPU (a render target, a store, a blit destination), its shape tiles, and no other
// import of it exists yet. It stays pitch for good when:
//  - its first GPU import only samples it: the CPU produces it (a CG-drawn backing store, a CPU-decoded frame) and may
//    rewrite it every frame, and a tiled surface pays a copy-engine move and a remap of the locker's mapping at every CPU
//    lock and unlock (more than the GPU saves on one sampled read);
//  - it lies in a client's own memory (IOSurface created over the app's pages): the app reads and writes those pages
//    without IOSurfaceLock, so they must hold rows;
//  - another import made before it asked, or its texel size or extent, rules it out.
// A GPU-produced surface is CPU-read rarely (a snapshot's encoder), and its lock sees rows.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NV_MMU_PTE_KIND_GENERIC_16BX2: NIL's kind for uncompressed colour of 8 to 128 bits a texel. The 3D and 2D engines lay out
// block-linear memory only under it (a kind-0 mapping scrambles each GOB's interior).
#define TGM_LAYOUT_KIND 0xfeu
#define TGM_LAYOUT_MAX_EXTENT 16384u

struct tgm_layout {
	uint32_t width, height, bpp;
	uint32_t row_bytes;  // the row stride: width x bpp in whole GOBs
	uint32_t rows;       // height in whole blocks
	uint8_t y_log2;      // GOBs a block tall, log2
	uint64_t align;      // max(one block, 4096)
	uint64_t bytes;      // row_bytes x rows, in whole `align`s
};

// Texel sizes NIL tiles (others are pitch-linear: NV_MMU_PTE_KIND_PITCH)
static inline int tgm_layout_tiles_bpp(uint32_t bpp)
{
	return (bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 16) ? 1 : 0;
}

static inline uint32_t tgm_layout_log2_ceil(uint32_t v)
{
	uint32_t l = 0;
	while ((1u << l) < v)
		l++;
	return l;
}

// The block height of a 2D image `height` texels tall: the proprietary driver's choice by height that NIL copies, clamped
// below twice the image's height in GOBs. The hardware clamps each mip level's block height the same way.
static inline uint8_t tgm_layout_y_log2(uint32_t height)
{
	uint32_t y = 0;
	if (height >= 86) {
		y = 4;
	} else if (height >= 43) {
		y = 3;
	} else if (height >= 22) {
		y = 2;
	} else if (height >= 11) {
		y = 1;
	}
	const uint32_t clamp = tgm_layout_log2_ceil((height + 7) / 8);
	return (uint8_t)(y < clamp ? y : clamp);
}

// 0: no tiled layout (an extent of 0 or past TGM_LAYOUT_MAX_EXTENT, a texel size NIL does not tile)
static inline int tgm_layout_tiled(uint32_t width, uint32_t height, uint32_t bpp,
				   struct tgm_layout *out)
{
	if (!width || !height || width > TGM_LAYOUT_MAX_EXTENT || height > TGM_LAYOUT_MAX_EXTENT ||
	    !tgm_layout_tiles_bpp(bpp))
		return 0;
	const uint8_t y = tgm_layout_y_log2(height);
	const uint32_t block_rows = 8u << y;
	const uint64_t block = 512ULL << y;
	out->width = width;
	out->height = height;
	out->bpp = bpp;
	out->y_log2 = y;
	out->row_bytes = (width * bpp + 63) & ~63u;
	out->rows = (height + block_rows - 1) / block_rows * block_rows;
	out->align = block > 4096 ? block : 4096;
	out->bytes =
		((uint64_t)out->row_bytes * out->rows + out->align - 1) / out->align * out->align;
	return 1;
}

// Byte xB % 64 of row y % 8 inside a GOB (TegraColor)
static inline uint32_t tgm_layout_gob_offset(uint32_t xB, uint32_t y)
{
	return ((xB % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xB % 32) / 16) * 32 + (y % 2) * 16 +
	       (xB % 16);
}

// Byte xB of row y of an image `row_bytes` across with blocks 2^y_log2 GOBs tall, from its first byte
static inline uint64_t tgm_layout_offset_of(uint32_t row_bytes, uint8_t y_log2, uint32_t xB,
					    uint32_t y)
{
	const uint32_t block_rows = 8u << y_log2;
	const uint64_t block = (uint64_t)(y / block_rows) * (row_bytes / 64) + xB / 64;
	return block * (512ULL << y_log2) + (uint64_t)((y % block_rows) / 8) * 512 +
	       tgm_layout_gob_offset(xB, y);
}

static inline uint64_t tgm_layout_offset(const struct tgm_layout *l, uint32_t xB, uint32_t y)
{
	return tgm_layout_offset_of(l->row_bytes, l->y_log2, xB, y);
}

// The CPU's copies of `w` x `h` texels at (x, y) between a tiled image (`tiled`, its first byte) and rows `pitch` bytes
// apart, one GOB line's 16-byte run at a time; the caller keeps the rectangle inside the image and handles the cache lines.
static inline void tgm_layout_write(const struct tgm_layout *l, uint8_t *tiled, uint32_t x,
				    uint32_t y, uint32_t w, uint32_t h, const uint8_t *src,
				    uint64_t pitch)
{
	const uint32_t first = x * l->bpp;
	const uint32_t end = (x + w) * l->bpp;
	for (uint32_t row = 0; row < h; row++) {
		for (uint32_t xB = first; xB < end;) {
			const uint32_t stop = (xB & ~15u) + 16;
			const uint32_t run = (stop < end ? stop : end) - xB;
			__builtin_memcpy(tiled + tgm_layout_offset(l, xB, y + row),
					 src + row * pitch + (xB - first), run);
			xB += run;
		}
	}
}

static inline void tgm_layout_read(const struct tgm_layout *l, const uint8_t *tiled, uint32_t x,
				   uint32_t y, uint32_t w, uint32_t h, uint8_t *dst, uint64_t pitch)
{
	const uint32_t first = x * l->bpp;
	const uint32_t end = (x + w) * l->bpp;
	for (uint32_t row = 0; row < h; row++) {
		for (uint32_t xB = first; xB < end;) {
			const uint32_t stop = (xB & ~15u) + 16;
			const uint32_t run = (stop < end ? stop : end) - xB;
			__builtin_memcpy(dst + row * pitch + (xB - first),
					 tiled + tgm_layout_offset(l, xB, y + row), run);
			xB += run;
		}
	}
}

// Provided by the GPU driver (weak in its callers): the layout of the IOSurface whose first page is `pa` when it is
// block-linear (1: *out filled); 0 when it is pitch or no GPU client imports it. Engines that read or write a surface
// (VIC, NVDEC, NVJPG, NVENC, the display) program a block-linear one with out->y_log2 GOBs a block and out->row_bytes,
// and order their work after the GPU's on it as they do for a pitch one. Thread context; never waits for the GPU.
int tegra_gm20b_surface_layout(uint64_t pa, struct tgm_layout *out);

// For a kernel CPU reader of a block-linear IOSurface, by the physical address of any byte of it (`pa`): the surface's bytes
// as a CPU lock sees them (its texels as rows bytes_per_row apart, from its first byte) in a scratch, the copy engine's,
// behind the GPU's writers. 1: *rows to hand to tegra_gm20b_surface_rows_done, *memory an IOMemoryDescriptor of the
// scratch valid until then, *offset where `pa`'s byte is in it. 0: no block-linear surface holds `pa`: read the memory
// itself. -ENOMEM: one does but no scratch could be had: read nothing (its pages hold texels). Sleeps on the copy's fence
// interrupt: thread context, no spinlock or interrupt filter.
int tegra_gm20b_surface_rows(uint64_t pa, void **rows, void **memory, uint64_t *offset);
void tegra_gm20b_surface_rows_done(void *rows);

#ifdef __cplusplus
}
#endif
