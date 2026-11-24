// SPDX-License-Identifier: MIT
#include "gpu_internal.h"
#include "switch_gpu_exec_fixture.h"

#define SWITCH_EXEC_RING_BYTES (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + 0x2000)
#define SWITCH_EXEC_SEMA_OFFSET (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES)
#define SWITCH_EXEC_RING_VA 0x2000000000ull
#define SWITCH_EXEC_FENCES_VA 0x3000000000ull
/* nvif/chan906f.c's GET report: GPFIFO GET above the push buffer GET's 15 bits */
#define SWITCH_EXEC_GPPTR_SHIFT 15
#define SWITCH_EXEC_USERD_GP_PUT 0x8c
#define SWITCH_EXEC_CHANNELS 8

struct switch_exec_chan {
	struct tgm_chan tgm;
	struct tgm_vm vm;
	struct nvif_chan_func func;
	const struct nvif_chan_func *real;
	u32 userd[0x200 / 4];
	u32 push_reads, gpfifo_reads;
};

static struct switch_exec_hooks hooks;
static struct switch_exec_chan *chans[SWITCH_EXEC_CHANNELS];
static u32 chan_count;

static struct switch_exec_chan *switch_exec_of(struct nvif_chan *chan)
{
	return container_of(chan, struct switch_exec_chan, tgm.chan);
}

static u32 switch_exec_push_read_get(struct nvif_chan *nvif)
{
	struct switch_exec_chan *chan = switch_exec_of(nvif);

	if (hooks.read_get)
		hooks.read_get(hooks.context, chan, 0, ++chan->push_reads);
	return chan->real->push.read_get(nvif);
}

static u32 switch_exec_gpfifo_read_get(struct nvif_chan *nvif)
{
	struct switch_exec_chan *chan = switch_exec_of(nvif);

	if (hooks.read_get)
		hooks.read_get(hooks.context, chan, 1, ++chan->gpfifo_reads);
	return chan->real->gpfifo.read_get(nvif);
}

static void switch_exec_gpfifo_push(struct nvif_chan *nvif, bool main, u64 addr, u32 size,
				    bool no_prefetch)
{
	if (hooks.entry)
		hooks.entry(hooks.context, switch_exec_of(nvif), main, addr, size);
	switch_exec_of(nvif)->real->gpfifo.push(nvif, main, addr, size, no_prefetch);
}

/* gpu_exec.c's MMIO (SWITCH_MMIO_HOST_MODEL): plain host memory, and the doorbell is chan_doorbell's GP_PUT store to a
 * channel's USERD */
uint32_t switch_mmio_model_read32(uintptr_t address);
void switch_mmio_model_write32(uintptr_t address, uint32_t value);
uint16_t switch_mmio_model_read16(uintptr_t address);
void switch_mmio_model_write16(uintptr_t address, uint16_t value);
uint8_t switch_mmio_model_read8(uintptr_t address);
void switch_mmio_model_write8(uintptr_t address, uint8_t value);

uint32_t switch_mmio_model_read32(uintptr_t address)
{
	return *(volatile uint32_t *)address;
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
	for (u32 i = 0; i < chan_count; i++)
		if (hooks.doorbell &&
		    address == (uintptr_t)&chans[i]->userd[SWITCH_EXEC_USERD_GP_PUT / 4])
			hooks.doorbell(hooks.context, chans[i], value);
	*(volatile uint32_t *)address = value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
	return *(volatile uint16_t *)address;
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
	*(volatile uint16_t *)address = value;
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
	return *(volatile uint8_t *)address;
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
	*(volatile uint8_t *)address = value;
}

void switch_exec_reset(const struct switch_exec_hooks *with)
{
	while (chan_count) {
		struct switch_exec_chan *chan = chans[--chan_count];

		list_del(&chan->tgm.live);
		switch_exec_host_free(chan->tgm.ring);
		switch_exec_host_free(chan);
	}
	INIT_LIST_HEAD(&tgm_live_chans);
	if (!tgm_fence_pages.va)
		tgm_fence_pages.va = switch_exec_host_alloc(TGM_FENCE_BYTES);
	memset(tgm_fence_pages.va, 0, TGM_FENCE_BYTES);
	switch_exec_reset_driver();
	hooks = *with;
}

struct switch_exec_chan *switch_exec_chan_new(uint32_t uid, uint32_t fence_page)
{
	struct switch_exec_chan *chan = switch_exec_host_alloc(sizeof(*chan));
	struct tgm_chan *tgm = &chan->tgm;
	u8 *ring = switch_exec_host_alloc(SWITCH_EXEC_RING_BYTES);
	const size_t fence = (size_t)fence_page * TGM_FENCE_SLOT;

	if (chan_count == SWITCH_EXEC_CHANNELS || fence_page >= TGM_FENCE_USER_PAGES)
		nvxnu_panic("exec fixture: channel %u or fence page %u out of range", chan_count,
			    fence_page);
	chans[chan_count] = chan;
	tgm->vm = &chan->vm;
	chan->vm.fences_va = SWITCH_EXEC_FENCES_VA;
	tgm->ring = ring;
	tgm->ring_va = SWITCH_EXEC_RING_VA + (u64)chan_count * SWITCH_EXEC_RING_BYTES;
	tgm->id = uid;
	tgm->chid = chan_count++;
	tgm->fence_cpu = (volatile u32 *)((u8 *)tgm_fence_pages.va + fence);
	tgm->fence_gpu = SWITCH_EXEC_FENCES_VA + fence;
	INIT_LIST_HEAD(&tgm->live);
	nvif_chan906f_ctor(&tgm->chan, chan->userd, ring + CHAN_PUSH_BYTES, CHAN_GPFIFO_BYTES, ring,
			   tgm->ring_va, CHAN_PUSH_BYTES, ring + SWITCH_EXEC_SEMA_OFFSET,
			   tgm->ring_va + SWITCH_EXEC_SEMA_OFFSET);
	tgm->chan.push.seg = tgm->chan.push.cur;
	chan->real = tgm->chan.func;
	chan->func = *chan->real;
	chan->func.push.read_get = switch_exec_push_read_get;
	chan->func.gpfifo.read_get = switch_exec_gpfifo_read_get;
	chan->func.gpfifo.push = switch_exec_gpfifo_push;
	tgm->chan.func = &chan->func;
	return chan;
}

struct tgm_chan *switch_exec_chan_tgm(struct switch_exec_chan *chan)
{
	return &chan->tgm;
}

void switch_exec_chan_high(struct switch_exec_chan *chan, int high)
{
	chan->tgm.high = high;
}

void switch_exec_chan_critical(struct switch_exec_chan *chan, int critical)
{
	chan->tgm.critical = critical;
}

void switch_exec_chan_share_vm(struct switch_exec_chan *chan, struct switch_exec_chan *with)
{
	chan->tgm.vm = with->tgm.vm;
}

void switch_exec_chan_kernel(struct switch_exec_chan *chan, int kernel)
{
	chan->tgm.kernel = kernel;
}

void switch_exec_chan_dirty_tlb(struct switch_exec_chan *chan)
{
	tgm_tlb_mapped(&chan->vm.tlb);
}

void switch_exec_chan_enable_visible(struct switch_exec_chan *chan)
{
	chan->tgm.visible_cpu = chan->tgm.fence_cpu + 1;
	chan->tgm.visible_gpu = chan->tgm.fence_gpu + 4;
}

void switch_exec_set_low_busy(uint32_t chid, int busy)
{
	switch_gpu_busy_open(&tgm_busy, chid, 0);
	switch_gpu_busy_level(&tgm_busy, chid, 1);
	if (busy)
		switch_gpu_busy_kick(&tgm_busy, chid, 1);
	else
		switch_gpu_busy_done(&tgm_busy, chid, 0);
}

int switch_exec_chan_job_waits(struct switch_exec_chan *chan, uint32_t seqno)
{
	struct switch_gpu_kicked_job job = {0};

	if (!switch_gpu_jobs_find(&chan->tgm.jobs, seqno, &job))
		return -1;
	return job.waits;
}

void switch_exec_chan_count_wfis(struct switch_exec_chan *chan, uint32_t from_dword,
				 uint32_t to_dword, struct switch_exec_wfi_counts *out)
{
	const u32 *words = chan->tgm.chan.push.mem.object.map.ptr;
	const u64 sema_va = chan->tgm.ring_va + SWITCH_EXEC_SEMA_OFFSET;
	const u64 stamps_base = chan->tgm.ring_va + CHAN_STAMPS;
	const u64 stamps_end = stamps_base + (u64)CHAN_NSTAMPS * sizeof(struct tgm_stamps);
	u32 sema_hi = 0;
	u32 sema_lo = 0;
	u32 i = from_dword;

	memset(out, 0, sizeof(*out));
	while (i < to_dword) {
		const u32 hdr = words[i++];
		const u32 sec = (hdr >> 29) & 7u;
		const u32 count = (hdr >> 16) & 0x1fffu;
		const u32 subc = (hdr >> 13) & 7u;
		const u32 mthd = (hdr & 0xfffu) << 2;
		u32 j;

		if (sec == NV906F_DMA_SEC_OP_IMMD_DATA_METHOD)
			continue;
		if (sec != NV906F_DMA_SEC_OP_INC_METHOD &&
		    sec != NV906F_DMA_SEC_OP_NON_INC_METHOD && sec != NV906F_DMA_SEC_OP_ONE_INC)
			continue;
		for (j = 0; j < count && i < to_dword; ++j) {
			const u32 data = words[i++];
			u32 cur = mthd;

			if (sec == NV906F_DMA_SEC_OP_INC_METHOD)
				cur += j * 4u;
			else if (sec == NV906F_DMA_SEC_OP_ONE_INC && j)
				cur += 4u;

			if (subc != PUSH906F_SUBC_NV906F)
				continue;
			if (cur == NVB06F_WFI) {
				out->method_wfis++;
			} else if (cur == NVB06F_FB_FLUSH) {
				out->fb_flushes++;
			} else if (cur == NVB06F_MEM_OP_D &&
				   data == NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY) {
				out->l2_flushes++;
			} else if (cur == NV906F_SEMAPHOREA) {
				sema_hi = data & 0xffu;
			} else if (cur == NV906F_SEMAPHOREB) {
				sema_lo = data;
			} else if (cur == NV906F_SEMAPHORED &&
				   (data & 0xfu) == NV906F_SEMAPHORED_OPERATION_RELEASE) {
				const u64 va = ((u64)sema_hi << 32) | sema_lo;
				const int dis = (data >> 20) & 1u;

				if (va == sema_va)
					continue;
				if (dis)
					out->release_wfi_dis++;
				else
					out->release_wfi_en++;
				if (va >= stamps_base && va < stamps_end) {
					const u64 off =
						(va - stamps_base) % sizeof(struct tgm_stamps);
					if (off == offsetof(struct tgm_stamps, start))
						dis ? out->start_stamps_dis++
						    : out->start_stamps_en++;
					else if (off == offsetof(struct tgm_stamps, end))
						dis ? out->end_stamps_dis++ : out->end_stamps_en++;
				} else if (chan->tgm.visible_cpu && va == chan->tgm.visible_gpu) {
					dis ? out->visible_wfi_dis++ : out->visible_wfi_en++;
				} else if (va == chan->tgm.fence_gpu) {
					dis ? out->fence_wfi_dis++ : out->fence_wfi_en++;
				} else {
					dis ? out->extra_wfi_dis++ : out->extra_wfi_en++;
				}
			}
		}
	}
}

void switch_exec_chan_complete_stamp(struct switch_exec_chan *chan, uint32_t seqno,
				     uint64_t start_pt, uint64_t end_pt)
{
	volatile struct tgm_stamps *st = chan_stamps(&chan->tgm, seqno);

	st->start.payload = seqno;
	st->start.ns = start_pt;
	st->end.payload = seqno;
	st->end.ns = end_pt;
	WRITE_ONCE(*chan->tgm.fence_cpu, seqno);
#if TGM_STATS
	stats_fences(&chan->tgm);
#endif
}

void switch_exec_chan_live(struct switch_exec_chan *chan, int live)
{
	mutex_lock(&tgm_live_lock);
	list_del_init(&chan->tgm.live);
	if (live)
		list_add_tail(&chan->tgm.live, &tgm_live_chans);
	mutex_unlock(&tgm_live_lock);
}

void switch_exec_chan_set_get(struct switch_exec_chan *chan, uint32_t push_get, uint32_t gpfifo_get)
{
	volatile u32 *sema = (volatile u32 *)((u8 *)chan->tgm.ring + SWITCH_EXEC_SEMA_OFFSET);

	*sema = (gpfifo_get << SWITCH_EXEC_GPPTR_SHIFT) | push_get;
}

void switch_exec_chan_place(struct switch_exec_chan *chan, uint32_t push_dwords,
			    uint32_t gpfifo_cur, int32_t gpfifo_free)
{
	struct nvif_chan *nvif = &chan->tgm.chan;
	u32 *at = (u32 *)nvif->push.mem.object.map.ptr + push_dwords;

	nvif->push.bgn = nvif->push.cur = nvif->push.end = nvif->push.seg = at;
	nvif->gpfifo.cur = gpfifo_cur;
	nvif->gpfifo.free = gpfifo_free;
}

void switch_exec_chan_read(struct switch_exec_chan *chan, struct switch_exec_chan_pointers *out)
{
	struct nvif_chan *nvif = &chan->tgm.chan;
	const u32 *map = nvif->push.mem.object.map.ptr;

	out->gpfifo_cur = nvif->gpfifo.cur;
	out->gpfifo_free = nvif->gpfifo.free;
	out->gp_put = chan->userd[SWITCH_EXEC_USERD_GP_PUT / 4];
	out->submitted = chan->tgm.submitted;
	out->push_bgn = nvif->push.bgn - map;
	out->push_cur = nvif->push.cur - map;
	out->push_end = nvif->push.end - map;
	out->push_seg = nvif->push.seg - map;
	out->push_max = nvif->push.hw.max;
	out->defer_kick = nvif->defer_kick;
}

static uint32_t switch_exec_acquires(struct switch_exec_chan *waiter, uint64_t va, uint32_t value)
{
	const u32 *words = waiter->tgm.chan.push.mem.object.map.ptr;
	/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's NVDEF computes in int */
	const u32 acquire = NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
			    NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED);
	u32 found = 0;
	u32 i;

	for (i = 0; i + 4 <= CHAN_PUSH_BYTES / 4; ++i)
		found += words[i] == upper_32_bits(va) && words[i + 1] == lower_32_bits(va) &&
			 words[i + 2] == value && words[i + 3] == acquire;
	return found;
}

uint32_t switch_exec_chan_acquires(struct switch_exec_chan *waiter,
				   struct switch_exec_chan *producer, uint32_t value)
{
	return switch_exec_acquires(waiter,
				    waiter->vm.fences_va + ((uintptr_t)producer->tgm.fence_cpu -
							    (uintptr_t)tgm_fence_pages.va),
				    value);
}

uint32_t switch_exec_chan_word_acquires(struct switch_exec_chan *waiter, uint32_t pool,
					uint32_t value)
{
	return switch_exec_acquires(waiter, waiter->vm.fences_va + pool, value);
}

uint32_t switch_exec_chan_syncpt_waits(struct switch_exec_chan *waiter, uint32_t id, uint32_t value)
{
	const u32 *words = waiter->tgm.chan.push.mem.object.map.ptr;
	u32 expected[TGM_SYNCPT_WAIT_DWORDS];
	u32 found = 0, i;

	tgm_syncpt_wait(expected, id, value);
	for (i = 0; i + TGM_SYNCPT_WAIT_DWORDS <= CHAN_PUSH_BYTES / 4; ++i)
		found += !memcmp(words + i, expected, sizeof(expected));
	return found;
}
