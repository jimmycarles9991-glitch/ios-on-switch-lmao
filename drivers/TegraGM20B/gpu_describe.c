// SPDX-License-Identifier: MIT
// The lines a hang, a context-switch timeout and a stuck kernel channel print: the channel's state and the engine's.
#include "gpu_internal.h"

/* One line of what the GPU is doing about a channel: owner, hardware channel id, error, completed/submitted fence and
 * how long ago the last one was kicked; GPFIFO GET/PUT as the host sees them (USERD 0x88/0x8c) and the PUT the kernel
 * wrote; PGRAPH_STATUS (0x400700, bit 0 busy), PFIFO engine 0 (GR) status (0x2640); PBDMA 0: status (0x3080, bits 13-15
 * busy, gk104_runq_idle), the channel it serves (0x40120) and the method/data it is on (0x400c0/0x400c4: subchannel
 * 18:16, method 13:2, as gf100_runq_intr decodes them; a NV906F semaphore method there means a semaphore acquire that is
 * not satisfied); PMC_INTR_0; and the memory controller's last refused access (MC_INTSTATUS, MC_ERR_STATUS, MC_ERR_ADR). */
void tgm_chan_describe(struct tgm_chan *chan, char *buf, unsigned int len)
{
	struct nvkm_device *device = tgm_gpu.device;
	volatile u32 *userd = chan->user.map.ptr;
	const u32 eng = nvkm_rd32(device, 0x002640);
	u32 mc_int = 0;
	u32 mc_err = 0;
	u32 mc_adr = 0;
	char engine[48];
	char note[160];

	tgm_hang_engine(eng, engine, sizeof(engine));
	tgm_hang_note(&chan->hang, tgm_chan_completed(chan) + 1, chan->submitted,
		      chan->chan.gpfifo.map.ptr, chan->chan.gpfifo.max, chan->ring_va,
		      CHAN_PUSH_BYTES, chan->vm->connection, note, sizeof(note));
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	scnprintf(
		buf, len,
		"%s ch %u err %u fence %u/%u kicked %llu ms ago%s, gp %u/%u (put %u), PGRAPH %08x "
		"ENG0 %08x (%s), "
		"PBDMA0 %08x ch %u mthd %08x data %08x, PMC %08x, MC %08x err %08x at %08x",
		chan->owner[0] ? chan->owner : "?", chan->chid, chan->error,
		tgm_chan_completed(chan), chan->submitted,
		chan->submit_ns ? (nvxnu_nanotime() - chan->submit_ns) / 1000000 : 0ULL, note,
		userd ? userd[0x88 / 4] : 0, userd ? userd[0x8c / 4] : 0, chan->chan.gpfifo.cur,
		nvkm_rd32(device, 0x400700), eng, engine, nvkm_rd32(device, 0x003080),
		nvkm_rd32(device, 0x040120) & 0xfff, nvkm_rd32(device, 0x0400c0),
		nvkm_rd32(device, 0x0400c4), nvkm_rd32(device, 0x000100), mc_int, mc_err, mc_adr);
}

void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	char state[TGM_HANG_LINE];
	char order[64] = "";
	const struct tgm_order *w = &chan->waited;
	tgm_chan_describe(chan, state, sizeof(state));
	/* still waiting for another channel of its task (tgm_chan_order) */
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0) {
		scnprintf(order, sizeof(order), ", order wait on chan %llu fence %u (at %u)", w->id,
			  w->seqno, READ_ONCE(*w->cpu));
	}
	nvxnu_panic("tegra-gm20b: %s: fence %u still pending after %llu ms and no GPU error "
		    "reported: %s%s",
		    who, seqno, waited_ns / 1000000, state, order);
}

void tgm_engine_state(char *buf, unsigned int len)
{
	struct nvkm_device *device = tgm_gpu.device;
	struct tgm_engine_regs r = {};
	u32 i = 0;

	r.status1 = nvkm_rd32(device, 0x400604);
	r.engine_status = nvkm_rd32(device, 0x40060c);
	for (i = 0; i < 3; i++)
		r.activity[i] = nvkm_rd32(device, 0x400380 + 4 * i);
	r.activity[3] = nvkm_rd32(device, 0x400390);
	r.go_idle = nvkm_rd32(device, 0x404194);
	r.mailbox0 = nvkm_rd32(device, 0x409800);
	r.fecs_fe0 = nvkm_rd32(device, 0x409c00);
	r.fecs_1 = nvkm_rd32(device, 0x409400);
	r.current_ctx = nvkm_rd32(device, 0x409b00);
	r.new_ctx = nvkm_rd32(device, 0x409b04);
	r.trapped_addr = nvkm_rd32(device, 0x400704);
	r.trapped_data = nvkm_rd32(device, 0x400708);
	if (((r.trapped_addr >> 16) & 7) < 4)
		r.trapped_class = nvkm_rd32(device, 0x404200 + 4 * ((r.trapped_addr >> 16) & 7));
	r.gp_get = nvkm_rd32(device, 0x040014);
	r.gp_put = nvkm_rd32(device, 0x040000);
	r.get = nvkm_rd32(device, 0x040018);
	r.get_hi = nvkm_rd32(device, 0x04001c);
	r.put = nvkm_rd32(device, 0x04005c);
	r.put_hi = nvkm_rd32(device, 0x040060);
	r.method1 = nvkm_rd32(device, 0x0400c8);
	r.hdr_shadow = nvkm_rd32(device, 0x040118);
	tgm_engine_format(buf, len, &r);
}

static const char *chid_owner(int chid)
{
	const struct tgm_chan *chan = live_by(0, chid);

	return chan ? chan->owner : "-";
}

/* tgm_live_lock held: each engine's PFIFO_ENG_STATUS with the owners of the channels it holds and switches to, then each
 * PBDMA's status, channel and owner. The ctxsw lines print only ENG0 otherwise, and on GM20B the copy engine (ce2, engine
 * 1) shares runlist 0 and PBDMA0 with GR. */
void tgm_engines_line(char *buf, unsigned int len)
{
	struct nvkm_device *device = tgm_gpu.device;
	struct nvkm_runl *runl = NULL;
	struct nvkm_engn *engn = NULL;
	unsigned int at = 0;
	int i = 0;

	buf[0] = 0;
	nvkm_runl_foreach(runl, device->fifo)
	{
		/* nouveau's runlist walk takes each engine by container_of. */
		/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			const u32 eng = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			char decoded[48];

			tgm_hang_engine(eng, decoded, sizeof(decoded));
			at += scnprintf(buf + at, len - at, "%s%s %08x (%s; %s -> %s)",
					at ? ", " : "", engn->engine->subdev.name, eng, decoded,
					chid_owner((int)PFIFO_ENG_ID(eng)),
					chid_owner((int)PFIFO_ENG_NEXT_ID(eng)));
		}
		for (i = 0; i < runl->runq_nr; i++) {
			const u32 chid =
				PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[i]->id)));

			at += scnprintf(buf + at, len - at, ", PBDMA%d %08x ch %u %s",
					runl->runq[i]->id,
					nvkm_rd32(device, PBDMA_STATUS(runl->runq[i]->id)), chid,
					chid_owner((int)chid));
		}
	}
}
