// SPDX-License-Identifier: MIT
// The copy engine's moves of a block-linear IOSurface (TegraGM20BLayout.h) between its own pages, which the GPU and the
// display read tiled, and a scratch of rows bytes_per_row apart (a CPU lock's view of it). One move is one push, which
// TegraGM20BRelayout.cpp submits on the kernel's copy channel: a semaphore acquire on each GPU writer it must follow (the
// host waits on the GPU, switching to other channels meanwhile), then its copies, each NON_PIPELINED (it starts once the
// one before wrote everything) with FLUSH_ENABLE:
//  - to rows: the scratch cleared (every byte a task may read of it: no earlier user's bytes), then the surface's texels
//    into it;
//  - to tiled: the scratch's rows into the surface's texels;
//  - tile in place: what the surface holds as rows (its contents before it became block-linear) into the scratch, then
//    back into the surface as texels;
//  - untile in place: the surface's texels into the scratch as rows, then back into the surface as rows (it is pitch
//    again: no GPU client imports it any more).
// Plain C, so the host test runs the same words through a copy-engine model.
#pragma once
#include <stdint.h>
#include "TegraGM20BFences.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_RELAYOUT_SUBC_COPY 4u
#define TGM_RELAYOUT_MAX_WAITS 8u
#define TGM_RELAYOUT_DWORDS (TGM_RELAYOUT_MAX_WAITS * 5u + 64u)

enum tgm_relayout_op {
	TGM_RELAYOUT_TO_ROWS,
	TGM_RELAYOUT_TO_TILED,
	TGM_RELAYOUT_TILE_IN_PLACE,
	TGM_RELAYOUT_UNTILE_IN_PLACE,
};

// A GPU writer's fence as the move's channel sees it: the GPU address of its 32-bit seqno and the seqno to wait for;
// and the writer's channel
struct tgm_relayout_wait {
	uint64_t va;
	uint32_t seqno;
	uint32_t uid;
};

// The surface's pages at surface_va (TGM_LAYOUT_KIND: its texels) and again at surface_rows_va (kind 0: what it held as
// rows before it was tiled), and the scratch at rows_va (kind 0), each from its first byte. Block-linear memory is read and
// written only through a TGM_LAYOUT_KIND mapping and rows only through a kind-0 one: each kind places a GOB's bytes in
// DRAM its own way.
struct tgm_relayout_move {
	uint64_t surface_va, surface_rows_va, rows_va;
	uint32_t bytes_per_row;
	enum tgm_relayout_op op;
	uint64_t
		rows_bytes; /* the scratch's length (a multiple of 4096): what a move to rows clears */
};

static inline uint32_t tgm_relayout_header(uint32_t subc, uint32_t mthd, uint32_t count)
{
	return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}

// Zeros over `bytes` (a multiple of 4096) at va: lines of 4096 bytes, each 1024 four-byte components of constant A
static inline uint32_t tgm_relayout_clear(uint32_t *w, uint64_t va, uint64_t bytes)
{
	uint32_t i = 0;
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0700,
				     3);  // REMAP_CONST_A, CONST_B, COMPONENTS
	w[i++] = 0;
	w[i++] = 0;
	w[i++] =
		4u |
		3u << 16;  // DST_X CONST_A, COMPONENT_SIZE FOUR, one source and one destination component
	w[i++] =
		tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0408, 6);  // OFFSET_OUT .. LINE_COUNT
	w[i++] = (uint32_t)(va >> 32);
	w[i++] = (uint32_t)va;
	w[i++] = 4096;
	w[i++] = 4096;
	w[i++] = 1024;
	w[i++] = (uint32_t)(bytes / 4096);
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0300, 1);
	w[i++] = 2u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9 |
		 1u << 10;  // NON_PIPELINED, FLUSH, PITCH both, MULTI_LINE, REMAP
	return i;
}

// SEMAPHOREA..D, ACQ_GEQ with ACQUIRE_SWITCH (cl906f.h)
static inline uint32_t tgm_relayout_acquire(uint32_t *w, const struct tgm_relayout_wait *wait)
{
	w[0] = tgm_relayout_header(0, 0x0010, 4);
	w[1] = (uint32_t)(wait->va >> 32);
	w[2] = (uint32_t)wait->va;
	w[3] = wait->seqno;
	w[4] = 0x4u | 1u << 12;
	return 5;
}

// `src` to `dst`, the texels of `l` (a block-linear side with the layout's block height, its width the row stride in
// texels; a pitch side rows `pitch` apart) through SET_REMAP_COMPONENTS of the texel's components (at most 4 bytes each),
// so lengths count texels
static inline uint32_t tgm_relayout_copy(uint32_t *w, const struct tgm_layout *l, uint64_t src,
					 int src_tiled, uint64_t dst, int dst_tiled, uint32_t pitch)
{
	const uint32_t comp = l->bpp < 4 ? l->bpp : 4;
	const uint32_t n = l->bpp / comp;
	uint32_t launch = 2u | 1u << 2 | 1u << 9 |
			  1u << 10;  // NON_PIPELINED, FLUSH_ENABLE, MULTI_LINE, REMAP
	uint32_t i = 0;
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0708, 1);
	w[i++] =
		0 | 1u << 4 | 2u << 8 | 3u << 12 | (comp - 1) << 16 | (n - 1) << 20 | (n - 1) << 24;
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0400,
				     8);  // OFFSET_IN_UPPER .. LINE_COUNT
	w[i++] = (uint32_t)(src >> 32);
	w[i++] = (uint32_t)src;
	w[i++] = (uint32_t)(dst >> 32);
	w[i++] = (uint32_t)dst;
	w[i++] = src_tiled ? l->row_bytes : pitch;
	w[i++] = dst_tiled ? l->row_bytes : pitch;
	w[i++] = l->width;
	w[i++] = l->height;
	for (int side = 0; side < 2; side++) {
		if (!(side ? dst_tiled : src_tiled)) {
			launch |= side ? 1u << 8 : 1u << 7;  // DST or SRC_MEMORY_LAYOUT PITCH
			continue;
		}
		// SET_SRC_BLOCK_SIZE (0x728) or SET_DST_BLOCK_SIZE (0x70c), then WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
		w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, side ? 0x070c : 0x0728, 6);
		w[i++] = (uint32_t)l->y_log2 << 4 | 1u << 12;  // one GOB wide, Fermi's 8-row GOBs
		w[i++] = l->row_bytes / l->bpp;
		w[i++] = l->height;
		w[i++] = 1;
		w[i++] = 0;
		w[i++] = 0;
	}
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0300, 1);  // LAUNCH_DMA
	w[i++] = launch;
	return i;
}

// Appends to waits[nwaits..] (TGM_RELAYOUT_MAX_WAITS in all) the acquires a kernel copy on channel `self` makes on the
// running `writers`, writer i's fence at fence_va[i] in the copy's space (0: not in its fence view), by
// TegraGM20BFences.h tgm_writer_order, and returns the new count; *refused counts the writers it could not acquire on.
static inline uint32_t tgm_relayout_follow(uint32_t self, const struct tgm_relayout_writer *writers,
					   const uint64_t *fence_va, uint32_t nwriters,
					   struct tgm_relayout_wait *waits, uint32_t nwaits,
					   uint32_t *refused)
{
	for (uint32_t i = 0; i < nwriters; i++) {
		const int acquirable =
			(fence_va[i] != 0 && nwaits < TGM_RELAYOUT_MAX_WAITS) ? 1 : 0;
		switch (tgm_writer_order(self, writers[i].uid, acquirable)) {
		case TGM_WRITER_GPU_ACQUIRE:
			waits[nwaits].va = fence_va[i];
			waits[nwaits].seqno = writers[i].seqno;
			waits[nwaits++].uid = writers[i].uid;
			break;
		case TGM_WRITER_REFUSED:
			(*refused)++;
			break;
		case TGM_WRITER_IN_CHANNEL:
			break;
		}
	}
	return nwaits;
}

// tgm_relayout_follow over the writers still running, with their fences as `vm` maps them; a writer it refuses is one
// switch_refusal line. Thread context.
uint32_t tgm_relayout_waits(struct tgm_vm *vm, uint32_t self,
			    const struct tgm_relayout_writer *writers, uint32_t nwriters,
			    struct tgm_relayout_wait *waits, uint32_t nwaits);

// The whole push of one move, at most TGM_RELAYOUT_DWORDS words; `nwaits` at most TGM_RELAYOUT_MAX_WAITS
static inline uint32_t tgm_relayout_push(uint32_t *w, const struct tgm_layout *l,
					 const struct tgm_relayout_move *m,
					 const struct tgm_relayout_wait *waits, uint32_t nwaits)
{
	uint32_t i = 0;
	for (uint32_t k = 0; k < nwaits && k < TGM_RELAYOUT_MAX_WAITS; k++)
		i += tgm_relayout_acquire(w + i, &waits[k]);
	switch (m->op) {
	case TGM_RELAYOUT_TO_ROWS:
		i += tgm_relayout_clear(w + i, m->rows_va, m->rows_bytes);
		return i + tgm_relayout_copy(w + i, l, m->surface_va, 1, m->rows_va, 0,
					     m->bytes_per_row);
	case TGM_RELAYOUT_TO_TILED:
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_va, 1,
					     m->bytes_per_row);
	case TGM_RELAYOUT_TILE_IN_PLACE:
		i += tgm_relayout_copy(w + i, l, m->surface_rows_va, 0, m->rows_va, 0,
				       m->bytes_per_row);
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_va, 1,
					     m->bytes_per_row);
	case TGM_RELAYOUT_UNTILE_IN_PLACE:
		i += tgm_relayout_copy(w + i, l, m->surface_va, 1, m->rows_va, 0, m->bytes_per_row);
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_rows_va, 0,
					     m->bytes_per_row);
	}
	return i;
}

// tgm_relayout_by_cpu's moves over pages that `kva` gives a CPU address for: TO_ROWS clears the rows' pages first, as the
// copy engine's move does (a scratch may hold another surface's rows). The caller handles the cache lines.
static inline void tgm_relayout_cpu_bytes(const struct tgm_relayout_pages *p, uint64_t at,
					  uint8_t *bytes, uint32_t n, int store,
					  uint8_t *(*kva)(uint64_t pa))
{
	while (n) {
		const uint32_t room = 4096 - (uint32_t)(at & 4095);
		const uint32_t run = n < room ? n : room;
		uint8_t *cpu = kva(p->pas[at >> 12] & ~4095ULL) + (at & 4095);
		if (store) {
			__builtin_memcpy(cpu, bytes, run);
		} else {
			__builtin_memcpy(bytes, cpu, run);
		}
		at += run;
		bytes += run;
		n -= run;
	}
}

static inline void tgm_relayout_cpu(const struct tgm_relayout_pages *surface,
				    const struct tgm_relayout_pages *rows,
				    const struct tgm_layout *l, uint32_t bytes_per_row, int to_rows,
				    uint8_t *(*kva)(uint64_t pa))
{
	if (to_rows) {
		for (uint32_t i = 0; i < rows->npages; i++)
			__builtin_memset(kva(rows->pas[i] & ~4095ULL), 0, 4096);
	}
	const struct tgm_relayout_pages *from = to_rows ? surface : rows;
	const struct tgm_relayout_pages *to = to_rows ? rows : surface;
	const uint32_t end = l->width * l->bpp;
	uint8_t run[16];
	for (uint32_t y = 0; y < l->height; y++) {
		for (uint32_t xB = 0; xB < end; xB += 16) {
			const uint32_t n = end - xB < 16 ? end - xB : 16;
			const uint64_t texels = tgm_layout_offset(l, xB, y);
			const uint64_t line = (uint64_t)y * bytes_per_row + xB;
			tgm_relayout_cpu_bytes(from, to_rows ? texels : line, run, n, 0, kva);
			tgm_relayout_cpu_bytes(to, to_rows ? line : texels, run, n, 1, kva);
		}
	}
}

#ifdef __cplusplus
}
#endif
