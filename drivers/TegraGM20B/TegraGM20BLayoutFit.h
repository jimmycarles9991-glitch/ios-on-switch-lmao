// SPDX-License-Identifier: MIT
// NIL's block-linear layout of an IOSurface's image (TegraGM20BLayout.h) lowered to fit the surface's allocation, the way
// NIL's Tiling::choose lowers y_log2 one GOB power at a time under max_tile_size_B (nil/tiling.rs). NIL picks blocks by
// height alone: a 1280x720 BGRA surface of 3,686,400 bytes would need 128-row blocks padded to 768 rows (3,932,160 bytes);
// 16-row blocks lie in its 720 rows exactly. Kept out of TegraGM20BLayout.h, which the raw stack key hashes.
#pragma once
#include "TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void tgm_layout_set_y_log2(struct tgm_layout *l, uint8_t y_log2)
{
	const uint32_t block_rows = 8u << y_log2;
	const uint64_t block = 512ULL << y_log2;
	l->y_log2 = y_log2;
	l->rows = (l->height + block_rows - 1) / block_rows * block_rows;
	l->align = block > 4096 ? block : 4096;
	l->bytes = ((uint64_t)l->row_bytes * l->rows + l->align - 1) / l->align * l->align;
}

// The tallest blocks, at most NIL's, whose layout fits `bytes`. 0: no tiled layout fits.
static inline int tgm_layout_tiled_within(uint32_t width, uint32_t height, uint32_t bpp,
					  uint64_t bytes, struct tgm_layout *out)
{
	if (!tgm_layout_tiled(width, height, bpp, out))
		return 0;
	while (out->bytes > bytes && out->y_log2 > 0)
		tgm_layout_set_y_log2(out, (uint8_t)(out->y_log2 - 1));
	return out->bytes <= bytes;
}

// Whether a reader that knows only the image's shape (NVK, which takes NIL's block height) reads `l` right
static inline int tgm_layout_nil_height(const struct tgm_layout *l)
{
	return l->y_log2 == tgm_layout_y_log2(l->height);
}

#ifdef __cplusplus
}
#endif
