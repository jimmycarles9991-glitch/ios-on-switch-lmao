/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GM20B_GR_FAULT_H
#define TEGRA_GM20B_GR_FAULT_H

#include <core/device.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/runl.h>

/* nouveau's gf100_gr_intr, for each channel error it reports (ILLEGAL_MTHD, ILLEGAL_CLASS, DATA_ERROR, a TRAP other than
 * TEX alone), before it acknowledges it: GR holds the faulting method until then (gpu_fault.c). */
void tgm_gr_fault(int chid);

static inline void switch_gpu_gr_fault_stop(struct nvkm_device *device, bool *held,
					    struct nvkm_chan *chan, int chid)
{
	if (!chan || atomic_read(&chan->errored) == 2) {
		tgm_gr_fault(chid);
		return;
	}
	*held = true;
	nvkm_mask(device, 0x400500, 0x00010001, 0);
	tgm_gr_fault(chid);
	nvkm_chan_error(chan, false);
	nvkm_runl_rc_cgrp(chan->cgrp);
}

static inline void switch_gpu_gr_fault_access(struct nvkm_device *device, bool held)
{
	nvkm_wr32(device, 0x400500, held ? 0 : 0x00010001);
}

static inline void switch_gpu_gr_fault_release(struct nvkm_device *device, bool *held,
					       const atomic_t *pending)
{
	if (*held && !atomic_read(pending)) {
		*held = false;
		switch_gpu_gr_fault_access(device, false);
	}
}

void switch_gpu_gr_fault_recovered(struct nvkm_runl *runl);

#endif /* TEGRA_GM20B_GR_FAULT_H */
