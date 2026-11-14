/* SPDX-License-Identifier: MIT */
/* A channel's RAMFC words written while the host may hold the channel (sched.c tgm_sched_ramfc,
 * test/gm20b_ramfc_order_test.cpp). The host keeps a loaded channel's RAMFC in the PBDMA and saves it back when it
 * switches the channel out, which can land after ENG_STATUS and PBDMA_CHANNEL have moved on, so a word written before
 * that save is overwritten by it. nvgpu's order (channel_gk20a_set_schedule_params): the channel disabled so the host
 * cannot load it again, a preempt whose completion is waited for (the save has landed), the words, the channel enabled.
 * EBUSY comes from the includer's errno. */
#ifndef TEGRA_GM20B_RAMFC_ORDER_H
#define TEGRA_GM20B_RAMFC_ORDER_H

struct tgm_ramfc_steps {
	void *arg;
	int (*own)(
		void *arg); /* the runlist's preempt lock without waiting: 1, or 0 when another holds it */
	void (*release)(void *arg);
	void (*disable)(void *arg);
	void (*enable)(void *arg);
	int (*preempt)(void *arg); /* a preempt, its completion waited for: 0, or its error */
	void (*write)(void *arg);
};

/* 0; -EBUSY when the lock is another's (nothing done); or the preempt's error (nothing written). */
static inline int tgm_ramfc_write_quiesced(const struct tgm_ramfc_steps *s)
{
	int ret = 0;

	if (!s->own(s->arg))
		return -EBUSY;
	s->disable(s->arg);
	ret = s->preempt(s->arg);
	if (!ret)
		s->write(s->arg);
	s->enable(s->arg);
	s->release(s->arg);
	return ret;
}

#endif
