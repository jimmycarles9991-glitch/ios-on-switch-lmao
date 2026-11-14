// SPDX-License-Identifier: MIT
// sched.c as the kernel builds it, its #include lines left out for gm20b_sched_stubs.h's stand-ins.
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BHost.h"
#include "drivers/TegraGM20B/TegraGM20BRecover.h"
#include "drivers/TegraGM20B/TegraGM20BSched.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"
#include "drivers/TegraGM20B/TegraGM20BRamfcOrder.h"
#include "drivers/TegraGM20B/test/sched_source.h"

__attribute__((weak)) void nvkm_chan_block(struct nvkm_chan *chan)
{
	(void)chan;
}

__attribute__((weak)) void nvkm_chan_allow(struct nvkm_chan *chan)
{
	(void)chan;
}

__attribute__((weak)) int nvkm_chan_preempt_locked(struct nvkm_chan *chan, bool wait)
{
	(void)chan;
	(void)wait;
	return 0;
}

__attribute__((weak)) struct nvkm_chan *nvkm_runl_chan_get_chid(struct nvkm_runl *runl, int id,
								unsigned long *flags)
{
	(void)runl;
	(void)id;
	(void)flags;
	return NULL;
}

__attribute__((weak)) void nvkm_chan_put(struct nvkm_chan **chan, unsigned long flags)
{
	(void)flags;
	*chan = NULL;
}
