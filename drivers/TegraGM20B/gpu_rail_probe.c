// SPDX-License-Identifier: MIT
// What a rail-up left, for debug.switch_gpu_rail_cycle (TegraGM20BRailCycle.cpp) and HW Test gpu.rail_cycle, and the
// kernel channel job the debug probes run: debug kernels only.
#include "gpu_internal.h"

#if TGM_STATS
static u32 wake_coeff, wake_off_rate;

/* wake_clock, after the relock and before the idle clock's rate is put back (tgm_clock_ceiling_restore) or repaired
 * (tgm_idle_wake): the GPCPLL as the wake itself left it, off the idle clock's VCO or not at the PL of its maximum
 * rate. Under tgm_gpu.lock. */
void tgm_rail_wake_seen(u32 coeff)
{
	u32 max_pl = 0;

	WRITE_ONCE(wake_coeff, coeff);
	if (!tgm_idle.clk)
		return;
	nvxnu_mutex_lock(tgm_idle.clk);
	max_pl = tgm_idle.maximum_khz ? tgm_idle.vco_khz / (2 * tgm_idle.maximum_khz) : 0;
	if ((coeff & 0xffff) != tgm_idle.vco_coeff || ((coeff >> 16) & 0x3f) != max_pl)
		WRITE_ONCE(wake_off_rate, wake_off_rate + 1);
	nvxnu_mutex_unlock(tgm_idle.clk);
}

u32 tgm_rail_wakes_off_rate(void)
{
	return READ_ONCE(wake_off_rate);
}

/* One fence on the scrubber's kernel channel (the present's if there is none) through the host, PBDMA, semaphore
 * release and nonstall interrupt: wake_fence's job, with a late fence reported instead of judged hung. */
int tgm_probe_fence(u32 *chid, u64 *us)
{
	struct tgm_chan *chan = tgm_scrub.chan ? tgm_scrub.chan : tgm_present.chan;
	struct mutex *lock = tgm_scrub.chan ? &tgm_scrub.lock : &tgm_present.lock;
	u64 start = nvxnu_nanotime();
	u64 end = start + TGM_PRESENT_NS;
	u32 seqno = 0;
	int ret = 0;

	if (!chan || READ_ONCE(chan->error))
		return -ENODEV;
	*chid = chan->chid;
	tgm_gate_enter();
	mutex_lock(lock);
	ret = chan_push_wait(chan, CHAN_FENCE_DWORDS);
	if (!ret)
		seqno = chan_fence(chan);
	mutex_unlock(lock);
	while (!ret) {
		u32 seq = nvxnu_wake_seq();

		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			break;
		if (nvxnu_nanotime() >= end) {
			ret = -ETIMEDOUT;
		} else {
			nvxnu_idle(seq, end);
		}
	}
	gate_leave();
	*us = (nvxnu_nanotime() - start) / 1000;
	return ret;
}

static void gpcpll_look(struct nvkm_device *device, void *arg)
{
	struct tgm_rail_look *look = arg;

	look->coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	look->cfg = nvkm_rd32(device, GPCPLL_CFG_R);
	look->khz = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	look->pl = (look->coeff >> 16) & 0x3f;
	look->expect_pl = (look->coeff >> 16) & 0x3f;
	look->vco_ok = 1;
	if (tgm_idle.clk) {
		look->expect_pl = READ_ONCE(tgm_idle.low) ? tgm_idle.idle_pl : tgm_idle.run_pl;
		look->vco_ok = (look->coeff & 0xffff) == tgm_idle.vco_coeff;
	}
}

/* The GPU's first job since the ungate (tgm_probe_fence), then the GPCPLL against the idle clock's rate: enabled and
 * locked, on its VCO, at the PL the idle clock last set, and as the wake itself left it (tgm_rail_wake_seen). */
int tgm_rail_probe(struct tgm_rail_look *look)
{
	u64 us = 0;
	u32 chid = 0;
	int ret = 0;

	*look = (struct tgm_rail_look){0};
	look->fence = tgm_probe_fence(&chid, &us);
	look->fence_us = (u32)us;
	look->wake_coeff = READ_ONCE(wake_coeff);
	ret = tgm_gpu_held(gpcpll_look, look);
	if (ret)
		return ret;
	return !look->fence && (look->cfg & 0x20001) == 0x20001 && look->vco_ok &&
			       look->pl == look->expect_pl
		       ? 0
		       : -EIO;
}
#endif
