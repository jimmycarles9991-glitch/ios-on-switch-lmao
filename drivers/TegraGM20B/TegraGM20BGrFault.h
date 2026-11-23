/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_GR_FAULT_H
#define TEGRA_GM20B_GR_FAULT_H

#include <engine/fifo/chan.h>
#include <engine/fifo/runl.h>

/* nouveau's gf100_gr_intr, for each channel error it reports (ILLEGAL_MTHD, ILLEGAL_CLASS, DATA_ERROR, a TRAP other than
 * TEX alone), before it acknowledges it: GR holds the faulting method until then (gpu_fault.c). */
void tgm_gr_fault(int chid);

/* chan: the channel on FECS's current context (0x409b00) at the interrupt, the one nvgpu's gk20a_gr_isr recovers
 * (gr_fecs_current_ctx_r). Stopped, and its group recovered: nvkm_runl_rc preempts it under runl->mutex, as every
 * PFIFO_PREEMPT write is made. GR's FIFO access is the interrupt's to turn back on at its end, as nouveau's and nvgpu's
 * handlers do. Held off until nvkm_runl_rc was done (C24, C32), GR sat in a busy context load through the whole of its
 * preempt wait (2000 PTIMER ms, 3.26 s), and the channels waiting behind it hit the context-switch timeout. */
static inline void switch_gpu_gr_fault_stop(struct nvkm_chan *chan, int chid)
{
	tgm_gr_fault(chid);
	if (!chan)
		return;
	nvkm_chan_error(chan, false);
	nvkm_runl_rc_cgrp(chan->cgrp);
}

#endif /* TEGRA_GM20B_GR_FAULT_H */
