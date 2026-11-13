// SPDX-License-Identifier: MIT
// The kernel's copy-engine pushes, written by kits/SwitchGM20BEmit (TegraGM20BCopyEmit.c) into a caller's words. Only
// plain C here: nouveau's units (gpu_*.c) include it, and Mesa's drf.h must stay out of them.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* switch_gm20b_copy_rect's bound for one layer (SWITCH_GM20B_COPY_RECT_WORDS(1)) */
#define TGM_COPY_RECT_DWORDS 27u
#define TGM_FILL_SETUP_DWORDS 4u
#define TGM_FILL_PITCH_DWORDS 3u
#define TGM_FILL_LINES_DWORDS 7u

struct tgm_copy_side {
	uint64_t va;
	uint32_t pitch;     /* bytes between rows; a block-linear side's row stride */
	int32_t block_log2; /* GOBs per block in height, log2; negative: pitch-linear */
};

/* width x height four-byte texels from src to dst, each launch after the copy before it (NON_PIPELINED) */
uint32_t tgm_copy_texels(uint32_t *w, const struct tgm_copy_side *src,
			 const struct tgm_copy_side *dst, uint32_t width, uint32_t height);

/* `rows` lines of `row_bytes` bytes, pitch to pitch, no remap: lengths count bytes */
uint32_t tgm_copy_rows(uint32_t *w, uint64_t src, uint32_t src_pitch, uint64_t dst,
		       uint32_t dst_pitch, uint32_t row_bytes, uint32_t rows);

/* A fill's constant: every four-byte element written is `value` (SET_REMAP_CONST_A, no source) */
uint32_t tgm_fill_setup(uint32_t *w, uint32_t value);

uint32_t tgm_fill_pitch(uint32_t *w, uint32_t pitch);

/* `lines` lines of `line_bytes` at va, after tgm_fill_setup (and tgm_fill_pitch when lines > 1) */
uint32_t tgm_fill_lines(uint32_t *w, uint64_t va, uint32_t line_bytes, uint32_t lines,
			int multi_line);

#ifdef __cplusplus
}
#endif
