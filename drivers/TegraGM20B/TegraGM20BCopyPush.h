// SPDX-License-Identifier: MIT
// The push of one plane copy (TegraGM20BCopy.h) on the kernel's copy channel: the GPU writers' acquires, a host1x syncpoint
// wait, the copy (rows to rows, a block-linear surface's texels to rows, or rows into one's texels), a syncpoint increment.
#pragma once
#include <stdint.h>
#include "TegraGM20BCopy.h"
#include "TegraGM20BRelayout.h"
#include "TegraGM20BSyncpt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_COPY_DWORDS                                                                  \
	(TGM_RELAYOUT_MAX_WAITS * TGM_RELAYOUT_ACQUIRE_DWORDS + TGM_SYNCPT_WAIT_DWORDS + \
	 TGM_COPY_RECT_DWORDS + TGM_SYNCPT_INCR_DWORDS)

struct tgm_copy_job {
	uint64_t src_va, dst_va; /* each plane's first byte */
	uint32_t src_pitch, dst_pitch, row_bytes, rows;
	/* a block-linear source or destination surface's layout (never both), else null */
	const struct tgm_layout *src_tiled, *dst_tiled;
	uint32_t wait_syncpt, wait_threshold, incr_syncpt; /* TEGRA_GM20B_CE_NO_SYNCPT: none */
};

/* At most TGM_COPY_DWORDS words; `nwaits` at most TGM_RELAYOUT_MAX_WAITS */
static inline uint32_t tgm_copy_push(uint32_t *w, const struct tgm_copy_job *j,
				     const struct tgm_relayout_wait *waits, uint32_t nwaits)
{
	uint32_t i = 0;
	for (uint32_t k = 0; k < nwaits && k < TGM_RELAYOUT_MAX_WAITS; k++)
		i += tgm_relayout_acquire(w + i, &waits[k]);
	if (j->wait_syncpt != TEGRA_GM20B_CE_NO_SYNCPT)
		i += tgm_syncpt_wait(w + i, j->wait_syncpt, j->wait_threshold);
	if (j->src_tiled) {
		i += tgm_relayout_copy(w + i, j->src_tiled, j->src_va, 1, j->dst_va, 0,
				       j->dst_pitch);
	} else if (j->dst_tiled) {
		i += tgm_relayout_copy(w + i, j->dst_tiled, j->src_va, 0, j->dst_va, 1,
				       j->src_pitch);
	} else {
		i += tgm_copy_rows(w + i, j->src_va, j->src_pitch, j->dst_va, j->dst_pitch,
				   j->row_bytes, j->rows);
	}
	if (j->incr_syncpt != TEGRA_GM20B_CE_NO_SYNCPT)
		i += tgm_syncpt_incr(w + i, j->incr_syncpt);
	return i;
}

#ifdef __cplusplus
}
#endif
