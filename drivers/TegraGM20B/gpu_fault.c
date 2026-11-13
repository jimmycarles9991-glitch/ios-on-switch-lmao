// SPDX-License-Identifier: MIT
// The kill record clients read (TegraGM20BABI.h TegraGM20BAbiFence), and a GR error charged to the job that raised it.
#include "gpu_internal.h"
#include "TegraGM20BGrFault.h"

/* gm107_fifo_chid_nr, GM20B's FIFO */
#define CHIDS 2048

/* The live channels by hardware id, for nvkm's GR interrupt, which runs under its channel group's spinlock and can take
 * neither tgm_live_lock nor the runlist's locks. */
static struct tgm_chan *by_chid[CHIDS];

void tgm_fault_publish(struct tgm_chan *chan)
{
	if (chan->chid < CHIDS)
		__atomic_store_n(&by_chid[chan->chid], chan, __ATOMIC_RELEASE);
}

/* runl->mutex held (chan_free's nvkm destructor waits for it, so a channel read here is not freed under the caller): 1 if
 * hardware channel chid is a live client's with nothing pending */
int tgm_chid_unowned(int chid)
{
	struct tgm_chan *chan = NULL;

	if (chid < 0 || chid >= CHIDS)
		return 0;
	chan = __atomic_load_n(&by_chid[chid], __ATOMIC_ACQUIRE);
	return chan && !chan->kernel &&
	       tgm_switch_unowned(READ_ONCE(chan->error) != 0, tgm_chan_completed(chan),
				  READ_ONCE(chan->submitted));
}

/* chan_free, before nvkm's channel goes: nvkm's GR interrupt holds that channel's group lock while it calls
 * tgm_gr_fault, and nvkm takes the same lock to drop the channel, so a tgm_gr_fault that found this one is done
 * before chan_free frees it. */
void tgm_fault_unpublish(struct tgm_chan *chan)
{
	if (chan->chid < CHIDS && __atomic_load_n(&by_chid[chan->chid], __ATOMIC_ACQUIRE) == chan)
		__atomic_store_n(&by_chid[chan->chid], NULL, __ATOMIC_RELEASE);
}

void tgm_fence_kill_record(volatile u32 *fence_cpu, u32 first, u32 last, u32 error)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->killed_first, first);
	WRITE_ONCE(fence->killed_last, last);
	__asm__ volatile("dsb sy" ::: "memory");
	WRITE_ONCE(fence->error, error);
	__asm__ volatile("dsb sy" ::: "memory");
}

void tgm_fence_kill_clear(volatile uint32_t *fence_cpu)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->error, 0);
	WRITE_ONCE(fence->killed_first, 0);
	WRITE_ONCE(fence->killed_last, 0);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* GR holds the faulting method until nvkm acknowledges the error, so neither the rest of that job nor the wait for idle
 * before its fence release can run: the channel's first seqno not complete is the job that raised it. Once nvkm lets GR
 * go on, that job can finish and release its fence before the kill (tgm_chan_killed) comes, so the record names it now:
 * a client that sees its fence pass reads it failed, and the kill keeps it as the first failed seqno. A kernel
 * channel's error is fatal anyway. */
void tgm_gr_fault(int chid)
{
	struct tgm_chan *chan = NULL;

	if (chid < 0 || chid >= CHIDS)
		return;
	chan = __atomic_load_n(&by_chid[chid], __ATOMIC_ACQUIRE);
	if (!chan || chan->kernel || chan->faulted)
		return;
	chan->fault_seqno = tgm_chan_completed(chan) + 1;
	chan->faulted = 1;
	tgm_fence_kill_record(chan->fence_cpu, chan->fault_seqno, READ_ONCE(chan->submitted),
			      TGM_KILL_GR_ERROR);
}
