// SPDX-License-Identifier: MIT
// Debug kernels: each GR context's state as GR holds it, once per context.
#include "gpu_internal.h"

#if TGM_STATS
/* FECS_CURRENT_CTX: VALID bit 31, the resident context's instance block in 27:0 (4 KiB units), as gf100_gr_intr reads it */
#define FECS_CURRENT_CTX 0x409b00
#define FECS_CURRENT_CTX_VALID 0x80000000u
#define PD_NUM_TPC_PER_GPC(i) (0x406028 + (i) * 4)
#define DS_NUM_TPC_PER_GPC 0x405870
#define GR_CONTEXTS 64

/* The channels' contexts by instance block, for the fence interrupt pass, which runs under nvkm's event lock where
 * neither the runlist's locks nor tgm_live_lock can be taken. inst 0: a free slot. */
static struct {
	u64 inst;
	u32 chid;
	int logged;
	char owner[17];
} contexts[GR_CONTEXTS];

static u64 chan_inst_addr(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;
	u64 addr = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			addr = nchan->inst->addr;
			nvkm_chan_put(&nchan, flags);
			break;
		}
	}
	return addr;
}

/* chan_new and chan_free, under tgm_gpu.lock */
void tgm_stats_gr_bind(struct tgm_chan *chan)
{
	const u64 inst = chan_inst_addr(chan->chid);
	unsigned int i = 0;

	for (i = 0; inst && i < GR_CONTEXTS; i++) {
		if (__atomic_load_n(&contexts[i].inst, __ATOMIC_ACQUIRE))
			continue;
		contexts[i].chid = chan->chid;
		contexts[i].logged = 0;
		strscpy(contexts[i].owner, chan->owner, sizeof(contexts[i].owner));
		__atomic_store_n(&contexts[i].inst, inst, __ATOMIC_RELEASE);
		chan->gr_slot = i + 1;
		return;
	}
}

void tgm_stats_gr_unbind(struct tgm_chan *chan)
{
	if (chan->gr_slot)
		__atomic_store_n(&contexts[chan->gr_slot - 1].inst, 0, __ATOMIC_RELEASE);
	chan->gr_slot = 0;
}

/* At every fence interrupt, whichever channel's: PD's and DS's TPC counts per GPC are context state, their registers
 * read the resident context's, so the context FECS holds is logged the first time it is seen, with FECS_CURRENT_CTX
 * the same before and after the reads. */
void tgm_stats_gr_context(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	const u32 ctx = nvkm_rd32(device, FECS_CURRENT_CTX);
	const u64 inst = (u64)(ctx & 0x0fffffff) << 12;
	char owner[17];
	unsigned int i = 0;
	u32 pd[4];
	u32 ds = 0;
	u32 chid = 0;
	unsigned int r = 0;

	if (!(ctx & FECS_CURRENT_CTX_VALID))
		return;
	for (i = 0; i < GR_CONTEXTS; i++) {
		if (__atomic_load_n(&contexts[i].inst, __ATOMIC_ACQUIRE) == inst &&
		    !contexts[i].logged)
			break;
	}
	if (i == GR_CONTEXTS)
		return;
	chid = contexts[i].chid;
	memcpy(owner, contexts[i].owner, sizeof(owner));
	owner[sizeof(owner) - 1] = 0;
	for (r = 0; r < ARRAY_SIZE(pd); r++)
		pd[r] = nvkm_rd32(device, PD_NUM_TPC_PER_GPC(r));
	ds = nvkm_rd32(device, DS_NUM_TPC_PER_GPC);
	if (nvkm_rd32(device, FECS_CURRENT_CTX) != ctx ||
	    __atomic_load_n(&contexts[i].inst, __ATOMIC_ACQUIRE) != inst)
		return;
	contexts[i].logged = 1;
	nvxnu_stats("tegra-gm20b: GR context of %s ch %u: PD num_tpc_per_gpc %08x %08x %08x %08x, "
		    "DS %08x; GPU rail-ups so far %u",
		    owner, chid, pd[0], pd[1], pd[2], pd[3], ds,
		    tgm_rail_ups.display + tgm_rail_ups.demand);
}
#endif
